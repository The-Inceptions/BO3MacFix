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

static int held_lock = -1; // the critical section the made-up game's lock holds last, -1 for none
static int held_locks[4], held_count; // all it holds, in the order taken

// the game's Lua: what the take function gave back (T7OTakeCustomKey)
static struct {
    int strings, nils;
    char last[T7o_Keymap_Name_Max];
} pushed;
void hks_push_string(lua_State *s, const char *value) {
    (void)s;
    pushed.strings++;
    snprintf(pushed.last, sizeof(pushed.last), "%s", value);
}
void hks_push_nil(lua_State *s) {
    (void)s;
    pushed.nils++;
    pushed.last[0] = '\0';
}
// T7OTakeCustomKey() as the UI's timer calls it: the name, or NULL for nil
static const char *take(void) {
    int strings = pushed.strings, nils = pushed.nils;
    CHECK(t7o_small_lua_take_custom_key(NULL) == 1);
    CHECK(pushed.strings + pushed.nils == strings + nils + 1); // one value each time
    return pushed.strings > strings ? pushed.last : NULL;
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

// the game's scoped critical sections: each object goes on a per-thread list, so they're left the other way round
static void game_critical_section_enter(void *lock, int critical_section, int mode) {
    CHECK(mode == 0 && held_count < 4);
    memcpy(lock, &critical_section, sizeof(critical_section));
    if (held_count < 4)
        held_locks[held_count++] = critical_section;
    held_lock = critical_section;
}
static void game_critical_section_leave(void *lock) {
    int critical_section;
    memcpy(&critical_section, lock, sizeof(critical_section));
    CHECK(held_count > 0 && critical_section == held_locks[held_count - 1]);
    if (held_count > 0)
        held_count--;
    held_lock = held_count > 0 ? held_locks[held_count - 1] : -1;
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
    memset(&key_queue, 0, sizeof(key_queue));
    atomic_store(&key_events_lost, 0);
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

// the window thread's queue: in order, full at T7o_Key_Queue_Size, and past the counters' wrap
static void test_key_queue(void) {
    t7o_key_queue q;
    memset(&q, 0, sizeof(q));
    t7o_key_event e, out;
    memset(&e, 0, sizeof(e));
    CHECK(!t7o_key_queue_take(&q, &out));
    for (int i = 0; i < T7o_Key_Queue_Size; i++) {
        e.key = i;
        e.time_ns = 1000 + i;
        t7o_keyset_add(&e.held, i);
        CHECK(t7o_key_queue_put(&q, &e));
    }
    e.key = 99;
    CHECK(!t7o_key_queue_put(&q, &e)); // full: lost, the others kept
    for (int i = 0; i < T7o_Key_Queue_Size; i++) {
        CHECK(t7o_key_queue_take(&q, &out) && out.key == i && out.time_ns == 1000 + i && t7o_keyset_has(&out.held, i));
    }
    CHECK(!t7o_key_queue_take(&q, &out));
    // the counters wrap: still in order
    atomic_store(&q.written, UINT_MAX - 1);
    atomic_store(&q.read, UINT_MAX - 1);
    for (int i = 0; i < 4; i++) {
        e.key = 200 + i;
        CHECK(t7o_key_queue_put(&q, &e));
    }
    for (int i = 0; i < 4; i++)
        CHECK(t7o_key_queue_take(&q, &out) && out.key == 200 + i);
    CHECK(!t7o_key_queue_take(&q, &out) && atomic_load(&q.read) == 2);
}

// the main thread's rule for one event: not too old, the exact keys, the debounce from the event's time
static void test_keymap_fire(void) {
    const int64_t ms = 1000000;
    t7o_keymap map;
    memset(&map, 0, sizeof(map));
    const char *escape[] = { "customkeymap", "loadingskip1", "27" };
    t7o_keymap_command(&map, ARGC(escape), escape);
    t7o_key_event e;
    memset(&e, 0, sizeof(e));
    e.key = 27;
    e.time_ns = 5000 * ms;
    CHECK(t7o_keymap_fire(&map, &e, 6000 * ms) == &map.mappings[0]); // 1000 ms old is still taken
    CHECK(map.debounce.last_ns == 5000 * ms);                         // counted from the release
    e.time_ns = 5999 * ms;
    CHECK(t7o_keymap_fire(&map, &e, 6000 * ms) == NULL);              // 999 ms after the last one
    e.time_ns = 6000 * ms;
    CHECK(t7o_keymap_fire(&map, &e, 7001 * ms) == NULL);              // 1001 ms old: dropped
    CHECK(map.debounce.last_ns == 5000 * ms);                         // and it doesn't count
    CHECK(t7o_keymap_fire(&map, &e, 7000 * ms) == &map.mappings[0]);
    e.time_ns = 8000 * ms;
    t7o_keyset_add(&e.held, 'w');
    CHECK(t7o_keymap_fire(&map, &e, 8000 * ms) == NULL);              // another key held
    e.key = 26;
    memset(&e.held, 0, sizeof(e.held));
    CHECK(t7o_keymap_fire(&map, &e, 8000 * ms) == NULL);              // no mapping
    CHECK(t7o_keymap_fire(NULL, &e, 8000 * ms) == NULL && t7o_keymap_fire(&map, NULL, 8000 * ms) == NULL);
    CHECK(map.debounce.last_ns == 6000 * ms);
}

// the console command, the hook on the window thread and the take function on the main thread, against the made-up
// game
static void test_keymap_game(void) {
    wire_game();
    reset_keymap();
    // nothing mapped: the hook passes the events on and notes nothing
    queue_key(27, false);
    CHECK(queued_events == 1 && last_event.type == 1 && last_event.value == 27 && last_event.value2 == 0);
    CHECK(last_event.time == 0x1234 && take() == NULL);

    const char *escape[] = { "customkeymap", "loadingskip1", "27" };
    const char *click[] = { "customkeymap", "loadingskip2", "200" };
    run_command(customkeymap_f, ARGC(escape), escape);
    run_command(customkeymap_f, ARGC(click), click);
    CHECK(atomic_load(&keymap_count) == 2 && keymap.count == 2);
    const char *usage[] = { "customkeymap" };
    run_command(customkeymap_f, ARGC(usage), usage);
    run_command(customkeymap_f, 0, NULL); // arguments that can't be read
    CHECK(keymap.count == 2);

    // Escape goes down: nothing; it comes up while the game holds it as down: noted, and taken as loadingskip1
    set_key_down(0, 27, true);
    queue_key(27, true);
    CHECK(queued_events == 2 && last_event.value2 == 1 && atomic_load(&key_queue.written) == 0);
    queue_key(27, false);
    CHECK(queued_events == 3 && last_event.value == 27 && last_event.value2 == 0);
    CHECK(atomic_load(&key_queue.written) == 1);
    CHECK(t7o_keyset_has(&key_queue.events[0].held, 27) && key_queue.events[0].key == 27);
    const char *name = take();
    CHECK(name != NULL && strcmp(name, "loadingskip1") == 0);
    CHECK(take() == NULL); // taken once
    set_key_down(0, 27, false);

    // a click: a second within T7Overcharged's 1000 ms of the last one is dropped, for any mapping
    queue_key(200, false);
    CHECK(take() == NULL);
    keymap.debounce.last_ns -= 1000 * 1000000LL; // a second later
    queue_key(200, false);
    name = take();
    CHECK(name != NULL && strcmp(name, "loadingskip2") == 0);

    // two that come up before a timer looks: the first fires, the second stays for the next look (and is too soon)
    keymap.debounce.last_ns -= 1000 * 1000000LL;
    queue_key(27, false);
    queue_key(200, false);
    name = take();
    CHECK(name != NULL && strcmp(name, "loadingskip1") == 0);
    CHECK(atomic_load(&key_queue.read) + 1 == atomic_load(&key_queue.written));
    CHECK(take() == NULL && atomic_load(&key_queue.read) == atomic_load(&key_queue.written));

    // one that fires nothing, then one that fires, before a timer looks: the second is still called at that look
    keymap.debounce.last_ns -= 1000 * 1000000LL;
    queue_key(13, false); // Enter: no mapping
    queue_key(200, false);
    name = take();
    CHECK(name != NULL && strcmp(name, "loadingskip2") == 0);
    CHECK(atomic_load(&key_queue.read) == atomic_load(&key_queue.written));

    // what keeps it from being noted: another key held, the console open, the game quitting, a character
    keymap.debounce.last_ns -= 1000 * 1000000LL;
    set_key_down(0, 'w', true);
    queue_key(27, false);
    CHECK(take() == NULL); // noted, but the keys aren't the mapping's
    set_key_down(0, 'w', false);
    set_key_down(1, 'w', true); // local client 1's keys don't count
    unsigned written = atomic_load(&key_queue.written);
    set_key_catchers(0, 1);
    queue_key(27, false);
    set_key_catchers(0, 0);
    set_key_catchers(1, 1); // nor its console
    game_quitting = 1;
    queue_key(27, false);
    game_quitting = 0;
    fake_Sys_QueEvent(0, 2, 27, 0, 0, 0, 0); // a character, not a key
    CHECK(atomic_load(&key_queue.written) == written);
    queue_key(27, false); // local client 1's console and keys don't stop it
    name = take();
    CHECK(name != NULL && strcmp(name, "loadingskip1") == 0);
    set_key_down(1, 'w', false);
    set_key_catchers(1, 0);

    // one that waited too long (nothing took it, the UI was being rebuilt): dropped, the debounce untouched
    keymap.debounce.last_ns -= 1000 * 1000000LL;
    queue_key(27, false);
    key_queue.events[(atomic_load(&key_queue.written) - 1) % T7o_Key_Queue_Size].time_ns -= 1001 * 1000000LL;
    int64_t last = keymap.debounce.last_ns;
    debug = true; // its lines
    CHECK(take() == NULL && keymap.debounce.last_ns == last);
    // a full queue loses the newest, keeps the rest, and says how many when it's next looked at
    for (int i = 0; i < T7o_Key_Queue_Size + 3; i++)
        queue_key(200, false);
    CHECK(atomic_load(&key_events_lost) == 3);
    name = take();
    CHECK(name != NULL && strcmp(name, "loadingskip2") == 0 && atomic_load(&key_events_lost) == 0);
    while (take() != NULL || atomic_load(&key_queue.read) != atomic_load(&key_queue.written))
        ;
    debug = false;

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

// ---- dvars: a made-up dvar table the game's Dvar_FindByHash and Dvar_SetFromStringByName work on

typedef struct {
    const char *name;
    uint8_t dvar[0x60];          // dvar_t: flags at +0x18, type at +0x1c, a per-mode dvar's three at +0x28
    uint8_t modes[3][0x60];      // a per-mode dvar's own ones
    char value[32];
    int sets;
} game_dvar_t;
static game_dvar_t game_dvars[8];
static int game_dvar_count;
static int unknown_sets; // sets of names the table doesn't have (created by the game)

static game_dvar_t *add_dvar(const char *name, uint32_t type, uint32_t flags) {
    game_dvar_t *d = &game_dvars[game_dvar_count++];
    memset(d, 0, sizeof(*d));
    d->name = name;
    memcpy(d->dvar + 0x18, &flags, sizeof(flags));
    memcpy(d->dvar + 0x1c, &type, sizeof(type));
    return d;
}
static void set_mode_dvars(game_dvar_t *d, bool zombies, bool multiplayer, bool campaign, uint32_t flags) {
    bool has[3] = { zombies, multiplayer, campaign };
    for (int mode = 0; mode < 3; mode++) {
        uint8_t *own = has[mode] ? d->modes[mode] : NULL;
        memcpy(d->dvar + 0x28 + 8 * mode, &own, sizeof(own));
        memcpy(d->modes[mode] + 0x18, &flags, sizeof(flags));
    }
}
static uint32_t flags_of(const uint8_t *dvar) {
    uint32_t flags;
    memcpy(&flags, dvar + 0x18, sizeof(flags));
    return flags;
}
static void *game_find_dvar(uint32_t hash) {
    for (int i = 0; i < game_dvar_count; i++) {
        if (gsc_hash(game_dvars[i].name) == hash)
            return game_dvars[i].dvar;
    }
    return NULL;
}
static void game_set_dvar(const char *name, const char *value, bool create_if_missing) {
    CHECK(create_if_missing);
    for (int i = 0; i < game_dvar_count; i++) {
        if (strcmp(game_dvars[i].name, name) == 0) {
            snprintf(game_dvars[i].value, sizeof(game_dvars[i].value), "%s", value);
            game_dvars[i].sets++;
            return;
        }
    }
    unknown_sets++;
}
static uint32_t game_session_mode;

static void wire_dvars(void) {
    game_dvar_count = 0;
    unknown_sets = 0;
    Dvar_FindByHash = game_find_dvar;
    Dvar_SetFromStringByName = game_set_dvar;
    session_mode_state = &game_session_mode;
}

// the scheduler: what was asked for
static int scheduled, scheduled_seconds;
static void (*scheduled_work)(void *);
static void game_schedule(int seconds, void (*work)(void *)) {
    scheduled++;
    scheduled_seconds = seconds;
    scheduled_work = work;
}

static void test_session_mode(void) {
    CHECK(t7o_session_mode(0) == 0 && t7o_session_mode(1) == 1 && t7o_session_mode(2) == 2);
    CHECK(t7o_session_mode(3) == 3);
    CHECK(t7o_session_mode(0x40 | 2) == 2);   // the network bits above don't count
    CHECK(t7o_session_mode(0xfffffff0) == 0);
    CHECK(t7o_session_mode(0x7) == 7 && t7o_session_mode(0x8) == -8 && t7o_session_mode(0xf) == -1); // with its sign

    uint8_t plain[0x60], per_mode[0x60], zombies[0x60], campaign[0x60];
    memset(plain, 0, sizeof(plain));
    memset(per_mode, 0, sizeof(per_mode));
    uint32_t type = 5;
    memcpy(plain + 0x1c, &type, sizeof(type));
    CHECK(t7o_mode_dvar(plain, 0) == plain && t7o_mode_dvar(plain, 3) == plain && t7o_mode_dvar(plain, -1) == plain);
    type = 15;
    memcpy(per_mode + 0x1c, &type, sizeof(type));
    uint8_t *modes[3] = { zombies, NULL, campaign };
    memcpy(per_mode + 0x28, modes, sizeof(modes));
    CHECK(t7o_mode_dvar(per_mode, 0) == zombies);
    CHECK(t7o_mode_dvar(per_mode, 1) == per_mode); // a mode without its own: the dvar itself
    CHECK(t7o_mode_dvar(per_mode, 2) == campaign);
    CHECK(t7o_mode_dvar(per_mode, 3) == NULL && t7o_mode_dvar(per_mode, -1) == NULL);
    CHECK(t7o_mode_dvar(per_mode, 7) == NULL);
    type = 14;
    memcpy(per_mode + 0x1c, &type, sizeof(type));
    CHECK(t7o_mode_dvar(per_mode, 0) == per_mode);
}

static void test_network_defaults(void) {
    // T7Overcharged's five, in its order
    CHECK(t7o_network_default_count == 5);
    const t7o_dvar_change *d = t7o_network_defaults;
    CHECK(strcmp(d[0].name, "sv_maxRate") == 0 && d[0].set_flags && d[0].flags == 0 &&
          strcmp(d[0].value, "50000") == 0);
    CHECK(strcmp(d[1].name, "sv_network_fps") == 0 && d[1].set_flags && d[1].flags == 0 &&
          strcmp(d[1].value, "125") == 0);
    CHECK(strcmp(d[2].name, "g_allowvote") == 0 && !d[2].set_flags && strcmp(d[2].value, "0") == 0);
    CHECK(strcmp(d[3].name, "sv_mapswitch") == 0 && !d[3].set_flags && strcmp(d[3].value, "0") == 0);
    CHECK(strcmp(d[4].name, "maxvoicepacketsperframe") == 0 && !d[4].set_flags && strcmp(d[4].value, "0") == 0);

    // against the made-up game: the Mac's flags, values and a fourth dvar the table doesn't have
    wire_dvars();
    schedule_after = game_schedule;
    game_dvar_t *rate = add_dvar("sv_maxRate", 5, 0x0), *fps = add_dvar("sv_network_fps", 5, 0x1000);
    game_dvar_t *vote = add_dvar("g_allowvote", 0, 0x5);
    add_dvar("sv_mapswitch", 0, 0x100);
    game_session_mode = 3;
    mod_loaded_once = false;
    session_mode_state = NULL;
    t7o_small_mod_loaded(); // before the install: nothing
    CHECK(rate->sets == 0 && !mod_loaded_once);
    session_mode_state = &game_session_mode;
    t7o_small_mod_loaded();
    CHECK(flags_of(rate->dvar) == 0 && flags_of(fps->dvar) == 0 && flags_of(vote->dvar) == 0x5);
    CHECK(strcmp(rate->value, "50000") == 0 && strcmp(fps->value, "125") == 0 && strcmp(vote->value, "0") == 0);
    CHECK(strcmp(game_dvars[3].value, "0") == 0 && flags_of(game_dvars[3].dvar) == 0x100);
    CHECK(unknown_sets == 1); // maxvoicepacketsperframe, created as with create_if_missing
    // and the dvar flags are asked for 10 s later
    CHECK(scheduled == 1 && scheduled_seconds == 10 && scheduled_work == apply_dvar_patches);
    // once, as T7Overcharged's components start once
    memcpy(fps->dvar + 0x18, &(uint32_t){ 0x1000 }, sizeof(uint32_t));
    t7o_small_mod_loaded();
    CHECK(flags_of(fps->dvar) == 0x1000 && rate->sets == 1 && scheduled == 1);
    // with the debug lines, and a per-mode one in a mode it has
    wire_dvars();
    game_dvar_t *per_mode = add_dvar("sv_maxRate", 15, 0x100);
    set_mode_dvars(per_mode, true, false, true, 0x101);
    debug = true;
    game_session_mode = 0;
    apply_dvar_changes("network defaults", t7o_network_defaults, t7o_network_default_count);
    CHECK(flags_of(per_mode->modes[0]) == 0 && flags_of(per_mode->modes[2]) == 0x101);
    CHECK(flags_of(per_mode->dvar) == 0x100);
    game_session_mode = 1; // a mode without its own: the dvar itself
    apply_dvar_changes("network defaults", t7o_network_defaults, t7o_network_default_count);
    CHECK(flags_of(per_mode->dvar) == 0 && flags_of(per_mode->modes[2]) == 0x101);
    memcpy(per_mode->dvar + 0x18, &(uint32_t){ 0x100 }, sizeof(uint32_t));
    game_session_mode = 3; // no mode: left alone, the value is still set
    per_mode->sets = 0;
    apply_dvar_changes("network defaults", t7o_network_defaults, t7o_network_default_count);
    CHECK(flags_of(per_mode->dvar) == 0x100 && per_mode->sets == 1);
    debug = false;
}

static void test_dvar_patches(void) {
    // T7Overcharged's 35, in its order, with its flags; values for three
    CHECK(t7o_dvar_patch_count == 35);
    static const struct {
        const char *name;
        uint32_t flags;
        const char *value;
    } want[] = {
        { "cg_fovscale", 1, NULL }, { "ui_enabled", 1, NULL }, { "fx_drawclouds", 0, NULL }, { "fx_enable", 0, NULL },
        { "gpad_stick_deadzone_max", 1, NULL }, { "gpad_stick_deadzone_min", 1, NULL }, { "r_dof_enable", 0, NULL },
        { "r_fog", 0, NULL }, { "r_glow_allowed", 0, NULL }, { "r_lodbiasrigid", 0, NULL },
        { "r_lodThresholdPixelArea", 0, NULL }, { "r_spotLightEntityShadows", 0, NULL },
        { "r_viewModelSelfShadow", 0, NULL }, { "r_viewModelSpotOmniShadowResolution", 0, NULL },
        { "r_viewModelSunShadowResolution", 0, NULL }, { "r_volumetric_lighting_mode", 0, NULL }, { "r_zfar", 0, NULL },
        { "ui_error_callstack_ship", 0, "1" }, { "ui_error_report_delay", 0, "0" }, { "com_disable_popups", 0, "0" },
        { "doublejump_enabled", 0x40, NULL }, { "juke_enabled", 0x40, NULL }, { "mantle_enabled", 0x40, NULL },
        { "playerEnergy_enabled", 0x40, NULL }, { "sprintLeap_enabled", 0x40, NULL }, { "traverse_mode", 0x40, NULL },
        { "trm_enabled", 0x40, NULL }, { "steamLobby_frameEnable", 0, NULL }, { "steamP2P_Enable", 0, NULL },
        { "live_presence_platform", 0, NULL }, { "live_enableCounters", 0, NULL },
        { "lobbyAdvertiseShowInMatchmaking", 0, NULL }, { "lobbySearchShowInMatchmaking", 0, NULL },
        { "groups_enabled", 0, NULL }, { "noDW", 0, NULL },
    };
    CHECK(sizeof(want) / sizeof(want[0]) == t7o_dvar_patch_count);
    for (size_t i = 0; i < t7o_dvar_patch_count && i < sizeof(want) / sizeof(want[0]); i++) {
        const t7o_dvar_change *p = &t7o_dvar_patches[i];
        CHECK(strcmp(p->name, want[i].name) == 0 && p->set_flags && p->flags == want[i].flags);
        CHECK((p->value == NULL) == (want[i].value == NULL));
        CHECK(p->value == NULL || want[i].value == NULL || strcmp(p->value, want[i].value) == 0);
    }

    // against the made-up game, when the scheduled work runs: cheat protected ones lose it, a per-mode one gets this
    // mode's changed, the three values set after their flags
    wire_dvars();
    game_dvar_t *fov = add_dvar("cg_fovscale", 1, 0x100), *fog = add_dvar("r_fog", 0, 0x110);
    game_dvar_t *trm = add_dvar("trm_enabled", 15, 0x0);
    set_mode_dvars(trm, true, true, false, 0x104);
    game_dvar_t *ship = add_dvar("ui_error_callstack_ship", 0, 0x100);
    game_dvar_t *popups = add_dvar("com_disable_popups", 0, 0x1);
    game_session_mode = 0x40; // zombies, played over a LAN
    apply_dvar_patches(NULL);
    CHECK(flags_of(fov->dvar) == 0x1 && flags_of(fog->dvar) == 0x0);
    CHECK(flags_of(trm->modes[0]) == 0x40 && flags_of(trm->modes[1]) == 0x104 && flags_of(trm->dvar) == 0);
    CHECK(flags_of(ship->dvar) == 0 && strcmp(ship->value, "1") == 0 && ship->sets == 1);
    CHECK(flags_of(popups->dvar) == 0 && strcmp(popups->value, "0") == 0);
    CHECK(fov->sets == 0 && fog->sets == 0 && trm->sets == 0); // the others only get their flags
    CHECK(unknown_sets == 1); // ui_error_report_delay, created as with create_if_missing
}

// cursoroverride: there, says its usage without a file, logs the first file once and changes nothing else
static void test_cursoroverride(void) {
    cursoroverride_logged = false;
    const char *bare[] = { "cursoroverride" };
    run_command(cursoroverride_f, ARGC(bare), bare);
    CHECK(!cursoroverride_logged);
    run_command(cursoroverride_f, 0, NULL);
    CHECK(!cursoroverride_logged);
    const char *nine[] = { "cursoroverride", "9.ani" };
    run_command(cursoroverride_f, ARGC(nine), nine);
    CHECK(cursoroverride_logged);
    const char *other[] = { "cursoroverride", "12.ani" };
    run_command(cursoroverride_f, ARGC(other), other);
    CHECK(cursoroverride_logged);
}

// DB_FreeUnusedResources: T7Overcharged's arguments, and the Mac's function with the second flag
static int freed;
static bool freed_with, freed_locked;
static void game_free_unused(bool second) {
    freed++;
    freed_with = second;
    // with the zone function's two critical sections, in its order
    freed_locked = held_count == 2 && held_locks[0] == 0x28 && held_locks[1] == 0x60;
}

static void test_free_unused(void) {
    bool first = true, second = false;
    const char *lobby[] = { "DB_FreeUnusedResources", "0", "1" }; // AAE's lobby
    CHECK(t7o_free_unused_args(ARGC(lobby), lobby, &first, &second) == T7o_Number_Ok && !first && second);
    const char *bare[] = { "DB_FreeUnusedResources" };
    first = true, second = false;
    CHECK(t7o_free_unused_args(ARGC(bare), bare, &first, &second) == T7o_Number_Ok && !first && second);
    const char *one[] = { "DB_FreeUnusedResources", "1" }; // fewer than three words: the one number isn't read
    first = true, second = false;
    CHECK(t7o_free_unused_args(ARGC(one), one, &first, &second) == T7o_Number_Ok && !first && second);
    const char *other[] = { "DB_FreeUnusedResources", "2", "0" };
    CHECK(t7o_free_unused_args(ARGC(other), other, &first, &second) == T7o_Number_Ok && first && !second);
    const char *negative[] = { "DB_FreeUnusedResources", "-1", "0x" };
    CHECK(t7o_free_unused_args(ARGC(negative), negative, &first, &second) == T7o_Number_Ok && first && !second);
    first = false, second = false;
    const char *word[] = { "DB_FreeUnusedResources", "0", "yes" };
    CHECK(t7o_free_unused_args(ARGC(word), word, &first, &second) == T7o_Number_Invalid && !first && !second);
    const char *word1[] = { "DB_FreeUnusedResources", "x", "1" };
    CHECK(t7o_free_unused_args(ARGC(word1), word1, &first, &second) == T7o_Number_Invalid && !second);
    const char *huge[] = { "DB_FreeUnusedResources", "0", "9999999999" };
    CHECK(t7o_free_unused_args(ARGC(huge), huge, &first, &second) == T7o_Number_Range);
    CHECK(t7o_free_unused_args(3, NULL, &first, &second) == T7o_Number_Ok && !first && second);

    DB_FreeUnusedResources = game_free_unused;
    CritSec_ScopedLock = game_critical_section_enter;
    CritSec_ScopedUnlock = game_critical_section_leave;
    freed = 0;
    run_command(freeunused_f, ARGC(lobby), lobby);
    CHECK(freed == 1 && freed_with && freed_locked && held_count == 0); // and both left again
    run_command(freeunused_f, ARGC(other), other);
    CHECK(freed == 2 && !freed_with);
    run_command(freeunused_f, ARGC(word), word);
    run_command(freeunused_f, 0, NULL);
    CHECK(freed == 2);
    debug = true;
    run_command(freeunused_f, ARGC(bare), bare);
    debug = false;
    CHECK(freed == 3 && freed_with);

    // updateVidConfig: there, and nothing else
    updatevidconfig_logged = false;
    const char *vid[] = { "updateVidConfig" };
    run_command(updatevidconfig_f, ARGC(vid), vid);
    CHECK(updatevidconfig_logged && freed == 3);
}

int main(void) {
    test_stoi();
    test_keyset();
    test_keymap_command();
    test_keymap_event_and_find();
    test_keymap_debounce();
    test_key_queue();
    test_keymap_fire();
    test_keymap_game();
    test_menuresponse();
    test_session_mode();
    test_network_defaults();
    test_cursoroverride();
    test_dvar_patches();
    test_free_unused();
    if (failures == 0)
        printf("test_t7o_small: all passed\n");
    return failures == 0 ? 0 : 1;
}
