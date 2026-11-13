// t7o_small.c: T7Overcharged's rules for its smaller pieces, and their game side against a made-up game (this file
// includes the source).
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define ARGC(argv) ((int)(sizeof(argv) / sizeof((argv)[0])))

#include "../source/t7o_small.c"

// ---- what the code asks of the rest of BO3MacFix

// the words of the console command being run
static const char *const *command_argv;
static int command_argc;
const char *const *t7o_command_args(int *argc) {
    *argc = command_argc;
    return command_argv;
}

// the game's Lua: what was compiled and run, and in which state
static struct {
    int loads, calls;
    lua_State *state;
    char text[Keymap_Lua_Max], chunk[T7o_Keymap_Name_Max];
    int lock_held; // the VM's critical section was held while it ran
    bool load_fails, leaves_one; // the call leaves a value on the stack
} lua;
static int held_lock = -1; // the critical section the made-up game's lock holds, -1 for none

bool hks_load_source(lua_State *s, const char *source, size_t source_len, const char *chunk_name) {
    lua.loads++;
    lua.state = s;
    snprintf(lua.text, sizeof(lua.text), "%.*s", (int)source_len, source);
    snprintf(lua.chunk, sizeof(lua.chunk), "%s", chunk_name);
    lua.lock_held = held_lock;
    if (lua.load_fails)
        return false;
    HKS_TOP(s)->t = 6; // the compiled function
    HKS_TOP(s)++;
    return true;
}
bool hks_pcall(lua_State *s, int nargs, int nresults) {
    lua.calls++;
    HKS_TOP(s) -= nargs + 1;
    for (int i = 0; i < nresults + (lua.leaves_one ? 1 : 0); i++)
        HKS_TOP(s)++;
    return true;
}

// the hooking library: the hook's original
static int queued_events;
static struct {
    uint64_t time, length, extra, pointer;
    int type, value, value2;
} last_event;
static void game_queue_event(uint64_t time, int type, int value, int value2, uint64_t length, uint64_t extra,
                             uint64_t pointer) {
    queued_events++;
    last_event.time = time;
    last_event.type = type;
    last_event.value = value;
    last_event.value2 = value2;
    last_event.length = length;
    last_event.extra = extra;
    last_event.pointer = pointer;
}
static void *hooked_address;
int DobbyHook(void *address, void *fake_func, void **out_origin_func) {
    hooked_address = address;
    (void)fake_func;
    *out_origin_func = (void *)game_queue_event;
    return 0;
}

// ---- the made-up game

static int game_quitting;
static uint8_t game_client_ui_actives[0x1078 * 2];
static uint8_t game_player_keys[0x1940 * 2];
static uint8_t game_lua_state[0x100], game_lua_lock_info[8];
static HksObject game_lua_stack[16];
static lua_State *game_ui_vm;

static void game_critical_section_enter(void *lock, int critical_section, int mode) {
    CHECK(mode == 0 && held_lock == -1);
    memcpy(lock, &critical_section, sizeof(critical_section));
    held_lock = critical_section;
}
static void game_critical_section_leave(void *lock) {
    int critical_section;
    memcpy(&critical_section, lock, sizeof(critical_section));
    CHECK(critical_section == held_lock);
    held_lock = -1;
}

static void set_key_down(int local_client, int key, bool down) {
    int value = down ? 1 : 0;
    memcpy(game_player_keys + (size_t)local_client * 0x1940 + 0x138 + (size_t)key * 0x18, &value, sizeof(value));
}
static void set_key_catchers(int local_client, int catchers) {
    memcpy(game_client_ui_actives + (size_t)local_client * 0x1078 + 4, &catchers, sizeof(catchers));
}

// the game's side wired to the made-up game, as t7o_small_install does to the real one
static void wire_game(void) {
    quit_in_progress = &game_quitting;
    client_ui_actives = game_client_ui_actives;
    player_keys = game_player_keys;
    game_ui_vm = (lua_State *)game_lua_state;
    ui_vm = &game_ui_vm;
    HksObject *stack = game_lua_stack + 2; // the VM's stack, two values in
    memcpy(game_lua_state + 0x50, &stack, sizeof(stack));
    memcpy(game_lua_state + 0x48, &stack, sizeof(stack));
    CritSec_ScopedLock = game_critical_section_enter;
    CritSec_ScopedUnlock = game_critical_section_leave;
    orig_Sys_QueEvent = game_queue_event;
}

