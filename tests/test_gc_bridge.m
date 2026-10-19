// Tests gc_bridge.m's player slots as the game's XInputGetState and XInputSetState calls see them: the "found" line,
// SDL taking and letting go of user indexes, bridged controllers moving, waiting and moving down, disconnects, when
// the game is asked to look for controllers, and which disconnects PauseOnDisconnect pauses for. The controllers are
// stand-ins (FakeController), and nothing here talks to the GameController daemon or a real controller.
#include <sys/mman.h>
#include "../source/gc_bridge.m"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            failures++;                                                 \
        }                                                               \
    } while (0)

// what a GCController answers here, with no gamepad, motion, light or haptics behind it
@interface FakeController : NSObject
@end
@implementation FakeController
- (id)extendedGamepad {
    return nil;
}
- (id)motion {
    return nil;
}
- (id)light {
    return nil;
}
- (id)haptics {
    return nil;
}
- (NSString *)vendorName {
    return @"Fake Controller";
}
- (void)setPlayerIndex:(NSInteger)index {
}
@end

// what gc_bridge.m links against
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    return 0;
}
const void *prepatch_original_code(const void *address, size_t size) {
    return address;
}
static int asked = 0;
static const char *asked_why = NULL;
void controllers_request_gamepad_scan(const char *why) {
    asked++;
    asked_why = why;
}
static int game_reads = -1; // the player the game's first gamepad reads
int controllers_game_user_index(void) {
    return game_reads;
}
static int lost = 0; // PauseOnDisconnect's requests
void notices_controller_lost(void) {
    lost++;
}

// Aspyr's own XInput functions: connected only where SDL has a controller
static uint32_t aspyr_get_state(uint32_t user_index, XINPUT_STATE *state) {
    return sdl_slot_in_use((int)user_index) ? ERROR_SUCCESS : 0x48F;
}
static uint32_t aspyr_set_state(uint32_t user_index, XINPUT_VIBRATION *vibration) {
    return sdl_slot_in_use((int)user_index) ? ERROR_SUCCESS : 0x48F;
}

static void set_sdl(int user_index, uint8_t *controller) {
    *(void **)(base_address + ADDR_ASL_XInputSlots + user_index * 24) = controller;
}

static uint32_t get(int user_index) {
    XINPUT_STATE state;
    return fake_ASL_XInputGetState((uint32_t)user_index, &state);
}

// a bridged controller connecting, the way controller_connected fills its entry
static bridged_pad *bridge(id controller, const char *kind) {
    os_unfair_lock_lock(&pads_lock);
    bridged_pad *pad = NULL;
    for (int i = 0; i < XUSER_MAX_COUNT && pad == NULL; i++)
        if (pads[i].controller == nil)
            pad = &pads[i];
    pad->kind = kind;
    pad->found = false;
    note_sdl_slots();
    pad->user_index = first_free_user_index();
    pad->controller = [controller retain];
    os_unfair_lock_unlock(&pads_lock);
    return pad;
}

static void drain_input_queue(void) {
    dispatch_sync(input_queue, ^{
    });
}

static void disconnect_pad(id controller) {
    if (@available(macOS 11.3, *))
        controller_disconnected((GCController *)controller);
}

