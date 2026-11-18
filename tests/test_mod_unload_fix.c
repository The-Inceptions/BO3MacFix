// mod_unload_fix.c: which loads get a copy, that the copy outlives the zone's bytes, that copies are reused only for
// the same bytes from the same place of the same VM, that they are freed with their VM and only after the game closed
// it, the setting off and the two hooks' places, and the log (the test includes the source itself).
#include <stdio.h>
#include <unistd.h>
#include "../source/mod_unload_fix.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- the made-up game

// a lua_State: HavokScript's global state (shared by all the VM's threads) at +0x10
typedef struct {
    uint8_t pad[0x10];
    void *global;
    uint8_t rest[0x50];
} fake_state_t;

// a Lua file asset as the game's zones have it
typedef struct {
    const char *name;
    int32_t length;
    int32_t pad;
    const char *bytes;
} fake_luafile_t;
_Static_assert(offsetof(fake_luafile_t, length) == 8 && offsetof(fake_luafile_t, bytes) == 0x10, "a Lua file asset");

static uint8_t lobby_global[64], lobby2_global[64], ui_global[64];
static fake_state_t lobby_state_main, lobby_thread, lobby2_state, ui_state;
static lua_State *game_lobby_state; // the game's pointer to its lobby VM

// the game's reader: the asset's bytes in one piece, its length in *size; NULL for no asset or an empty one
static int reader_calls;
static const char *reader_answer_override; // when set, the reader hands this over whatever the asset says
static const char *game_reader(lua_State *s, const void *file, size_t *size) {
    (void)s;
    reader_calls++;
    if (file == NULL)
        return NULL;
    const fake_luafile_t *f = file;
    *size = (size_t)(int64_t)f->length;
    if (reader_answer_override != NULL)
        return reader_answer_override;
    return f->length != 0 ? f->bytes : NULL;
}

// the game's lua_close: the VM's memory goes with it, so its state's global pointer is gone afterwards; while it
// closes, the VM's own code (finalizers) may still run, so the lobby's copies must still be there
static int close_calls;
static lua_State *closed_state;
static const char *must_be_readable; // a kept copy the close reads, as a finalizer running the VM's code would
static char must_hold[64];
static void game_lua_close(lua_State *s) {
    close_calls++;
    closed_state = s;
    if (must_be_readable != NULL)
        CHECK(memcmp(must_be_readable, must_hold, strlen(must_hold)) == 0);
    if (s != NULL)
        ((fake_state_t *)(void *)s)->global = (void *)(uintptr_t)0xdead0000;
}

// the hooks BO3MacFix sets: the addresses, and the game's own functions they lead to
static uint64_t base;
static int hooks_set;
static void *hooked_reader_at, *hooked_close_at;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    hooks_set++;
    if (fake_func == (void *)fake_LuaFile_Reader) {
        hooked_reader_at = address;
        *out_origin_func = (void *)game_reader;
    } else if (fake_func == (void *)fake_hksi_lua_close) {
        hooked_close_at = address;
        *out_origin_func = (void *)game_lua_close;
    } else {
        *out_origin_func = NULL;
    }
    return 0;
}

// the memory the copies are made in: counted, and able to run out
static int allocations, releases, allocations_left = -1;
static void *test_allocate(size_t size) {
    if (allocations_left == 0)
        return NULL;
    if (allocations_left > 0)
        allocations_left--;
    allocations++;
    return malloc(size != 0 ? size : 1);
}
static void test_release(void *memory) {
    if (memory != NULL)
        releases++;
    free(memory);
}

// what the piece prints, caught from stdout
static char printed[8192];
static int saved_stdout = -1;
static FILE *capture;
static void capture_begin(void) {
    fflush(stdout);
    capture = tmpfile();
    saved_stdout = dup(1);
    dup2(fileno(capture), 1);
}
static void capture_end(void) {
    fflush(stdout);
    dup2(saved_stdout, 1);
    close(saved_stdout);
    rewind(capture);
    size_t n = fread(printed, 1, sizeof(printed) - 1, capture);
    printed[n] = '\0';
    fclose(capture);
}
static int count_lines(const char *text) {
    int n = 0;
    for (const char *p = printed; (p = strstr(p, text)) != NULL; p += strlen(text))
        n++;
    return n;
}

