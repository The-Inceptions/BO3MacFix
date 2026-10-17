// Tests sdl_displays.c against a fake SDL device: the launch reorder, publishing a new display list while another
// thread reads it, the size of new lists, and the mode table rebuild. Nothing here touches the game or the displays.
#include <malloc/malloc.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../source/sdl_displays.c"

uint64_t game_base_address = 0;

static uint32_t launch_target = 0;
uint32_t display_follow_launch_target(void) {
    return launch_target;
}

static int engine_mode = -1;
int engine_display_fullscreen_mode(void) {
    return engine_mode;
}

// ASL's original mode table builder, as the hook calls it
static int builder_calls = 0;
static uint32_t builder_saw_first = 0;
static int builder_saw_empty_table = 0;
static char *new_modes[3];
static void original_builder(void) {
    builder_calls++;
    builder_saw_first = sdl_displays_id(0);
    char **table = (char **)(game_base_address + ADDR_ASL_ModeTable);
    builder_saw_empty_table = table[1] == table[0];
    if (table[1] < table[2]) { // push_back into the existing storage, as ASL does when there's room
        memcpy(table[1], new_modes, sizeof(new_modes));
        table[1] += 0x18;
    }
}

int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)address;
    (void)fake_func;
    *out_origin_func = (void *)original_builder;
    return 0;
}

void sdl_cocoa_init(void) {}
double sdl_cocoa_screen(uint32_t display, char *name, size_t n) {
    snprintf(name, n, "display %u", display);
    return 2.0;
}
void sdl_cocoa_move_to_screen(void *nswindow, uint32_t display) {
    (void)nswindow;
    (void)display;
}
uint32_t sdl_cocoa_window_display(void *nswindow) {
    (void)nswindow;
    return 0;
}
void sdl_cocoa_describe(void *nswindow, char *buf, size_t n) {
    (void)nswindow;
    snprintf(buf, n, "frame=0,0,0,0 screen=0,0,0,0");
}

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); failures++; } } while (0)

static char *device;

static void set_displays(SDLDisplay *displays, int count) {
    *(SDLDisplay **)(device + DEVICE_DISPLAYS) = displays;
    *(int *)(device + DEVICE_NUM_DISPLAYS) = count;
}

static SDLDisplay *make_list(const uint32_t *ids, int count) {
    SDLDisplay *displays = calloc(count, sizeof(SDLDisplay)); // SDL's own list has no room to spare
    for (int i = 0; i < count; i++) {
        displays[i].driverdata = malloc(sizeof(CGDirectDisplayID));
        *displays[i].driverdata = ids[i];
        displays[i].desktop_mode.w = 1000 + (int)ids[i];
    }
    return displays;
}

static void test_launch_reorder(void) {
    static const uint32_t ids[] = { 1, 5, 9 };
    SDLDisplay *displays = make_list(ids, 3);
    set_displays(displays, 3);
    sdl_displays_init();

    launch_target = 9;
    fake_ASL_BuildModeTable();
    CHECK(builder_calls == 1);
    CHECK(builder_saw_first == 9);
    CHECK(sdl_displays_id(0) == 9 && sdl_displays_id(1) == 1 && sdl_displays_id(2) == 5);
    CHECK(displays[0].desktop_mode.w == 1009); // the whole entry moved, not just its id

    launch_target = 5; // in the middle: the others keep their order
    fake_ASL_BuildModeTable();
    CHECK(sdl_displays_id(0) == 5 && sdl_displays_id(1) == 9 && sdl_displays_id(2) == 1);

    launch_target = 42; // not in the list
    fake_ASL_BuildModeTable();
    launch_target = 0; // keep SDL's order
    fake_ASL_BuildModeTable();
    CHECK(sdl_displays_id(0) == 5 && sdl_displays_id(1) == 9 && sdl_displays_id(2) == 1);
    CHECK(builder_calls == 4);
    CHECK(sdl_displays_count() == 3 && sdl_displays_id(3) == 0 && sdl_displays_id(-1) == 0);
}

static void test_capacity(void) {
    SDLDisplay *displays = new_display_array(3);
    CHECK(malloc_size(displays) >= MAX_DISPLAYS * sizeof(SDLDisplay));
    free(displays);
    displays = new_display_array(MAX_DISPLAYS + 4);
    CHECK(malloc_size(displays) >= (MAX_DISPLAYS + 4) * sizeof(SDLDisplay));
    free(displays);
}

// which of the device's fields publish() writes first: the device's page is read-only, and the first write that
// faults is recorded before the page is made writable again
static char *first_write = NULL;
static void write_fault(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)context;
    if (first_write == NULL)
        first_write = info->si_addr;
    mprotect(device, sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE);
}

static void test_publish_order(void) {
    SDLDisplay *new_list = new_display_array(2);
    struct sigaction action = { 0 }, previous_segv, previous_bus;
    action.sa_sigaction = write_fault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, &previous_segv);
    sigaction(SIGBUS, &action, &previous_bus);
    mprotect(device, sysconf(_SC_PAGESIZE), PROT_READ);
    publish(device, new_list, 5);
    sigaction(SIGSEGV, &previous_segv, NULL);
    sigaction(SIGBUS, &previous_bus, NULL);
    // readers load the count and then the array, so the array has to be there first
    CHECK(first_write == device + DEVICE_DISPLAYS);
    CHECK(sdl_displays_count() == 5 && *(SDLDisplay **)(device + DEVICE_DISPLAYS) == new_list);
}