// the console runs a command of ours
static void run_command(void (*command)(void), int argc, const char *const *argv) {
    command_argc = argc;
    command_argv = argv;
    command();
}

// a key or mouse message goes through the game's Sys_QueEvent
static void queue_key(int key, bool down) {
    fake_Sys_QueEvent(0x1234, 1, key, down ? 1 : 0, 0, 0, 0);
}

static void reset_keymap(void) {
    memset(&keymap, 0, sizeof(keymap));
    atomic_store(&keymap_count, 0);
    memset(&lua, 0, sizeof(lua));
}

static void test_stoi(void) {
    int value = -7;
    CHECK(t7o_stoi("27", &value) == T7o_Number_Ok && value == 27);
    CHECK(t7o_stoi("  200", &value) == T7o_Number_Ok && value == 200); // blanks first
    CHECK(t7o_stoi("\t+5x", &value) == T7o_Number_Ok && value == 5);   // a sign, junk after the digits
    CHECK(t7o_stoi("-1", &value) == T7o_Number_Ok && value == -1);
    CHECK(t7o_stoi("0x1b", &value) == T7o_Number_Ok && value == 0); // base 10: the x ends it
    value = 99;
    CHECK(t7o_stoi("", &value) == T7o_Number_Invalid && value == 99);
    CHECK(t7o_stoi("  ", &value) == T7o_Number_Invalid);
    CHECK(t7o_stoi("esc", &value) == T7o_Number_Invalid);
    CHECK(t7o_stoi("-", &value) == T7o_Number_Invalid);
    CHECK(t7o_stoi(NULL, &value) == T7o_Number_Invalid);
    // a Windows long is an int
    CHECK(t7o_stoi("2147483647", &value) == T7o_Number_Ok && value == INT_MAX);
    CHECK(t7o_stoi("-2147483648", &value) == T7o_Number_Ok && value == INT_MIN);
    CHECK(t7o_stoi("2147483648", &value) == T7o_Number_Range && value == INT_MIN);
    CHECK(t7o_stoi("-2147483649", &value) == T7o_Number_Range);
    CHECK(t7o_stoi("99999999999999999999999", &value) == T7o_Number_Range);
}

static void test_keyset(void) {
    t7o_keyset a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    t7o_keyset_add(&a, 0);
    t7o_keyset_add(&a, 63);
    t7o_keyset_add(&a, 64);
    t7o_keyset_add(&a, 255);
    t7o_keyset_add(&a, 256); // left out
    t7o_keyset_add(&a, -1);
    CHECK(t7o_keyset_has(&a, 0) && t7o_keyset_has(&a, 63) && t7o_keyset_has(&a, 64) && t7o_keyset_has(&a, 255));
    CHECK(!t7o_keyset_has(&a, 1) && !t7o_keyset_has(&a, 62) && !t7o_keyset_has(&a, 65) && !t7o_keyset_has(&a, 254));
    CHECK(!t7o_keyset_has(&a, 256) && !t7o_keyset_has(&a, -1));
    CHECK(a.bits[0] == ((uint64_t)1 | (uint64_t)1 << 63) && a.bits[1] == 1 && a.bits[2] == 0 &&
          a.bits[3] == (uint64_t)1 << 63);
    CHECK(!t7o_keyset_equal(&a, &b));
    t7o_keyset_add(&b, 255);
    t7o_keyset_add(&b, 64);
    t7o_keyset_add(&b, 63);
    t7o_keyset_add(&b, 0);
    CHECK(t7o_keyset_equal(&a, &b));
}