static lua_State *as_state(fake_state_t *state) {
    return (lua_State *)(void *)state;
}

// a load as the game's parser asks the reader for a file: the bytes it gets, and the size
static const char *load(fake_state_t *state, const fake_luafile_t *file, size_t *size) {
    *size = 0x5a5a; // the parser's variable before the call
    return fake_LuaFile_Reader(as_state(state), file, size);
}

static void reset_game(void) {
    lobby_state_main.global = lobby_global;
    lobby_thread.global = lobby_global;
    lobby2_state.global = lobby2_global;
    ui_state.global = ui_global;
    game_lobby_state = as_state(&lobby_state_main);
    reader_calls = close_calls = 0;
    closed_state = NULL;
    must_be_readable = NULL;
    reader_answer_override = NULL;
}

// ---- the pure parts

static void test_is_lobby(void) {
    int a = 0, b = 0;
    CHECK(mod_unload_is_lobby(&a, &a));
    CHECK(!mod_unload_is_lobby(&a, &b));
    CHECK(!mod_unload_is_lobby(&a, NULL));
    CHECK(!mod_unload_is_lobby(NULL, NULL)); // no lobby VM yet: nothing counts as it, not even a state without one
}

static void test_keep_and_release(void) {
    mod_unload_kept_t k = { .allocate = test_allocate, .release = test_release };
    int vm1 = 0, vm2 = 0;
    char zone[200][32];
    const char *copies[200];
    memset(zone, 0, sizeof(zone));
    allocations = releases = 0;
    // 150 files in two VMs: the table grows past its first 64 and keeps every record
    for (int i = 0; i < 150; i++) {
        snprintf(zone[i], sizeof(zone[i]), "file %d of the zone", i);
        bool made = false;
        copies[i] = mod_unload_keep(&k, i % 3 == 0 ? (void *)&vm2 : (void *)&vm1, zone[i], 32, &made);
        CHECK(made && copies[i] != NULL && copies[i] != zone[i] && memcmp(copies[i], zone[i], 32) == 0);
    }
    CHECK(k.count == 150 && k.bytes == 150 * 32 && k.capacity >= 150);
    // the zone goes: every copy keeps its bytes
    char expected[32];
    for (int i = 0; i < 150; i++) {
        snprintf(expected, sizeof(expected), "file %d of the zone", i);
        memset(zone[i], 0xcc, sizeof(zone[i]));
        CHECK(memcmp(copies[i], expected, strlen(expected) + 1) == 0);
    }
    // the same bytes from the same place again: the same copy, nothing made
    for (int i = 0; i < 150; i++) {
        memset(zone[i], 0, sizeof(zone[i]));
        snprintf(zone[i], sizeof(zone[i]), "file %d of the zone", i);
    }
    int before = allocations;
    bool made = true;
    CHECK(mod_unload_keep(&k, &vm1, zone[1], 32, &made) == copies[1] && !made && allocations == before);
    // not for another VM, another size, another place, or other bytes at the same place
    CHECK(mod_unload_keep(&k, &vm2, zone[1], 32, &made) != copies[1] && made);
    CHECK(mod_unload_keep(&k, &vm1, zone[1], 31, &made) != copies[1] && made);
    memcpy(zone[199], zone[1], 32);
    CHECK(mod_unload_keep(&k, &vm1, zone[199], 32, &made) != copies[1] && made);
    zone[1][0] = 'F';
    const char *changed = mod_unload_keep(&k, &vm1, zone[1], 32, &made);
    CHECK(changed != copies[1] && made && changed[0] == 'F' && copies[1][0] == 'f');
    size_t count = k.count, bytes = k.bytes;
    CHECK(count == 154 && bytes == 150 * 32 + 32 + 31 + 32 + 32);
    // vm2's go (every third, and the one above), vm1's stay, in their order
    size_t freed_bytes = 0;
    int released_before = releases;
    size_t freed = mod_unload_release(&k, &vm2, &freed_bytes);
    CHECK(freed == 51 && freed_bytes == 51 * 32 && releases - released_before == 51);
    CHECK(k.count == count - 51 && k.bytes == bytes - 51 * 32);
    size_t at = 0;
    for (int i = 0; i < 150; i++) {
        if (i % 3 == 0)
            continue;
        CHECK(at < k.count && k.files[at].vm == &vm1 && k.files[at].copy == copies[i]);
        at++;
    }
    // a VM with nothing kept: nothing happens
    freed = mod_unload_release(&k, &vm2, &freed_bytes);
    CHECK(freed == 0 && freed_bytes == 0 && k.count == count - 51);
    freed = mod_unload_release(&k, &vm1, &freed_bytes);
    // every copy is released, and the two tables the growth replaced; the last table is still there
    CHECK(freed == count - 51 && k.count == 0 && k.bytes == 0 && releases == allocations - 1);
    test_release(k.files);
}