static char *table_storage;
static void reset_table(void) {
    char **table = (char **)(game_base_address + ADDR_ASL_ModeTable);
    char *old_modes[3] = { (char *)0x1000, (char *)0x1018, (char *)0x1018 };
    memcpy(table_storage, old_modes, sizeof(old_modes));
    table[0] = table_storage;
    table[1] = table_storage + 0x18;
    table[2] = table_storage + 0x18;
}

static pthread_mutex_t *window_mutex(void) {
    return (pthread_mutex_t *)(game_base_address + ADDR_ASL_WindowMutex);
}

static void *hold_mutex(void *done) {
    pthread_mutex_lock(window_mutex());
    while (!*(volatile int *)done)
        usleep(1000);
    pthread_mutex_unlock(window_mutex());
    return NULL;
}

static int engine_result = -1;
static void *rebuild_on_engine_thread(void *unused) {
    (void)unused;
    engine_result = sdl_displays_rebuild_mode_table();
    return NULL;
}

static void test_rebuild_mode_table(void) {
    char **table = (char **)(game_base_address + ADDR_ASL_ModeTable);
    table_storage = calloc(1, 0x18);
    char *modes = calloc(4, 0x18);
    new_modes[0] = modes;
    new_modes[1] = modes + 4 * 0x18;
    new_modes[2] = modes + 4 * 0x18;

    reset_table();
    CHECK(sdl_displays_rebuild_mode_table() == 1);
    CHECK(builder_saw_empty_table);
    CHECK(table[0] == table_storage && table[1] == table_storage + 0x18 && table[2] == table_storage + 0x18); // same storage
    CHECK(((char **)table_storage)[0] == modes); // the entry is the new one

    // on the main thread, with another thread holding ASL's window mutex: busy, without waiting
    reset_table();
    int calls = builder_calls, done = 0;
    pthread_t holder;
    pthread_create(&holder, NULL, hold_mutex, &done);
    while (pthread_mutex_trylock(window_mutex()) == 0) { // until the holder has it
        pthread_mutex_unlock(window_mutex());
        usleep(1000);
    }
    CHECK(sdl_displays_rebuild_mode_table() == 0);
    CHECK(builder_calls == calls && table[1] == table_storage + 0x18);
    done = 1;
    pthread_join(holder, NULL);

    // on the engine thread, while the main thread holds it (as sdl_displays_refresh does): waits, then rebuilds
    reset_table();
    pthread_mutex_lock(window_mutex());
    pthread_t engine;
    pthread_create(&engine, NULL, rebuild_on_engine_thread, NULL);
    usleep(50000);
    CHECK(builder_calls == calls && engine_result == -1);
    pthread_mutex_unlock(window_mutex());
    pthread_join(engine, NULL);
    CHECK(engine_result == 1 && builder_calls == calls + 1);
}

static void test_window(void) {
    static uint8_t hidden[0xe0], shown[0xe0];
    *(char **)(device + DEVICE_WINDOWS) = (char *)hidden;
    *(char **)(hidden + WINDOW_NEXT) = (char *)shown;
    *(uint32_t *)(shown + WINDOW_FLAGS) = SDL_WINDOW_SHOWN | 0x1001; // desktop fullscreen
    CHECK(sdl_game_fullscreen_mode() == 1);

    engine_mode = 2;
    CHECK(sdl_game_fullscreen_mode() == 1); // SDL's flags win

    // without the engine's mode, a window covering its display is a Fullscreen Window
    engine_mode = -1;
    CGRect bounds = CGDisplayBounds(CGMainDisplayID());
    int *rect = (int *)(shown + WINDOW_RECT);
    *(uint32_t *)(shown + WINDOW_FLAGS) = SDL_WINDOW_SHOWN;
    rect[0] = (int)bounds.origin.x;
    rect[1] = (int)bounds.origin.y;
    rect[2] = (int)bounds.size.width;
    rect[3] = (int)bounds.size.height;
    CHECK(sdl_game_fullscreen_mode() == 2);
    engine_mode = 0;
    CHECK(sdl_game_fullscreen_mode() == 0); // the engine says windowed
    rect[2] -= 100;
    engine_mode = -1;
    CHECK(sdl_game_fullscreen_mode() == 0);
    engine_mode = 2;
    CHECK(sdl_game_fullscreen_mode() == 2); // moved to another screen at its old size, still a Fullscreen Window
    engine_mode = 1;
    CHECK(sdl_game_fullscreen_mode() == 0); // the engine's Fullscreen with SDL not fullscreen: alt-tabbed out
    *(uint32_t *)(shown + WINDOW_FLAGS) = 0;
    CHECK(sdl_game_fullscreen_mode() == 0); // nothing shown
    engine_mode = -1;
    *(char **)(device + DEVICE_WINDOWS) = NULL;
}

int main(void) {
    // the game's image, as far as the addresses used here go (pages are only made when they're touched)
    game_base_address = (uint64_t)mmap(NULL, ADDR_ASL_WindowMutex + 0x1000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    device = mmap(NULL, 0x540, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); // a page of its own
    *(char **)(game_base_address + ADDR_SDL_VideoDevice) = device;
    pthread_mutexattr_t recursive; // ASL's std::recursive_mutex
    pthread_mutexattr_init(&recursive);
    pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(window_mutex(), &recursive);

    test_launch_reorder();
    test_capacity();
    test_publish_order();
    test_rebuild_mode_table();
    test_window();
    printf("%s\n", failures == 0 ? "sdl_displays: all tests passed" : "sdl_displays: tests failed");
    return failures != 0;
}