static void test_keymap_command(void) {
    t7o_keymap map;
    memset(&map, 0, sizeof(map));
    // All-around Enhancement's two
    const char *escape[] = { "customkeymap", "loadingskip1", "27" };
    const char *click[] = { "customkeymap", "loadingskip2", "200" };
    CHECK(t7o_keymap_command(&map, ARGC(escape), escape) == T7o_Keymap_Added);
    CHECK(t7o_keymap_command(&map, ARGC(click), click) == T7o_Keymap_Added);
    CHECK(map.count == 2);
    CHECK(strcmp(map.mappings[0].name, "loadingskip1") == 0 && map.mappings[0].debounce_ms == 1000);
    CHECK(t7o_keyset_has(&map.mappings[0].keys, 27) && map.mappings[0].keys.bits[0] == (uint64_t)1 << 27);
    CHECK(strcmp(map.mappings[1].name, "loadingskip2") == 0 && map.mappings[1].debounce_ms == 1000);
    CHECK(map.mappings[1].keys.bits[3] == (uint64_t)1 << (200 - 192) && map.mappings[1].keys.bits[0] == 0);
    // its loading screen runs them again at every map's load: the same keys take the new name and time
    const char *again[] = { "customkeymap", "loadingskip3", "27", "250" };
    CHECK(t7o_keymap_command(&map, ARGC(again), again) == T7o_Keymap_Replaced);
    CHECK(map.count == 2 && strcmp(map.mappings[0].name, "loadingskip3") == 0 && map.mappings[0].debounce_ms == 250);
    // T7Overcharged's example: two keys together
    const char *both[] = { "customkeymap", "test", "168+169" };
    CHECK(t7o_keymap_command(&map, ARGC(both), both) == T7o_Keymap_Added);
    CHECK(map.count == 3 && t7o_keyset_has(&map.mappings[2].keys, 168) && t7o_keyset_has(&map.mappings[2].keys, 169));
    CHECK(map.mappings[2].keys.bits[2] == ((uint64_t)1 << (168 - 128) | (uint64_t)1 << (169 - 128)));
    // the same set in another order is the same mapping
    const char *swapped[] = { "customkeymap", "test2", "169+168", "-5" };
    CHECK(t7o_keymap_command(&map, ARGC(swapped), swapped) == T7o_Keymap_Replaced);
    CHECK(map.count == 3 && strcmp(map.mappings[2].name, "test2") == 0 && map.mappings[2].debounce_ms == -5);
    // a trailing '+' ends the list; blanks and junk after a number are std::stoi's
    const char *trailing[] = { "customkeymap", "t", " 13+9x+" };
    CHECK(t7o_keymap_command(&map, ARGC(trailing), trailing) == T7o_Keymap_Added);
    CHECK(map.mappings[3].keys.bits[0] == ((uint64_t)1 << 13 | (uint64_t)1 << 9));
    // keys above 255 and negative ones are left out, even all of them: a mapping that never fires
    const char *outside[] = { "customkeymap", "never", "256+-1+4294967295" };
    CHECK(t7o_keymap_command(&map, ARGC(outside), outside) == T7o_Keymap_Bad_Number); // beyond an int: throws
    const char *outside2[] = { "customkeymap", "never", "256+-1" };
    CHECK(t7o_keymap_command(&map, ARGC(outside2), outside2) == T7o_Keymap_Added);
    CHECK(map.count == 5 && map.mappings[4].keys.bits[0] == 0 && map.mappings[4].keys.bits[3] == 0);
    const char *none[] = { "customkeymap", "empty", "" };
    CHECK(t7o_keymap_command(&map, ARGC(none), none) == T7o_Keymap_Replaced); // the empty set again
    CHECK(map.count == 5 && strcmp(map.mappings[4].name, "empty") == 0);
    // what std::stoi throws for: nothing changes
    const char *empty_part[] = { "customkeymap", "x", "27++200" };
    const char *leading_plus[] = { "customkeymap", "x", "+27" };
    const char *word[] = { "customkeymap", "x", "esc" };
    const char *bad_time[] = { "customkeymap", "x", "30", "soon" };
    const char *huge_time[] = { "customkeymap", "x", "30", "3000000000" };
    CHECK(t7o_keymap_command(&map, ARGC(empty_part), empty_part) == T7o_Keymap_Bad_Number);
    CHECK(t7o_keymap_command(&map, ARGC(leading_plus), leading_plus) == T7o_Keymap_Bad_Number);
    CHECK(t7o_keymap_command(&map, ARGC(word), word) == T7o_Keymap_Bad_Number);
    CHECK(t7o_keymap_command(&map, ARGC(bad_time), bad_time) == T7o_Keymap_Bad_Number);
    CHECK(t7o_keymap_command(&map, ARGC(huge_time), huge_time) == T7o_Keymap_Bad_Number);
    CHECK(map.count == 5);
    // fewer than three words: T7Overcharged's usage
    const char *short1[] = { "customkeymap", "x" };
    CHECK(t7o_keymap_command(&map, ARGC(short1), short1) == T7o_Keymap_Usage);
    CHECK(t7o_keymap_command(&map, 3, NULL) == T7o_Keymap_Usage);
    CHECK(t7o_keymap_command(NULL, ARGC(escape), escape) == T7o_Keymap_Usage);
    // a name that would end the Lua's quoted string, or doesn't fit
    const char *quote[] = { "customkeymap", "a') os.exit() --", "30" };
    const char *backslash[] = { "customkeymap", "a\\", "30" };
    const char *newline[] = { "customkeymap", "a\nb", "30" };
    const char *empty_name[] = { "customkeymap", "", "30" };
    char long_name[T7o_Keymap_Name_Max + 1];
    memset(long_name, 'n', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    const char *too_long[] = { "customkeymap", long_name, "30" };
    CHECK(t7o_keymap_command(&map, ARGC(quote), quote) == T7o_Keymap_Bad_Name);
    CHECK(t7o_keymap_command(&map, ARGC(backslash), backslash) == T7o_Keymap_Bad_Name);
    CHECK(t7o_keymap_command(&map, ARGC(newline), newline) == T7o_Keymap_Bad_Name);
    CHECK(t7o_keymap_command(&map, ARGC(empty_name), empty_name) == T7o_Keymap_Bad_Name);
    CHECK(t7o_keymap_command(&map, ARGC(too_long), too_long) == T7o_Keymap_Bad_Name);
    long_name[T7o_Keymap_Name_Max - 1] = '\0'; // 63 characters fit
    CHECK(t7o_keymap_command(&map, ARGC(too_long), too_long) == T7o_Keymap_Added);
    CHECK(map.count == 6 && strlen(map.mappings[5].name) == T7o_Keymap_Name_Max - 1);
    // a full table keeps what it has
    char numbers[T7o_Keymap_Max][8];
    for (int i = map.count; i < T7o_Keymap_Max; i++) {
        snprintf(numbers[i], sizeof(numbers[i]), "%d", 100 + i);
        const char *one[] = { "customkeymap", "n", numbers[i] };
        CHECK(t7o_keymap_command(&map, ARGC(one), one) == T7o_Keymap_Added);
    }
    const char *one_more[] = { "customkeymap", "n", "99" };
    CHECK(t7o_keymap_command(&map, ARGC(one_more), one_more) == T7o_Keymap_Full);
    CHECK(map.count == T7o_Keymap_Max);
    const char *replace_full[] = { "customkeymap", "m", "27" }; // replacing still works
    CHECK(t7o_keymap_command(&map, ARGC(replace_full), replace_full) == T7o_Keymap_Replaced);
}

static void test_keymap_event_and_find(void) {
    // key releases only, with the console closed, while the game isn't quitting
    CHECK(t7o_keymap_event_counts(1, 0, 27, 0, 0));
    CHECK(!t7o_keymap_event_counts(1, 1, 27, 0, 0));  // down
    CHECK(!t7o_keymap_event_counts(1, 3, 27, 0, 0));  // value2's low bit
    CHECK(t7o_keymap_event_counts(1, 2, 27, 0, 0));   // ... and only that
    CHECK(!t7o_keymap_event_counts(2, 0, 27, 0, 0));  // a character
    CHECK(!t7o_keymap_event_counts(0, 0, 27, 0, 0));
    CHECK(!t7o_keymap_event_counts(1, 0, 27, 1, 0));  // quitting
    CHECK(!t7o_keymap_event_counts(1, 0, 27, 0, 1));  // the console is open
    CHECK(!t7o_keymap_event_counts(1, 0, 27, 0, 3));
    CHECK(t7o_keymap_event_counts(1, 0, 27, 0, 0x10)); // other catchers (the UI, chat) don't matter
    CHECK(t7o_keymap_event_counts(1, 0, 255, 0, 0));
    CHECK(t7o_keymap_event_counts(1, 0, 0, 0, 0));
    CHECK(!t7o_keymap_event_counts(1, 0, 256, 0, 0));
    CHECK(!t7o_keymap_event_counts(1, 0, -1, 0, 0));

    t7o_keymap map;
    memset(&map, 0, sizeof(map));
    const char *escape[] = { "customkeymap", "loadingskip1", "27" };
    const char *combo[] = { "customkeymap", "combo", "168+169" };
    t7o_keymap_command(&map, ARGC(escape), escape);
    t7o_keymap_command(&map, ARGC(combo), combo);
    t7o_keyset held;
    memset(&held, 0, sizeof(held));
    // the key that came up is still held in the game's own state, or isn't: both are the set {27}
    const t7o_keymapping *found = t7o_keymap_find(&map, &held, 27);
    CHECK(found == &map.mappings[0]);
    t7o_keyset_add(&held, 27);
    CHECK(t7o_keymap_find(&map, &held, 27) == &map.mappings[0]);
    // another key held: no mapping is exactly that
    t7o_keyset_add(&held, 'w');
    CHECK(t7o_keymap_find(&map, &held, 27) == NULL);
    // two together: the one that comes up first, while the other is held
    memset(&held, 0, sizeof(held));
    t7o_keyset_add(&held, 168);
    t7o_keyset_add(&held, 169);
    CHECK(t7o_keymap_find(&map, &held, 169) == &map.mappings[1]);
    memset(&held, 0, sizeof(held));
    t7o_keyset_add(&held, 168);
    CHECK(t7o_keymap_find(&map, &held, 169) == &map.mappings[1]);
    CHECK(t7o_keymap_find(&map, &held, 168) == NULL); // then the other one alone isn't the pair
    memset(&held, 0, sizeof(held));
    CHECK(t7o_keymap_find(&map, &held, 169) == NULL);
    CHECK(t7o_keymap_find(&map, &held, 300) == NULL);
    const char *never[] = { "customkeymap", "never", "256" };
    t7o_keymap_command(&map, ARGC(never), never);
    CHECK(map.count == 3 && map.mappings[2].keys.bits[0] == 0 && map.mappings[2].keys.bits[3] == 0);
    CHECK(t7o_keymap_find(&map, &held, 300) == NULL && t7o_keymap_find(&map, &held, -1) == NULL);
    CHECK(t7o_keymap_find(&map, &held, 256) == NULL);
    CHECK(t7o_keymap_find(&map, NULL, 27) == NULL && t7o_keymap_find(NULL, &held, 27) == NULL);
}

static void test_keymap_debounce(void) {
    t7o_debounce d;
    memset(&d, 0, sizeof(d));
    const int64_t ms = 1000000;
    // the first time always, and the first mapping's time is the one for all
    CHECK(t7o_keymap_debounce_passes(&d, 1000, 5000 * ms));
    CHECK(d.started && !d.first && d.debounce_ms == 1000 && d.last_ns == 5000 * ms);
    CHECK(!t7o_keymap_debounce_passes(&d, 0, 5999 * ms + ms - 1)); // 999.999999 ms: 999 whole ones
    CHECK(d.last_ns == 5000 * ms); // a dropped one doesn't move the time
    CHECK(t7o_keymap_debounce_passes(&d, 0, 6000 * ms));
    CHECK(d.last_ns == 6000 * ms && d.debounce_ms == 1000);
    CHECK(!t7o_keymap_debounce_passes(&d, 5000, 6500 * ms));
    CHECK(t7o_keymap_debounce_passes(&d, 5000, 7000 * ms)); // still the first one's 1000
    // a debounce of 0 or less never drops
    memset(&d, 0, sizeof(d));
    CHECK(t7o_keymap_debounce_passes(&d, -5, 100));
    CHECK(t7o_keymap_debounce_passes(&d, 1000, 100));
    CHECK(t7o_keymap_debounce_passes(&d, 1000, 101));
    memset(&d, 0, sizeof(d));
    CHECK(t7o_keymap_debounce_passes(&d, 0, 100));
    CHECK(t7o_keymap_debounce_passes(&d, 1000, 100));
    // a clock that went back: the difference is cut towards zero, so -0.5 ms is 0, and that's below 1
    memset(&d, 0, sizeof(d));
    CHECK(t7o_keymap_debounce_passes(&d, 1, 10 * ms));
    CHECK(!t7o_keymap_debounce_passes(&d, 1, 10 * ms - ms / 2));
    CHECK(t7o_keymap_debounce_passes(&d, 1, 11 * ms));
}

static void test_keymap_lua(void) {
    char text[Keymap_Lua_Max];
    CHECK(t7o_keymap_lua(text, sizeof(text), "loadingskip1"));
    CHECK(strcmp(text, " \t\t\t\t\t\t\tEnableGlobals(); \t\t\t\t\t\t\tif CoD.Mod and CoD.Mod.CallCustomFunction then "
                       "\t\t\t\t\t\t\tCoD.Mod.CallCustomFunction('loadingskip1') end; \t\t\t\t\t\t\t") == 0);
    // T7Overcharged's text is 131 characters with "{}" (the length it gives its formatter, 0x83)
    CHECK(strlen(text) == 131 - 2 + strlen("loadingskip1"));
    char small[40];
    CHECK(!t7o_keymap_lua(small, sizeof(small), "loadingskip1") && small[0] == '\0');
    CHECK(!t7o_keymap_lua(text, sizeof(text), NULL) && !t7o_keymap_lua(NULL, 10, "x"));
    char exact[133];
    CHECK(t7o_keymap_lua(exact, sizeof(exact), "abc") && strlen(exact) == 132);
    CHECK(!t7o_keymap_lua(exact, sizeof(exact), "abcd") && exact[0] == '\0');
}

// the console command and the hook, against the made-up game
static void test_keymap_game(void) {
    wire_game();
    reset_keymap();
    // nothing mapped: the hook passes the events on and looks at nothing
    game_quitting = 1; // would stop a mapping
    queue_key(27, false);
    CHECK(queued_events == 1 && last_event.type == 1 && last_event.value == 27 && last_event.value2 == 0);
    CHECK(last_event.time == 0x1234 && lua.loads == 0);
    game_quitting = 0;

    const char *escape[] = { "customkeymap", "loadingskip1", "27" };
    const char *click[] = { "customkeymap", "loadingskip2", "200" };
    run_command(customkeymap_f, ARGC(escape), escape);
    run_command(customkeymap_f, ARGC(click), click);
    CHECK(atomic_load(&keymap_count) == 2 && keymap.count == 2);
    const char *usage[] = { "customkeymap" };
    run_command(customkeymap_f, ARGC(usage), usage);
    run_command(customkeymap_f, 0, NULL); // arguments that can't be read
    CHECK(keymap.count == 2);

    // Escape goes down: nothing; it comes up while the game holds it as down: loadingskip1's function
    memcpy(game_lua_lock_info + 4, &(int){ 0x41 }, sizeof(int));
    uint8_t *info = game_lua_lock_info;
    memcpy(game_lua_state + 0xa0, &info, sizeof(info)); // the lua_State's pointer to its lock
    set_key_down(0, 27, true);
    queue_key(27, true);
    CHECK(lua.loads == 0 && queued_events == 2 && last_event.value2 == 1);
    queue_key(27, false);
    CHECK(queued_events == 3 && last_event.value == 27 && last_event.value2 == 0);
    CHECK(lua.loads == 1 && lua.calls == 1 && lua.state == game_ui_vm);
    CHECK(strstr(lua.text, "CoD.Mod.CallCustomFunction('loadingskip1') end;") != NULL);
    CHECK(strcmp(lua.chunk, "loadingskip1") == 0);
    CHECK(lua.lock_held == 0x41 && held_lock == -1); // under the VM's own critical section, and left again
    HksObject *base;
    memcpy(&base, game_lua_state + 0x50, sizeof(base));
    HksObject *top;
    memcpy(&top, game_lua_state + 0x48, sizeof(top));
    CHECK(top == base); // the stack as it was
    set_key_down(0, 27, false);

    // a click: a second within T7Overcharged's 1000 ms of the last one is dropped, for any mapping
    queue_key(200, false);
    CHECK(lua.loads == 1);
    keymap.debounce.last_ns -= 1000 * 1000000LL; // a second later
    queue_key(200, false);
    CHECK(lua.loads == 2 && strcmp(lua.chunk, "loadingskip2") == 0);
    CHECK(strstr(lua.text, "CallCustomFunction('loadingskip2')") != NULL);

    // the VM without a lock of its own: the game's critical section 0x3c
    keymap.debounce.last_ns -= 1000 * 1000000LL;
    memset(game_lua_state + 0xa0, 0, sizeof(void *));
    queue_key(27, false);
    CHECK(lua.loads == 3 && lua.lock_held == 0x3c && held_lock == -1);

    // what stops it: another key held, the console open, the game quitting, a key going down, no UI state
    keymap.debounce.last_ns -= 1000 * 1000000LL;
    set_key_down(0, 'w', true);
    queue_key(27, false);
    set_key_down(0, 'w', false);
    set_key_down(1, 'w', true); // local client 1's keys don't count
    set_key_catchers(0, 1);
    queue_key(27, false);
    set_key_catchers(0, 0);
    set_key_catchers(1, 1); // nor its console
    game_quitting = 1;
    queue_key(27, false);
    game_quitting = 0;
    fake_Sys_QueEvent(0, 2, 27, 0, 0, 0, 0); // a character, not a key
    CHECK(lua.loads == 3);
    queue_key(27, false); // local client 1's console and keys don't stop it
    CHECK(lua.loads == 4);
    set_key_down(1, 'w', false);
    set_key_catchers(1, 0);
    keymap.debounce.last_ns -= 1000 * 1000000LL;
    game_ui_vm = NULL;
    queue_key(27, false);
    CHECK(lua.loads == 4);
    game_ui_vm = (lua_State *)game_lua_state;
    keymap.debounce.last_ns -= 1000 * 1000000LL;
    lua.load_fails = true; // a compile error: nothing is called, the stack is as it was
    queue_key(27, false);
    CHECK(lua.loads == 5 && lua.calls == 4 && held_lock == -1);
    memcpy(&top, game_lua_state + 0x48, sizeof(top));
    CHECK(top == base);
    lua.load_fails = false;
    keymap.debounce.last_ns -= 1000 * 1000000LL;
    lua.leaves_one = true; // the stack is put back as it was whatever the Lua leaves on it
    queue_key(27, false);
    CHECK(lua.loads == 6 && lua.calls == 5);
    memcpy(&top, game_lua_state + 0x48, sizeof(top));
    CHECK(top == base);
    lua.leaves_one = false;
    // every event went on to the game, with all its values
    fake_Sys_QueEvent(77, 3, 5, 6, 7, 8, 9);
    CHECK(last_event.time == 77 && last_event.type == 3 && last_event.value == 5 && last_event.value2 == 6 &&
          last_event.length == 7 && last_event.extra == 8 && last_event.pointer == 9);
}

static void test_menuresponse(void) {
    // the number after the response: AAE's binds pass 200 or 50
    int debounce = -1;
    const char *flash[] = { "SendMenuResponse", "StartMenu_Main", "aae_flashlight", "200" };
    const char *plain[] = { "SendMenuResponse", "StartMenu_Main", "aae_hud" };
    const char *word[] = { "SendMenuResponse", "StartMenu_Main", "aae_hud", "soon" };
    const char *huge[] = { "SendMenuResponse", "StartMenu_Main", "aae_hud", "3000000000" };
    const char *junk[] = { "SendMenuResponse", "StartMenu_Main", "aae_hud", " 50ms" };
    CHECK(t7o_menuresponse_debounce(ARGC(flash), flash, &debounce) == T7o_Number_Ok && debounce == 200);
    CHECK(t7o_menuresponse_debounce(ARGC(plain), plain, &debounce) == T7o_Number_Ok && debounce == 1000);
    CHECK(t7o_menuresponse_debounce(ARGC(junk), junk, &debounce) == T7o_Number_Ok && debounce == 50);
    CHECK(t7o_menuresponse_debounce(ARGC(word), word, &debounce) == T7o_Number_Invalid);
    CHECK(t7o_menuresponse_debounce(ARGC(huge), huge, &debounce) == T7o_Number_Range);

    // one time for every call, each call's own debounce: the first always goes
    const int64_t ms = 1000000;
    t7o_debounce d;
    memset(&d, 0, sizeof(d));
    CHECK(t7o_menuresponse_debounce_passes(&d, 200, 1000 * ms));
    CHECK(d.started && !d.first && d.last_ns == 1000 * ms && d.debounce_ms == 200);
    CHECK(!t7o_menuresponse_debounce_passes(&d, 200, 1199 * ms)); // a bind pressed again too soon
    CHECK(d.last_ns == 1000 * ms && d.debounce_ms == 200);
    CHECK(t7o_menuresponse_debounce_passes(&d, 50, 1100 * ms)); // another one with 50 ms: 100 ms is enough for it
    CHECK(d.last_ns == 1100 * ms && d.debounce_ms == 50);
    CHECK(!t7o_menuresponse_debounce_passes(&d, 200, 1299 * ms));
    CHECK(t7o_menuresponse_debounce_passes(&d, 200, 1300 * ms)); // exactly the debounce: goes
    CHECK(!t7o_menuresponse_debounce_passes(&d, 1000, 2299 * ms)); // without a number: a second
    CHECK(t7o_menuresponse_debounce_passes(&d, 0, 2299 * ms));
    CHECK(t7o_menuresponse_debounce_passes(&d, 0, 2299 * ms)); // 0 drops nothing
    CHECK(t7o_menuresponse_debounce_passes(&d, -10, 2299 * ms));
    // whole milliseconds, cut towards zero
    memset(&d, 0, sizeof(d));
    CHECK(t7o_menuresponse_debounce_passes(&d, 1, 0));
    CHECK(!t7o_menuresponse_debounce_passes(&d, 1, ms - 1));
    CHECK(t7o_menuresponse_debounce_passes(&d, 1, ms));

    // the console command's side: fewer than three words or a time that isn't one change nothing
    memset(&menuresponse_debounce, 0, sizeof(menuresponse_debounce));
    const char *two[] = { "SendMenuResponse", "StartMenu_Main" };
    CHECK(!t7o_small_menuresponse_passes(ARGC(two), two) && !menuresponse_debounce.started);
    CHECK(!t7o_small_menuresponse_passes(ARGC(word), word) && !menuresponse_debounce.started);
    CHECK(!t7o_small_menuresponse_passes(3, NULL));
    CHECK(t7o_small_menuresponse_passes(ARGC(flash), flash));
    CHECK(!t7o_small_menuresponse_passes(ARGC(flash), flash)); // the same bind at once
    menuresponse_debounce.last_ns -= 200 * ms;
    debug = true; // its lines
    CHECK(t7o_small_menuresponse_passes(ARGC(flash), flash));
    CHECK(!t7o_small_menuresponse_passes(ARGC(plain), plain));
    debug = false;
    // a call that sends nothing (no such menu, say) still counts: the time is set by what passes
    const char *empty[] = { "SendMenuResponse", "", "" };
    menuresponse_debounce.last_ns -= 1000 * ms;
    CHECK(t7o_small_menuresponse_passes(ARGC(empty), empty));
    CHECK(!t7o_small_menuresponse_passes(ARGC(flash), flash));
}

int main(void) {
    test_stoi();
    test_keyset();
    test_keymap_command();
    test_keymap_event_and_find();
    test_keymap_debounce();
    test_keymap_lua();
    test_keymap_game();
    test_menuresponse();
    if (failures == 0)
        printf("test_t7o_small: all passed\n");
    return failures == 0 ? 0 : 1;
}