static void test_keep_without_memory(void) {
    mod_unload_kept_t k = { .allocate = test_allocate, .release = test_release };
    int vm = 0;
    char zone[16] = "some lua";
    bool made = true;
    allocations_left = 0; // not even the table
    CHECK(mod_unload_keep(&k, &vm, zone, sizeof(zone), &made) == NULL && !made && k.count == 0);
    allocations_left = 1; // the table, but not the copy
    CHECK(mod_unload_keep(&k, &vm, zone, sizeof(zone), &made) == NULL && !made && k.count == 0 && k.bytes == 0);
    allocations_left = -1;
    const char *copy = mod_unload_keep(&k, &vm, zone, sizeof(zone), &made);
    CHECK(copy != NULL && made && k.count == 1 && memcmp(copy, zone, sizeof(zone)) == 0);
    size_t bytes;
    mod_unload_release(&k, &vm, &bytes);
    test_release(k.files);
}

// ---- the hooks against the made-up game

static void test_setting_off(void) {
    hooks_set = 0;
    mod_unload_fix_init(0x100000000ULL, 0);
    CHECK(hooks_set == 0 && lobby_state == NULL);
}

static void test_hooks(void) {
    hooks_set = 0;
    base = (uint64_t)(uintptr_t)&game_lobby_state - ADDR_LobbyVM_State;
    mod_unload_fix_init(base, 1);
    CHECK(hooks_set == 2);
    CHECK(hooked_reader_at == (void *)(uintptr_t)(base + ADDR_LuaFile_Reader));
    CHECK(hooked_close_at == (void *)(uintptr_t)(base + ADDR_hksi_lua_close));
    CHECK(lobby_state == (lua_State *const *)&game_lobby_state);
    CHECK(debug == 0);
    kept.allocate = test_allocate;
    kept.release = test_release;
}

static char zone_a[19639], zone_b[4000], zone_c[100];
static fake_luafile_t file_a = { "lua/shared/luareadonlytables.lua", sizeof(zone_a), 0, zone_a };
static fake_luafile_t file_b = { "lua/lobby/lobby.lua", sizeof(zone_b), 0, zone_b };
static fake_luafile_t file_c = { "ui/uieditor/menus/main.lua", sizeof(zone_c), 0, zone_c };

static void fill(char *zone, size_t size, int seed) {
    for (size_t i = 0; i < size; i++)
        zone[i] = (char)(i * 7 + (size_t)seed);
}

