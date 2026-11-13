// aae_clientfx.c: T7Overcharged's rules for its table of client effects, and the six hooks against a made-up cache
// whose generic functions (find, add, let go) do what the game's do. The test includes the source itself.
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/aae_clientfx.c"

// ---- the made-up game: two copies of the cache, and effects to register

static uint8_t *cache; // what ADDR_bgCache is in the game, 2 * Cache_Side_Size bytes
static uint64_t base;  // so that base + ADDR_bgCache is the made-up cache
static aae_clientfx_entry_t *game_side(uint32_t side) {
    return (aae_clientfx_entry_t *)(cache + (uint64_t)side * Cache_Side_Size + Cache_ClientFx);
}

// an effect is an asset whose first field is its name
typedef struct {
    const char *name;
    char text[40];
} effect_t;
static effect_t effects[4200];
static int effect_count;
static const void *effect_named(const char *name) {
    for (int i = 0; i < effect_count; i++)
        if (strcmp(effects[i].text, name) == 0)
            return &effects[i];
    effect_t *made = &effects[effect_count++];
    snprintf(made->text, sizeof(made->text), "%s", name);
    made->name = made->text;
    return made;
}

// what the game printed last, and how often it stopped a map for a full table
static char printed[200];
static int prints, exceeded, exceeded_count, dirty;
static void game_printf(int32_t kind, int32_t channel, const char *format, ...) {
    va_list list;
    va_start(list, format);
    vsnprintf(printed, sizeof(printed), format, list);
    va_end(list);
    CHECK(kind == 8 && channel == 0x17);
    prints++;
}

// the game's hash of a name: 5381, then * 33 + each letter in lower case
static uint32_t game_hash(const char *name) {
    if (name == NULL)
        return 0;
    uint32_t hash = 5381;
    for (; *name != '\0'; name++)
        hash = hash * 33 + (uint32_t)tolower((unsigned char)*name);
    return hash;
}

// BG_Cache_GetIndexInternal: entry 0 is never used; the first free entry ends the search
static int game_get_index(uint32_t side, uint32_t kind, aae_clientfx_entry_t *table, int32_t count, const char *name,
                          bool free_entry) {
    (void)side; // passed on as the caller gave it, by the game's functions and T7Overcharged's alike
    CHECK(kind == Type_ClientFx);
    uint32_t hash = game_hash(name);
    int i = 1;
    for (; i < count; i++) {
        if (table[i].hash == 0)
            return free_entry ? i : 0;
        if (table[i].hash == hash)
            return i;
    }
    exceeded++;
    exceeded_count = count;
    return 0;
}
// BG_Cache_RegisterDef: a new entry gets the asset, the hash and a reference; one that is there gets a reference
// when asked for one
static int game_register_def(uint32_t side, uint32_t kind, aae_clientfx_entry_t *table, int32_t count,
                             const char *name, const void *asset, uint32_t reference) {
    if (asset == NULL)
        return 0;
    int index = game_get_index(side, kind, table, count, name, true);
    if (index == 0)
        return 0;
    uint32_t old = table[index].hash;
    if (old == 0) {
        table[index].hash = game_hash(name);
        table[index].asset = asset;
    }
    if (old == 0 || (uint8_t)reference != 0)
        table[index].references++;
    dirty = 1;
    return index;
}
// the generic release: a reference less, and the whole entry free at none (entry 0's count when not found, as the
// game's does)
static uint64_t game_release(uint32_t side, uint32_t kind, aae_clientfx_entry_t *table, int32_t count,
                             const char *name) {
    int index = game_get_index(side, kind, table, count, name, false);
    if (--table[index].references == 0)
        memset(&table[index], 0, sizeof(table[index]));
    return (uint64_t)index << 4;
}
static bool game_find_asset(int32_t type, const char *name, const void **asset) {
    CHECK(type == Asset_Fx);
    if (strncmp(name, "missing", 7) == 0)
        return false;
    *asset = effect_named(name);
    return true;
}