// runs what's waiting on the main queue: PauseOnDisconnect's check (with lost_grace at 0) and player index updates
static void run_main_queue(void) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    base_address = (uint64_t)mmap(NULL, ADDR_ASL_XInputSlots + 0x1000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    *(uint8_t *)(base_address + ADDR_ASL_XInputEnabled) = 1;
    orig_ASL_XInputGetState = aspyr_get_state;
    orig_ASL_XInputSetState = aspyr_set_state;
    enabled = true;
    input_queue = dispatch_queue_create("test input", DISPATCH_QUEUE_SERIAL);

    // SDL_GameControllers, with the GUID's vendor at +0x10 and product at +0x14
    static uint8_t sony[0x40], xbox[0x40], xbox2[0x40], xbox3[0x40];
    uint16_t v = 0x054C, p = 0x0CE6;
    memcpy(sony + 0x10, &v, 2);
    memcpy(sony + 0x14, &p, 2);
    v = 0x045E;
    p = 0x0B13;
    for (uint8_t *c = xbox; c != NULL; c = c == xbox ? xbox2 : c == xbox2 ? xbox3 : NULL) {
        memcpy(c + 0x10, &v, 2);
        memcpy(c + 0x14, &p, 2);
    }

    // a DualSense on player 1: the game's scan finds it, once
    FakeController *dualsense = [FakeController new];
    bridged_pad *a = bridge(dualsense, "DualSense");
    CHECK(a->user_index == 0);
    CHECK(get(0) == ERROR_SUCCESS && a->found);
    CHECK(get(0) == ERROR_SUCCESS);
    CHECK(get(1) == 0x48F && get(2) == 0x48F && get(3) == 0x48F);
    CHECK(asked == 0);
    CHECK(gc_bridge_is_playstation_user(0) && !gc_bridge_is_playstation_user(1));

    // rumble that doesn't change isn't passed on; a change is (this stand-in has no haptics)
    XINPUT_VIBRATION still = { 0, 0 }, rumble = { 30000, 10000 };
    CHECK(fake_ASL_XInputSetState(0, &still) == ERROR_SUCCESS);
    CHECK(fake_ASL_XInputSetState(0, &rumble) == ERROR_SUCCESS);
    drain_input_queue();
    CHECK(a->wanted_motors[0] == 30000 && a->wanted_motors[1] == 10000);
    CHECK(fake_ASL_XInputSetState(1, &rumble) == 0x48F);

    // SDL takes a PlayStation controller as player 1: the DualSense moves to player 2 and the game is asked to look
    set_sdl(0, sony);
    CHECK(get(0) == ERROR_SUCCESS); // SDL's now
    CHECK(a->user_index == 1 && !a->found && asked == 1);
    CHECK(get(1) == ERROR_SUCCESS && a->found);

    // SDL has an Xbox controller as player 3 and lets go of player 1: the DualSense moves back down to player 1
    set_sdl(2, xbox);
    get(1);
    CHECK(a->user_index == 1);
    set_sdl(0, NULL);
    CHECK(get(1) == 0x48F); // the game asks player 2, the DualSense has just moved down
    CHECK(a->user_index == 0 && asked == 2);
    CHECK(get(0) == ERROR_SUCCESS && a->found);

    // SDL fills every index: the DualSense waits and nothing is asked; player 4 frees up and it takes it
    set_sdl(0, xbox2);
    set_sdl(1, xbox3);
    set_sdl(3, sony);
    get(0);
    CHECK(a->user_index == -1 && asked == 2);
    set_sdl(3, NULL);
    CHECK(get(3) == ERROR_SUCCESS);
    CHECK(a->user_index == 3 && asked == 3 && a->found);
    for (int u = 0; u < 4; u++)
        set_sdl(u, NULL);
    get(0);
    CHECK(a->user_index == 0 && asked == 4);

    // a second DualSense (the same one over USB, say) on player 2. The first goes away: the game's next look at
    // player 1 finds the second there, so the game never sees player 1 gone
    FakeController *dualsense_usb = [FakeController new];
    bridged_pad *b = bridge(dualsense_usb, "DualSense");
    CHECK(b->user_index == 1);
    if (@available(macOS 11.3, *))
        controller_disconnected((GCController *)dualsense);
    CHECK(a->controller == nil && asked == 5 && strcmp(asked_why, "a PlayStation controller disconnected") == 0);
    CHECK(get(0) == ERROR_SUCCESS);
    CHECK(b->user_index == 0 && asked == 6 && b->found);
    CHECK(get(1) == 0x48F);

    // a controller the bridge doesn't read going away still asks the game to look (another may take its gamepad)
    FakeController *other = [FakeController new];
    if (@available(macOS 11.3, *))
        controller_disconnected((GCController *)other);
    CHECK(asked == 7 && strcmp(asked_why, "a controller disconnected") == 0);
    CHECK(b->controller == dualsense_usb && b->user_index == 0);

    // PauseOnDisconnect, with the game reading player 1 (the DualSense from above): only that controller going away,
    // with none taking its place in time, asks to pause
    lost_grace = 0;
    game_reads = 0;
    CHECK(b->found);

    // an idle second controller on player 2 dies, and one SDL has: no
    FakeController *idle = [FakeController new];
    bridged_pad *c = bridge(idle, "DualShock 4");
    CHECK(c->user_index == 1 && get(1) == ERROR_SUCCESS && c->found);
    disconnect_pad(idle);
    disconnect_pad(other);
    run_main_queue();
    CHECK(lost == 0 && c->controller == nil && b->user_index == 0);

    // the DualSense's Bluetooth link comes back while its USB cable is in (a second DualSense, on player 2), then the
    // cable comes out: the Bluetooth one moves down to player 1 at the game's next read, no
    FakeController *bluetooth = [FakeController new];
    bridged_pad *d = bridge(bluetooth, "DualSense");
    CHECK(d->user_index == 1);
    disconnect_pad(dualsense_usb);
    CHECK(get(0) == ERROR_SUCCESS && d->user_index == 0 && d->found);
    run_main_queue();
    CHECK(lost == 0);

    // and back: the Bluetooth link drops and the DualSense is on USB again before the check looks: no
    FakeController *usb = [FakeController new];
    disconnect_pad(bluetooth);
    bridged_pad *e = bridge(usb, "DualSense");
    CHECK(e->user_index == 0);
    run_main_queue();
    CHECK(lost == 0);

    // the DualSense the game reads goes away for good: yes, once
    CHECK(get(0) == ERROR_SUCCESS && e->found);
    disconnect_pad(usb);
    run_main_queue();
    CHECK(lost == 1);

    // the game reads SDL's controller (an Xbox one, say). Another one going away: no. That one going away: yes,
    // whether SDL lets go of it after macOS says it's gone or before
    set_sdl(0, xbox);
    get(0);
    FakeController *xbox_pad = [FakeController new];
    disconnect_pad(xbox_pad);
    run_main_queue();
    CHECK(lost == 1);
    disconnect_pad(xbox_pad);
    set_sdl(0, NULL);
    run_main_queue();
    CHECK(lost == 2);
    set_sdl(0, xbox);
    get(0);
    set_sdl(0, NULL);
    get(0);
    disconnect_pad(xbox_pad);
    run_main_queue();
    CHECK(lost == 3);

    drain_input_queue();
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false); // the player index updates, on the main queue
    if (failures == 0)
        printf("gc_bridge tests passed\n");
    else
        printf("gc_bridge tests: %d FAILED\n", failures);
    return failures != 0;
}