static void test_loads(void) {
    reset_game();
    fill(zone_a, sizeof(zone_a), 1);
    fill(zone_b, sizeof(zone_b), 2);
    fill(zone_c, sizeof(zone_c), 3);
    static char saved_a[sizeof(zone_a)];
    memcpy(saved_a, zone_a, sizeof(zone_a));
    allocations = releases = 0;

    // the lobby's VM loads the mod's file: it gets a copy with the same bytes, the game's reader asked once
    size_t size;
    capture_begin();
    const char *a = load(&lobby_state_main, &file_a, &size);
    capture_end();
    CHECK(reader_calls == 1 && size == sizeof(zone_a));
    CHECK(a != NULL && a != zone_a && memcmp(a, saved_a, sizeof(zone_a)) == 0);
    CHECK(kept.count == 1 && kept.bytes == sizeof(zone_a));
    CHECK(count_lines("lobby lua: the lobby's Lua runs from copies of the files it loads") == 1);
    CHECK(count_lines("ModUnloadFix") == 1 && count_lines("kept lua/") == 0); // no debug lines without the switch

    // asked again for the same file in the same load: the same copy, nothing more made
    int made_before = allocations;
    capture_begin();
    CHECK(load(&lobby_state_main, &file_a, &size) == a && size == sizeof(zone_a) && allocations == made_before);
    // one of the lobby VM's threads (a coroutine): the same VM, so the same copy
    CHECK(load(&lobby_thread, &file_a, &size) == a && reader_calls == 3);
    // another lobby file: a copy of its own; the first line isn't said again
    const char *b = load(&lobby_thread, &file_b, &size);
    capture_end();
    CHECK(b != NULL && b != zone_b && size == sizeof(zone_b) && memcmp(b, zone_b, sizeof(zone_b)) == 0);
    CHECK(kept.count == 2 && kept.bytes == sizeof(zone_a) + sizeof(zone_b) && printed[0] == '\0');

    // the UI's VM reads the game's own bytes, as without this
    int count = (int)kept.count;
    CHECK(load(&ui_state, &file_c, &size) == zone_c && size == sizeof(zone_c) && (int)kept.count == count);
    CHECK(load(&ui_state, &file_a, &size) == zone_a && size == sizeof(zone_a));

    // no answer, or an empty one, from the game: handed on as it is
    CHECK(load(&lobby_state_main, NULL, &size) == NULL && size == 0x5a5a);
    fake_luafile_t empty = { "lua/lobby/empty.lua", 0, 0, zone_b };
    CHECK(load(&lobby_state_main, &empty, &size) == NULL && size == 0);
    reader_answer_override = zone_b; // an answer with no bytes in it
    CHECK(load(&lobby_state_main, &empty, &size) == zone_b && size == 0 && (int)kept.count == count);
    reader_answer_override = NULL;

    // while the game has no lobby VM (before it is made, or between its close and the new one) nobody's load counts
    game_lobby_state = NULL;
    CHECK(load(&lobby_state_main, &file_b, &size) == zone_b && (int)kept.count == count);
    game_lobby_state = as_state(&lobby_state_main);

    // the mod is unloaded: the zone's bytes go (here: are overwritten), the lobby's copies keep theirs
    memset(zone_a, 0xcc, sizeof(zone_a));
    CHECK(memcmp(a, saved_a, sizeof(zone_a)) == 0);
    // the zone's memory holds another file at the same place now: that one gets a new copy, the old one stays
    fill(zone_a, sizeof(zone_a), 9);
    const char *a2 = load(&lobby_state_main, &file_a, &size);
    CHECK(a2 != NULL && a2 != a && memcmp(a2, zone_a, sizeof(zone_a)) == 0 && memcmp(a, saved_a, sizeof(zone_a)) == 0);
    CHECK(kept.count == 3);

    // the game closes the UI's VM: the game's close runs, the lobby's copies stay
    int released = releases;
    capture_begin();
    fake_hksi_lua_close(as_state(&ui_state));
    capture_end();
    CHECK(close_calls == 1 && closed_state == as_state(&ui_state));
    CHECK(kept.count == 3 && releases == released && printed[0] == '\0');
    CHECK(memcmp(a, saved_a, sizeof(zone_a)) == 0);

    // a second lobby VM while the first isn't closed (the game never does this; a restart closes first): copies of
    // its own, which the first one's close leaves alone
    game_lobby_state = as_state(&lobby2_state);
    fill(zone_b, sizeof(zone_b), 2);
    const char *b2 = load(&lobby2_state, &file_b, &size);
    CHECK(b2 != NULL && b2 != b && kept.count == 4);

    // the game closes the first lobby VM: its close runs first, with the copies still there (its finalizers may run
    // its code), then its copies are freed, and only its own
    must_be_readable = a;
    memcpy(must_hold, saved_a, sizeof(must_hold) - 1);
    must_hold[sizeof(must_hold) - 1] = '\0';
    if (strlen(must_hold) == 0) // the made-up bytes start with 1, but be sure the check reads something
        must_hold[0] = saved_a[0];
    capture_begin();
    fake_hksi_lua_close(as_state(&lobby_state_main));
    capture_end();
    must_be_readable = NULL;
    CHECK(close_calls == 2 && closed_state == as_state(&lobby_state_main));
    CHECK(kept.count == 1 && kept.files[0].copy == b2 && releases == released + 3);
    CHECK(memcmp(b2, zone_b, sizeof(zone_b)) == 0);
    CHECK(count_lines("lobby lua: the game closed the lobby's Lua, the copies of its 3 files (") == 1);

    // the second lobby VM is closed too; a lobby VM made anew at the first one's place (the game's lobby heap is one
    // block, so its global state can be at the same address) loads the same file: a new copy, the old ones are gone
    fake_hksi_lua_close(as_state(&lobby2_state));
    CHECK(kept.count == 0 && kept.bytes == 0 && releases == released + 4);
    lobby_state_main.global = lobby_global;
    game_lobby_state = as_state(&lobby_state_main);
    const char *a3 = load(&lobby_state_main, &file_a, &size);
    CHECK(a3 != NULL && kept.count == 1 && memcmp(a3, zone_a, sizeof(zone_a)) == 0);
    fake_hksi_lua_close(as_state(&lobby_state_main));
    CHECK(kept.count == 0);

    // lua_close(NULL), which the game never does: handed on, nothing else
    close_calls = 0;
    fake_hksi_lua_close(NULL);
    CHECK(close_calls == 1 && closed_state == NULL);
}