// the game's six functions, with its own table and 1024, and how often each was called
static int called_release, called_get, called_index, called_name, called_register, called_init;
static uint64_t game_release_clientfx(uint32_t side, const char *name) {
    called_release++;
    return game_release(side, Type_ClientFx, game_side(side), 1024, name);
}
static const void *game_get_clientfx(uint32_t side, int32_t index) {
    called_get++;
    return game_side(side)[index].asset;
}
static int game_get_clientfx_index(uint32_t side, const char *name) {
    called_index++;
    return game_get_index(side, Type_ClientFx, game_side(side), 1024, name, false);
}
static const char *game_get_clientfx_name(uint32_t side, int32_t index) {
    called_name++;
    const void *asset = game_side(side)[index].asset;
    return asset != NULL ? *(const char *const *)asset : "";
}
static void game_register_clientfx(uint32_t side, const char *name, uint32_t reference) {
    called_register++;
    const void *asset;
    if (game_find_asset(Asset_Fx, name, &asset) &&
        game_register_def(side, Type_ClientFx, game_side(side), 1024, name, asset, reference & 0xff) <= 0)
        game_printf(8, 0x17, "Unable to register client fx %s\n", name);
}
static void game_init(uint32_t side) {
    called_init++;
    memset(cache + (uint64_t)side * Cache_Side_Size, 0, Cache_Side_Size);
}

// hook_extend_clientfx as the mod left it: the game's dvar has its kind at +0x1c and its value at +0x28; its kinds
// are the game's numbers, written out here so that the source's own are checked against them
#define Game_Bool 1
#define Game_Int 6
#define Game_Text 8
static struct {
    uint8_t game_side[0x60];
    bool there;
    char text[40];
} dvar;
static int wrong_asks;
static void *game_find_dvar(uint32_t hash) {
    CHECK(hash == gsc_hash("hook_extend_clientfx"));
    return dvar.there ? &dvar : NULL;
}
static const char *game_dvar_text(void *found) {
    int kind;
    memcpy(&kind, (uint8_t *)found + Dvar_Kind, sizeof(kind));
    if (kind != Game_Text) {
        wrong_asks++;
        return "1";
    }
    return dvar.text;
}
static void set_dvar(bool there, int kind, int32_t value, const char *text) {
    memset(&dvar, 0, sizeof(dvar));
    dvar.there = there;
    memcpy(dvar.game_side + Dvar_Kind, &kind, sizeof(kind));
    memcpy(dvar.game_side + Dvar_Value, &value, sizeof(value));
    snprintf(dvar.text, sizeof(dvar.text), "%s", text != NULL ? text : "");
}

// ---- what the code said to the log
static char said[64][200];
static int said_count;
static int record(const char *format, ...) {
    va_list list;
    va_start(list, format);
    if (said_count < 64)
        vsnprintf(said[said_count], sizeof(said[0]), format, list);
    va_end(list);
    said_count++;
    return 0;
}
// how many lines said exactly this
static int lines(const char *text) {
    int found = 0;
    for (int i = 0; i < said_count && i < 64; i++)
        found += strcmp(said[i], text) == 0;
    return found;
}