static void test_no_memory(void) {
    reset_game();
    fill(zone_b, sizeof(zone_b), 4);
    size_t size;
    allocations_left = 0;
    said_no_memory = false;
    capture_begin();
    CHECK(load(&lobby_state_main, &file_b, &size) == zone_b && size == sizeof(zone_b) && kept.count == 0);
    CHECK(load(&lobby_state_main, &file_a, &size) == zone_a && kept.count == 0);
    capture_end();
    CHECK(count_lines("lobby lua: no memory for a copy of a file of the lobby's Lua") == 1);
    allocations_left = -1;
    const char *b = load(&lobby_state_main, &file_b, &size);
    CHECK(b != NULL && b != zone_b && kept.count == 1);
    fake_hksi_lua_close(as_state(&lobby_state_main));
    CHECK(kept.count == 0);
}

static void test_debug_lines(void) {
    reset_game();
    debug = Debug_Lines;
    size_t size;
    capture_begin();
    const char *a = load(&lobby_state_main, &file_a, &size);
    load(&lobby_state_main, &file_a, &size); // the same copy: no second line
    capture_end();
    char line[160];
    snprintf(line, sizeof(line), "lobby lua: kept lua/shared/luareadonlytables.lua (19639 bytes) from %p at %p (1 files, "
             "19 KB)", (void *)zone_a, (const void *)a);
    CHECK(count_lines(line) == 1 && count_lines("lobby lua: kept ") == 1);
    // a second file: the count and the total so far
    fill(zone_b, sizeof(zone_b), 5);
    capture_begin();
    const char *b = load(&lobby_state_main, &file_b, &size);
    capture_end();
    snprintf(line, sizeof(line), "lobby lua: kept lua/lobby/lobby.lua (4000 bytes) from %p at %p (2 files, 23 KB)",
             (void *)zone_b, (const void *)b);
    CHECK(count_lines(line) == 1);
    fake_hksi_lua_close(as_state(&lobby_state_main));

    // =2: where the game has each file, and the lobby reads the game's own bytes (for a run that asks that question)
    reset_game();
    debug = Debug_Where;
    capture_begin();
    CHECK(load(&lobby_state_main, &file_a, &size) == zone_a && size == sizeof(zone_a) && kept.count == 0);
    CHECK(load(&ui_state, &file_c, &size) == zone_c);
    capture_end();
    snprintf(line, sizeof(line), "lobby lua: lua/shared/luareadonlytables.lua (19639 bytes) at %p to %p, read where",
             (void *)zone_a, (void *)(zone_a + sizeof(zone_a)));
    CHECK(count_lines(line) == 1 && count_lines("lobby lua: ") == 1); // the UI's load says nothing
    debug = 0;
}

int main(void) {
    test_is_lobby();
    test_keep_and_release();
    test_keep_without_memory();
    test_setting_off();
    test_hooks();
    test_loads();
    test_no_memory();
    test_debug_lines();
    if (failures != 0) {
        printf("test_mod_unload_fix: %d failures\n", failures);
        return 1;
    }
    printf("test_mod_unload_fix: all passed\n");
    return 0;
}