// ---- what the code asks of BO3MacFix: the hooks
static struct {
    void *address, *replacement, **original;
} hooked[8];
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 8)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    if (out_origin_func == (void **)&orig_BG_Cache_ReleaseClientFx)
        *out_origin_func = (void *)game_release_clientfx;
    else if (out_origin_func == (void **)&orig_BG_Cache_GetClientFx)
        *out_origin_func = (void *)game_get_clientfx;
    else if (out_origin_func == (void **)&orig_BG_Cache_GetClientFxIndex)
        *out_origin_func = (void *)game_get_clientfx_index;
    else if (out_origin_func == (void **)&orig_BG_Cache_GetClientFxName)
        *out_origin_func = (void *)game_get_clientfx_name;
    else if (out_origin_func == (void **)&orig_BG_Cache_RegisterClientFx)
        *out_origin_func = (void *)game_register_clientfx;
    else if (out_origin_func == (void **)&orig_BG_Cache_Init)
        *out_origin_func = (void *)game_init;
    return 0;
}
static bool was_hooked(uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 8; i++)
        if (hooked[i].address == (void *)(base + (uint64_t)place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
}

// a game as it starts: nothing registered, T7Overcharged not started, the hooks set and the game's functions in
// place of the addresses aae_clientfx_init takes
static void new_game(bool enabled) {
    memset(cache, 0, 2 * Cache_Side_Size);
    memset(tables, 0xab, sizeof(tables)); // our tables start as anything: nothing may read them before the switch
    atomic_store(&active, false);
    atomic_store(&decided, false);
    for (int i = 0; i < 2; i++) {
        atomic_store(&highest[i], 0);
        atomic_store(&past_the_game[i], false);
    }
    hooks = 0;
    base_address = 0;
    aae_clientfx_init(base, enabled);
    BG_Cache_GetIndexInternal = game_get_index;
    BG_Cache_RegisterDef = game_register_def;
    BG_Cache_Release = game_release;
    BG_Cache_FindAsset = game_find_asset;
    Com_Printf = game_printf;
    Dvar_FindByHash = game_find_dvar;
    Dvar_GetString = game_dvar_text;
    called_release = called_get = called_index = called_name = called_register = called_init = 0;
    prints = exceeded = exceeded_count = dirty = wrong_asks = 0;
    printed[0] = '\0';
    say = record;
    said_count = 0;
    debug = 0;
    set_dvar(true, Game_Text, 0, "1"); // as the mod sets it
}

static char name_of[64];
static const char *fx(int number) {
    snprintf(name_of, sizeof(name_of), "fx/test/effect_%d", number);
    return name_of;
}

// ---- the rules

static void test_switch(void) {
    aae_clientfx_dvar_t none = { .there = false };
    CHECK(aae_clientfx_switch(&none)); // T7Overcharged makes it with 1
    struct {
        int kind;
        int32_t number;
        const char *text;
        bool on;
    } cases[] = {
        { Game_Bool, 0, NULL, false }, { Game_Bool, 1, NULL, true },  { Game_Int, 0, NULL, false },
        { Game_Int, 1, NULL, true },   { Game_Int, 5, NULL, true },   { Game_Int, -1, NULL, true },
        { Game_Text, 0, "1", true },   { Game_Text, 0, "0", false },  { Game_Text, 0, " 1", true },
        { Game_Text, 0, "2", true },   { Game_Text, 0, "-1", true },  { Game_Text, 0, "1.5", true },
        { Game_Text, 0, "0.5", false }, { Game_Text, 0, "abc", false }, { Game_Text, 0, "", false },
        { Game_Text, 0, NULL, false }, { Game_Text, 7, "0", false },  { 2, 0, NULL, true },
        { 7, 0, "0", true },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        aae_clientfx_dvar_t d = { .there = true, .kind = cases[i].kind, .number = cases[i].number,
                                  .text = cases[i].text };
        if (aae_clientfx_switch(&d) != cases[i].on) {
            printf("FAIL switch case %zu\n", i);
            failures++;
        }
    }
}

static void test_side(void) {
    CHECK(aae_clientfx_side(0) == 0);
    CHECK(aae_clientfx_side(1) == 1);
    CHECK(aae_clientfx_side(2) == 1 && aae_clientfx_side(0x80000000) == 1 && aae_clientfx_side(0xffffffff) == 1);
}

static void test_name(void) {
    static aae_clientfx_entry_t table[Aae_ClientFx_Count];
    memset(table, 0, sizeof(table));
    table[0].asset = effect_named("fx/zero");
    table[7].asset = effect_named("fx/seven");
    table[2047].asset = effect_named("fx/last");
    table[1023].asset = effect_named("fx/game_last");
    table[1024].asset = effect_named("fx/first_of_ours");
    CHECK(strcmp(aae_clientfx_name(table, 7), "fx/seven") == 0);
    CHECK(strcmp(aae_clientfx_name(table, 0), "fx/zero") == 0); // entry 0 is read like any other
    CHECK(strcmp(aae_clientfx_name(table, 1023), "fx/game_last") == 0);
    CHECK(strcmp(aae_clientfx_name(table, 1024), "fx/first_of_ours") == 0);
    CHECK(strcmp(aae_clientfx_name(table, 2047), "fx/last") == 0);
    CHECK(strcmp(aae_clientfx_name(table, 8), "") == 0);
    // outside the table: "", and nothing behind or before it is read (the sanitizer would say so)
    CHECK(strcmp(aae_clientfx_name(table, 2048), "") == 0);
    CHECK(strcmp(aae_clientfx_name(table, -1), "") == 0);
    CHECK(strcmp(aae_clientfx_name(table, INT32_MIN), "") == 0 && strcmp(aae_clientfx_name(table, INT32_MAX), "") == 0);
}

static void test_take_over(void) {
    static aae_clientfx_entry_t game[Aae_ClientFx_Game_Count], ours[Aae_ClientFx_Count];
    for (int i = 0; i < Aae_ClientFx_Game_Count; i++)
        game[i] = (aae_clientfx_entry_t){ .asset = &effects[i % 10], .hash = (uint32_t)i * 7 + 3,
                                          .references = (uint8_t)i, .flags = { 1, 2, (uint8_t)i } };
    memset(ours, 0x5a, sizeof(ours));
    aae_clientfx_take_over(ours, game);
    CHECK(memcmp(ours, game, sizeof(game)) == 0);
    bool rest_free = true;
    for (int i = Aae_ClientFx_Game_Count; i < Aae_ClientFx_Count; i++)
        rest_free &= ours[i].asset == NULL && ours[i].hash == 0 && ours[i].references == 0 && ours[i].flags[0] == 0 &&
                     ours[i].flags[1] == 0 && ours[i].flags[2] == 0;
    CHECK(rest_free);
    CHECK(aae_clientfx_in_use(ours, Aae_ClientFx_Count) == Aae_ClientFx_Game_Count);
    ours[2000].hash = 9;
    CHECK(aae_clientfx_in_use(ours, Aae_ClientFx_Count) == Aae_ClientFx_Game_Count + 1);
    CHECK(aae_clientfx_in_use(ours, 2000) == Aae_ClientFx_Game_Count);
    aae_clientfx_clear(ours);
    bool all_free = true;
    for (int i = 0; i < Aae_ClientFx_Count; i++)
        all_free &= ours[i].asset == NULL && ours[i].hash == 0 && ours[i].references == 0 && ours[i].flags[0] == 0 &&
                    ours[i].flags[2] == 0;
    CHECK(all_free && aae_clientfx_in_use(ours, Aae_ClientFx_Count) == 0);
}

// ---- the hooks

static void test_init(void) {
    new_game(false);
    CHECK(hooks == 0); // switched off: nothing hooked, the game's own functions stay
    aae_clientfx_t7o_started();
    CHECK(!atomic_load(&active));
    new_game(true);
    CHECK(hooks == 6);
    CHECK(was_hooked(ADDR_BG_Cache_ReleaseClientFx, (void *)fake_BG_Cache_ReleaseClientFx,
                     (void **)&orig_BG_Cache_ReleaseClientFx));
    CHECK(was_hooked(ADDR_BG_Cache_GetClientFx, (void *)fake_BG_Cache_GetClientFx,
                     (void **)&orig_BG_Cache_GetClientFx));
    CHECK(was_hooked(ADDR_BG_Cache_GetClientFxIndex, (void *)fake_BG_Cache_GetClientFxIndex,
                     (void **)&orig_BG_Cache_GetClientFxIndex));
    CHECK(was_hooked(ADDR_BG_Cache_GetClientFxName, (void *)fake_BG_Cache_GetClientFxName,
                     (void **)&orig_BG_Cache_GetClientFxName));
    CHECK(was_hooked(ADDR_BG_Cache_RegisterClientFx, (void *)fake_BG_Cache_RegisterClientFx,
                     (void **)&orig_BG_Cache_RegisterClientFx));
    CHECK(was_hooked(ADDR_BG_Cache_Init, (void *)fake_BG_Cache_Init, (void **)&orig_BG_Cache_Init));
    // the game's functions it calls, from where they are
    hooks = 0;
    aae_clientfx_init(base, 1);
    CHECK(BG_Cache_GetIndexInternal == (void *)(base + ADDR_BG_Cache_GetIndexInternal) &&
          BG_Cache_RegisterDef == (void *)(base + ADDR_BG_Cache_RegisterDef) &&
          BG_Cache_Release == (void *)(base + ADDR_BG_Cache_Release) &&
          BG_Cache_FindAsset == (void *)(base + ADDR_BG_Cache_FindAsset) &&
          Com_Printf == (void *)(base + ADDR_Com_Printf) && Dvar_FindByHash == (void *)(base + ADDR_Dvar_FindByHash) &&
          Dvar_GetString == (void *)(base + ADDR_Dvar_GetString));
    // the places, as they were read from the game's code
    CHECK(ADDR_bgCache == 0xa974930 && Cache_Side_Size == 0x45fe00 && Cache_ClientFx == 0x43c00 &&
          Type_ClientFx == 0x1b && Asset_Fx == 0x26 && ADDR_BG_Cache_Init == 0x12a1ae0 &&
          ADDR_BG_Cache_GetIndexInternal == 0x12a7048 && ADDR_BG_Cache_RegisterDef == 0x12a7d0d &&
          ADDR_BG_Cache_Release == 0x12a7237 && ADDR_BG_Cache_FindAsset == 0x12a29ea &&
          ADDR_BG_Cache_ReleaseClientFx == 0x12a75a1 && ADDR_BG_Cache_GetClientFx == 0x12a7474 &&
          ADDR_BG_Cache_GetClientFxIndex == 0x12a74d5 && ADDR_BG_Cache_GetClientFxName == 0x12a749c &&
          ADDR_BG_Cache_RegisterClientFx == 0x12a750a && ADDR_Com_Printf == 0xe71396 && Dvar_Kind == 0x1c &&
          Dvar_Value == 0x28 && Dvar_Kind_Bool == Game_Bool && Dvar_Kind_Int == Game_Int &&
          Dvar_Kind_Text == Game_Text);
}

// fill a side with the game's or our functions alike, through the hooks
static int register_many(uint32_t side, int from, int to) {
    int done = 0;
    for (int i = from; i < to; i++) {
        int before = exceeded;
        fake_BG_Cache_RegisterClientFx(side, fx(i), 1);
        done += exceeded == before;
    }
    return done;
}

static void test_before_the_start(void) {
    // until T7Overcharged starts every call goes to the game's own, with its table of 1024
    new_game(true);
    CHECK(register_many(1, 0, 1023) == 1023 && exceeded == 0);
    CHECK(called_register == 1023 && fake_BG_Cache_GetClientFxIndex(1, fx(5)) == 6 && called_index == 1);
    CHECK(fake_BG_Cache_GetClientFx(1, 6) == effect_named(fx(5)) && called_get == 1);
    CHECK(strcmp(fake_BG_Cache_GetClientFxName(1, 6), fx(5)) == 0 && called_name == 1);
    fake_BG_Cache_RegisterClientFx(1, fx(7), 0); // there already, and no reference asked for
    CHECK(game_side(1)[8].references == 1 && called_register == 1024);
    fake_BG_Cache_RegisterClientFx(1, fx(7), 1);
    CHECK(game_side(1)[8].references == 2);
    CHECK(fake_BG_Cache_ReleaseClientFx(1, fx(5)) == (6 << 4) && called_release == 1);
    CHECK(game_side(1)[6].hash == 0 && fake_BG_Cache_GetClientFxIndex(1, fx(5)) == 0);
    fake_BG_Cache_Init(0);
    CHECK(called_init == 1 && game_side(1)[1].hash != 0);
    // the game's own stop at 1024: the 1023 are there, one more doesn't fit
    fake_BG_Cache_RegisterClientFx(1, fx(5), 1); // back into its free entry
    fake_BG_Cache_RegisterClientFx(1, fx(2000), 1);
    CHECK(exceeded == 1 && exceeded_count == 1024 &&
          strcmp(printed, "Unable to register client fx fx/test/effect_2000\n") == 0);
    // our tables were never touched
    CHECK(tables[0][1].hash == 0xabababab && tables[1][1023].hash == 0xabababab);
}

static void test_start_says_no(void) {
    new_game(true);
    set_dvar(true, Game_Text, 0, "0");
    aae_clientfx_t7o_started();
    CHECK(!atomic_load(&active) && wrong_asks == 0);
    CHECK(said_count == 1 && lines("client effects: T7Overcharged's hook_extend_clientfx is \"0\", so the game's table "
                                   "of 1024 stays (MoreClientEffects)\n") == 1);
    // T7Overcharged decides once, as it starts: the mod setting it again before the next start changes nothing
    set_dvar(true, Game_Text, 0, "1");
    aae_clientfx_t7o_started();
    CHECK(!atomic_load(&active));
    CHECK(register_many(1, 0, 1024) == 1023 && exceeded == 1 && exceeded_count == 1024);
    // a bool that is off
    new_game(true);
    set_dvar(true, Game_Bool, 0, NULL);
    aae_clientfx_t7o_started();
    CHECK(!atomic_load(&active) && wrong_asks == 0);
    CHECK(said_count == 1 && lines("client effects: T7Overcharged's hook_extend_clientfx is 0 (a dvar of kind 1), so "
                                   "the game's table of 1024 stays (MoreClientEffects)\n") == 1);
    // switched off, a start does nothing at all
    new_game(false);
    aae_clientfx_t7o_started();
    CHECK(!atomic_load(&active) && !atomic_load(&decided));
}

static void test_start_says_yes(void) {
    new_game(true);
    // what the game has registered by then goes along: the server's 10, the client's full 1023
    CHECK(register_many(0, 5000, 5010) == 10);
    CHECK(register_many(1, 0, 1023) == 1023 && exceeded == 0);
    game_side(0)[3].flags[1] = 0x77;
    set_dvar(false, 0, 0, NULL); // not set at all: T7Overcharged's own 1
    debug = 1;
    aae_clientfx_t7o_started();
    CHECK(atomic_load(&active));
    CHECK(said_count == 2 && lines("client effects: T7Overcharged's hook_extend_clientfx is not set, so client effects "
                                   "have a table of 2048 from now on (MoreClientEffects)\n") == 1);
    CHECK(lines("client effects: taken over from the game: 10 of the server's entries, 1023 of the client's\n") == 1);
    CHECK(memcmp(tables[0], game_side(0), Aae_ClientFx_Game_Count * sizeof(aae_clientfx_entry_t)) == 0);
    CHECK(memcmp(tables[1], game_side(1), Aae_ClientFx_Game_Count * sizeof(aae_clientfx_entry_t)) == 0);
    CHECK(tables[0][1024].hash == 0 && tables[1][2047].asset == NULL && tables[0][3].flags[1] == 0x77);
    // from now on the game's tables stay as they are
    static aae_clientfx_entry_t snapshot[2][Aae_ClientFx_Game_Count];
    memcpy(snapshot[0], game_side(0), sizeof(snapshot[0]));
    memcpy(snapshot[1], game_side(1), sizeof(snapshot[1]));
    called_release = called_get = called_index = called_name = called_register = 0;
    // the client's: 1024 more, entries 1024 to 2047; then the stop, at 2048
    CHECK(register_many(1, 1023, 2047) == 1024 && exceeded == 0);
    CHECK(atomic_load(&highest[1]) == 2047 && atomic_load(&past_the_game[1]) && !atomic_load(&past_the_game[0]));
    // the log heard of it once, and with the debug switch of every 256 more: 1024, 1280, 1536 and 1792
    CHECK(said_count == 7 && lines("client effects: the client's table holds more than the game's 1023 (entry 1024 is "
                                   "fx/test/effect_1023)\n") == 1);
    CHECK(lines("client effects: the client's table has reached entry 1024 (fx/test/effect_1023)\n") == 1 &&
          lines("client effects: the client's table has reached entry 1280 (fx/test/effect_1279)\n") == 1 &&
          lines("client effects: the client's table has reached entry 1536 (fx/test/effect_1535)\n") == 1 &&
          lines("client effects: the client's table has reached entry 1792 (fx/test/effect_1791)\n") == 1);
    debug = 0;
    fake_BG_Cache_RegisterClientFx(1, fx(2047), 1);
    CHECK(exceeded == 1 && exceeded_count == 2048 && prints == 1 &&
          strcmp(printed, "Unable to register client fx fx/test/effect_2047\n") == 0);
    CHECK(called_register == 0 && called_index == 0);
    // lookups find the old ones where they were and the new ones in ours
    CHECK(fake_BG_Cache_GetClientFxIndex(1, fx(0)) == 1 && fake_BG_Cache_GetClientFxIndex(1, fx(1500)) == 1501);
    CHECK(fake_BG_Cache_GetClientFxIndex(0, fx(5003)) == 4 && fake_BG_Cache_GetClientFxIndex(0, fx(1500)) == 0);
    CHECK(fake_BG_Cache_GetClientFxIndex(0, "fx/never") == 0 && exceeded == 1);
    // looking up a name that isn't there in a full table stops the map too, the game's rule, at 2048 now
    CHECK(fake_BG_Cache_GetClientFxIndex(1, "fx/never") == 0 && exceeded == 2 && exceeded_count == 2048);
    CHECK(fake_BG_Cache_GetClientFx(1, 2047) == effect_named(fx(2046)) &&
          fake_BG_Cache_GetClientFx(0, 4) == effect_named(fx(5003)));
    CHECK(strcmp(fake_BG_Cache_GetClientFxName(1, 1024), fx(1023)) == 0);
    CHECK(strcmp(fake_BG_Cache_GetClientFxName(1, 2048), "") == 0 &&
          strcmp(fake_BG_Cache_GetClientFxName(0, 11), "") == 0);
    CHECK(called_get == 0 && called_name == 0);
    // a side that is neither 0 nor 1 is the client's, as with T7Overcharged
    CHECK(fake_BG_Cache_GetClientFxIndex(2, fx(1500)) == 1501 &&
          fake_BG_Cache_GetClientFx(7, 1501) == effect_named(fx(1500)));
    CHECK(memcmp(snapshot[0], game_side(0), sizeof(snapshot[0])) == 0 &&
          memcmp(snapshot[1], game_side(1), sizeof(snapshot[1])) == 0);

    // references: a second registration that asks for one adds one, one that doesn't adds none
    fake_BG_Cache_RegisterClientFx(0, "fx/held", 1);
    int held = fake_BG_Cache_GetClientFxIndex(0, "fx/held");
    CHECK(held == 11 && tables[0][held].references == 1 && atomic_load(&highest[0]) == 11);
    fake_BG_Cache_RegisterClientFx(0, "fx/held", 1);
    fake_BG_Cache_RegisterClientFx(0, "fx/held", 0);
    fake_BG_Cache_RegisterClientFx(0, "fx/held", 0x100); // only the lowest byte counts
    CHECK(tables[0][held].references == 2);
    fake_BG_Cache_RegisterClientFx(0, "fx/held", 0x101);
    CHECK(tables[0][held].references == 3);
    // letting go: a reference less each time, the entry free at none
    fake_BG_Cache_ReleaseClientFx(0, "fx/held");
    fake_BG_Cache_ReleaseClientFx(0, "fx/held");
    CHECK(tables[0][held].references == 1 && fake_BG_Cache_GetClientFxIndex(0, "fx/held") == held);
    CHECK(fake_BG_Cache_ReleaseClientFx(0, "fx/held") == ((uint64_t)held << 4));
    CHECK(tables[0][held].hash == 0 && tables[0][held].asset == NULL &&
          fake_BG_Cache_GetClientFxIndex(0, "fx/held") == 0);
    // one past the game's 1023 is let go of in ours, by the limit of 2048
    CHECK(tables[1][1501].references == 1);
    fake_BG_Cache_ReleaseClientFx(1, fx(1500));
    CHECK(tables[1][1501].hash == 0 && tables[1][1501].asset == NULL && exceeded == 2);
    fake_BG_Cache_RegisterClientFx(1, fx(1500), 1); // back into its free entry
    CHECK(fake_BG_Cache_GetClientFxIndex(1, fx(1500)) == 1501 && tables[1][1501].references == 1);
    CHECK(called_release == 0);
    // an asset that isn't there is left out, without the registration's own line (the lookup prints its own)
    prints = 0;
    fake_BG_Cache_RegisterClientFx(1, "missing/effect", 1);
    CHECK(prints == 0 && exceeded == 2 && fake_BG_Cache_GetClientFxIndex(0, "missing/effect") == 0);

    // a map starts: the reset clears that side of ours, the game's own reset runs too, the other side stays
    called_init = 0;
    said_count = 0;
    debug = 1;
    fake_BG_Cache_Init(1);
    CHECK(said_count == 1 && lines("client effects: the client's table is reset: 2047 entries were in use, the highest "
                                   "index since the last reset was 2047\n") == 1);
    debug = 0;
    CHECK(called_init == 1 && aae_clientfx_in_use(tables[1], Aae_ClientFx_Count) == 0);
    CHECK(aae_clientfx_in_use(tables[0], Aae_ClientFx_Count) == 10 && game_side(1)[1].hash == 0);
    CHECK(atomic_load(&highest[1]) == 0 && !atomic_load(&past_the_game[1]) && atomic_load(&highest[0]) == 11);
    CHECK(register_many(1, 0, 1023) == 1023 && atomic_load(&highest[1]) == 1023 && !atomic_load(&past_the_game[1]));
    CHECK(register_many(1, 1023, 1024) == 1 && atomic_load(&highest[1]) == 1024 && atomic_load(&past_the_game[1]));
    CHECK(register_many(1, 1024, 2047) == 1023 && exceeded == 2 && fake_BG_Cache_GetClientFxIndex(1, fx(2046)) == 2047);
    CHECK(said_count == 2 && lines("client effects: the client's table holds more than the game's 1023 (entry 1024 is "
                                   "fx/test/effect_1023)\n") == 1);
    fake_BG_Cache_Init(0);
    CHECK(called_init == 2 && aae_clientfx_in_use(tables[0], Aae_ClientFx_Count) == 0 &&
          aae_clientfx_in_use(tables[1], Aae_ClientFx_Count) == 2047);

    // T7Overcharged started again (the mod starts it at every map): nothing changes, nothing is copied again
    set_dvar(true, Game_Text, 0, "0");
    game_side(1)[5].hash = 1234;
    aae_clientfx_t7o_started();
    CHECK(atomic_load(&active) && tables[1][5].hash != 1234);
}

static void test_kinds_of_dvar(void) {
    // what the dvar is when T7Overcharged starts decides, whatever kind of dvar the mod's Engine.SetDvar made
    struct {
        bool there;
        int kind;
        int32_t value;
        const char *text;
        bool on;
    } cases[] = {
        { true, Game_Text, 0, "1", true }, { true, Game_Text, 1, "0", false },
        { true, Game_Bool, 1, "0", true }, { true, Game_Bool, 0x100, "1", false },
        { true, Game_Int, 3, "0", true },  { true, Game_Int, 0, "1", false },
        { true, Game_Int, 0x100, "0", true }, { true, Game_Bool, 0x101, "0", true },
        { false, Game_Text, 0, "0", true }, { true, 2, 0, "0", true },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        new_game(true);
        set_dvar(cases[i].there, cases[i].kind, cases[i].value, cases[i].text);
        aae_clientfx_t7o_started();
        if (atomic_load(&active) != cases[i].on || wrong_asks != 0) {
            printf("FAIL dvar case %zu\n", i);
            failures++;
        }
    }
}

int main(void) {
    cache = calloc(2, Cache_Side_Size);
    if (cache == NULL)
        return 2;
    base = (uint64_t)cache - ADDR_bgCache;
    test_switch();
    test_side();
    test_name();
    test_take_over();
    test_init();
    test_before_the_start();
    test_start_says_no();
    test_start_says_yes();
    test_kinds_of_dvar();
    free(cache);
    if (failures != 0) {
        printf("test_aae_clientfx: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_clientfx: all passed\n");
    return 0;
}
