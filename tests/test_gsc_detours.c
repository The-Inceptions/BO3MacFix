// gsc_detours.c against a made-up game: script assets, the table of loaded scripts, a Scr_LoadScript that loads a
// script's includes first and links its calls, and the mapname dvar (the test includes the source itself).
#include <stdio.h>
#include <stdlib.h>
#include "../source/gsc_detours.c"
#include "fake_gsc.h"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int DobbyHook(void *address, void *fake_func, void **out_origin_func) {
    (void)address, (void)fake_func, (void)out_origin_func;
    return 0;
}

#pragma mark - the made-up game

#define Max_Assets 32
static script_asset_t assets[Max_Assets];
static char asset_names[Max_Assets][96];
static unsigned asset_count;
static const char *map_dvar;
static int unresolved; // imports the made-up linker couldn't resolve: the real game stops the map load over one

static script_asset_t *game_find(int type, const char *name, bool error_if_missing, int wait_time) {
    (void)error_if_missing, (void)wait_time;
    CHECK(type == Asset_Script);
    for (unsigned i = 0; i < asset_count; i++)
        if (strcmp(asset_names[i], name) == 0)
            return &assets[i];
    return NULL;
}

static script_asset_t *add_asset(const char *name, uint8_t *buffer, size_t length) {
    snprintf(asset_names[asset_count], sizeof(asset_names[0]), "%s", name);
    assets[asset_count] = (script_asset_t){ asset_names[asset_count], (int32_t)length, 0, buffer };
    return &assets[asset_count++];
}

static uint32_t *game_count(void) {
    return (uint32_t *)(base + ADDR_scrObjCount);
}
static uint8_t *game_entry(uint32_t index) {
    return (uint8_t *)(base + ADDR_scrObjTable + (uint64_t)index * Obj_Entry_Size);
}

// the function an import resolves to: one of the script's own, else a public one of an include
static const uint8_t *resolve(const uint8_t *obj, const gsc_import_t *import) {
    gsc_export_t e;
    if (gsc_obj_find_export(obj, import->name_space, import->name, &e))
        return obj + e.address;
    for (unsigned i = 0; i < gsc_obj_include_count(obj); i++) {
        char name[128];
        snprintf(name, sizeof(name), "%s.gsc", gsc_obj_include(obj, i));
        script_asset_t *asset = game_find(Asset_Script, name, true, -1);
        if (asset != NULL && gsc_obj_find_export(asset->buffer, import->name_space, import->name, &e))
            return asset->buffer + e.address;
    }
    return NULL;
}

// the game's loader: registers the script, loads what it includes, moves it behind those, links its calls
static int game_load_obj(uint8_t *obj) {
    uint64_t magic;
    memcpy(&magic, obj, sizeof(magic));
    if (magic != Gsc_Magic)
        return -1;
    for (uint32_t i = 0; i < *game_count(); i++)
        if (script_at(i) == obj)
            return 1;
    uint32_t index = (*game_count())++;
    memcpy(game_entry(index), &obj, sizeof(obj));
    for (unsigned i = 0; i < gsc_obj_include_count(obj); i++) {
        char name[128];
        snprintf(name, sizeof(name), "%s.gsc", gsc_obj_include(obj, i));
        script_asset_t *asset = game_find(Asset_Script, name, true, -1);
        if (asset == NULL || game_load_obj(asset->buffer) < 0)
            return -2;
    }
    uint32_t last = *game_count() - 1;
    memmove(game_entry(index), game_entry(index + 1), (size_t)(last - index) * Obj_Entry_Size);
    memcpy(game_entry(last), &obj, sizeof(obj));
    gsc_import_t import;
    unsigned at;
    size_t cursor = 0;
    while (gsc_obj_next_import(obj, &at, &cursor, &import)) {
        const uint8_t *function = resolve(obj, &import);
        if (function == NULL) {
            unresolved++;
            continue;
        }
        for (unsigned c = 0; c < import.call_count; c++)
            memcpy(obj + gsc_call_slot(gsc_import_call(&import, c), import.flags), &function, sizeof(function));
    }
    return 1;
}

static int game_load_script(int inst, const char *name) {
    if (inst != Script_Server)
        return 0; // the made-up game has no client scripts
    char asset_name[128];
    snprintf(asset_name, sizeof(asset_name), "%s.gsc", name);
    script_asset_t *asset = game_find(Asset_Script, asset_name, true, -1);
    return asset != NULL && game_load_obj(asset->buffer) >= 0;
}

static void *game_dvar(uint32_t hash) {
    return hash == gsc_hash("mapname") ? (void *)&map_dvar : NULL;
}
static const char *game_dvar_string(const void *dvar) {
    return *(const char *const *)dvar;
}
static void *game_no_builtin(int inst, uint32_t name, int *kind, int *min_params, int *max_params) {
    (void)inst, (void)name, (void)kind, (void)min_params, (void)max_params;
    return NULL;
}
static char game_links_dev_calls(void) {
    return 0;
}

#pragma mark - its scripts

#define F1 (Code_At + 0x10)
#define F2 (Code_At + 0x100)
#define Replacement (Code_At + 0x10)
#define Replacement_Size 0x80

static uint8_t first[0x2000], zm[0x2000], utility[0x2000], level[0x2000], late[0x2000], plain[0x2000], dummy[0x2000];
static uint8_t common_main[0x2000], level_main[0x2000];
static uint8_t zm_patch[0x3000], utility_patch[0x3000], utility_level_patch[0x3000], level_patch[0x3000];
static uint8_t bad_patch[0x3000];
static script_asset_t *zm_patch_asset, *utility_patch_asset, *utility_level_asset, *level_patch_asset, *bad_asset;

// a patch script asset replacing one function of a script, its replacement calling the original and one more thing
static script_asset_t *add_patch(uint8_t *out, const char *asset_name, const char *target_script,
                                 uint32_t target_name_space, uint32_t target_name, const char *include,
                                 const char *second_include, uint32_t other_name_space, uint32_t other_name) {
    static uint8_t body[0x2000];
    const char *includes[] = { "#using", include, second_include };
    unsigned include_count = second_include != NULL ? 3 : 2;
    made_export_t exports[] = { { 0xee, 0x900 + asset_count, Replacement, 1, 0 } };
    made_import_t imports[] = {
        { target_name_space, target_name, 1, Gsc_Call_Function, { Replacement + 0x10, Code_At + 0x300 }, 2 },
        { other_name_space, other_name, 0, Gsc_Call_Function, { Replacement + 0x30 }, 1 },
    };
    size_t body_size = make_script(body, "", 0, includes, include_count, exports, 1, imports, 2);
    gsic_detour_t detour = { 0x900 + asset_count, target_name_space, target_name, Replacement, Replacement_Size,
                             target_script };
    return add_asset(asset_name, out, make_gsic(out, &detour, 1, body, body_size));
}

static void make_game(void) {
    static uint8_t table[ADDR_scrObjCount + 2 * sizeof(uint32_t) - ADDR_scrObjTable];
    base = (uint64_t)table - ADDR_scrObjTable;
    DB_FindXAssetHeader = game_find;
    Dvar_FindByHash = game_dvar;
    Dvar_GetString = game_dvar_string;
    builtin_function = builtin_method = game_no_builtin;
    linker_skips_dev_calls = game_links_dev_calls;
    orig_Scr_LoadScript = game_load_script;

    add_asset("scripts/shared/first.gsc", first, make_script(first, "scripts/shared/first.gsc", 0, NULL, 0, NULL, 0, NULL, 0));
    add_asset("scripts/zm/dummy.gsc", dummy, make_script(dummy, "scripts/zm/dummy.gsc", 0, NULL, 0, NULL, 0, NULL, 0));
    // scripts/zm/_zm.gsc: f1 and f2, and it calls f1 itself
    made_export_t zm_exports[] = { { 0xac, 0x1, F1, 1, 0 }, { 0xac, 0x2, F2, 0, 0 } };
    made_import_t zm_imports[] = { { 0xac, 0x1, 1, Gsc_Call_Function, { Code_At + 0x120 }, 1 } };
    add_asset("scripts/zm/_zm.gsc", zm, make_script(zm, "scripts/zm/_zm.gsc", 0, NULL, 0, zm_exports, 2, zm_imports, 1));
    made_export_t utility_exports[] = { { 0x17, 0x1, F1, 1, 0 } };
    add_asset("scripts/zm/_zm_utility.gsc", utility,
              make_script(utility, "scripts/zm/_zm_utility.gsc", 0, NULL, 0, utility_exports, 1, NULL, 0));
    // the map's script: its own m1, and it calls _zm's f1 and _zm_utility's g1
    const char *level_includes[] = { "scripts/zm/_zm", "scripts/zm/_zm_utility" };
    made_export_t level_exports[] = { { 0x77, 0x1, F2, 1, 0 } };
    made_import_t level_imports[] = { { 0xac, 0x1, 1, Gsc_Call_Function, { Code_At + 0x20 }, 1 },
                                      { 0x17, 0x1, 0, Gsc_Call_Thread, { Code_At + 0x40 }, 1 },
                                      { 0x77, 0x1, 0, Gsc_Call_Address, { Code_At + 0x60 }, 1 } };
    add_asset("scripts/zm/zm_test.gsc", level,
              make_script(level, "scripts/zm/zm_test.gsc", 0, level_includes, 2, level_exports, 1, level_imports, 3));
    // a script the game loads later: calls f1 and f2
    const char *late_includes[] = { "scripts/zm/_zm" };
    made_import_t late_imports[] = { { 0xac, 0x1, 1, Gsc_Call_Method, { Code_At + 0x50 }, 1 },
                                     { 0xac, 0x2, 0, Gsc_Call_Function, { Code_At + 0x30 }, 1 } };
    add_asset("scripts/zm/late.gsc", late,
              make_script(late, "scripts/zm/late.gsc", 0, late_includes, 1, NULL, 0, late_imports, 2));

    made_export_t main_exports[] = { { 0x6b, gsc_hash("main"), Code_At + 8, 0, 2 } };
    add_asset("scripts/aae_common/main.gsc", common_main, make_script(common_main, "", 0, NULL, 0, main_exports, 1, NULL, 0));
    add_asset("scripts/aae_level/zm_test/main.gsc", level_main,
              make_script(level_main, "", 0, NULL, 0, main_exports, 1, NULL, 0));
    zm_patch_asset = add_patch(zm_patch, "scripts/aae_common/zm/_zm.gsc", "scripts/zm/_zm.gsc", 0xac, 0x1,
                               "scripts/zm/_zm", NULL, 0xac, 0x2);
    utility_patch_asset = add_patch(utility_patch, "scripts/aae_common/zm/_zm_utility.gsc",
                                    "scripts/zm/_zm_utility.gsc", 0x17, 0x1, "scripts/zm/_zm_utility", NULL, 0x17, 0x1);
    utility_level_asset = add_patch(utility_level_patch, "scripts/aae_level/zm_test/zm/_zm_utility.gsc",
                                    "scripts/zm/_zm_utility.gsc", 0x17, 0x1, "scripts/zm/_zm_utility", NULL, 0x17, 0x1);
    level_patch_asset = add_patch(level_patch, "scripts/aae_level/zm_test/zm/zm_test.gsc", "scripts/zm/zm_test.gsc",
                                  0x77, 0x1, "scripts/zm/zm_test", "scripts/zm/_zm", 0xac, 0x1);
    // on zm_plain a plain script sits where _zm's patch script would be
    add_asset("scripts/aae_level/zm_plain/zm/_zm.gsc", plain, make_script(plain, "", 0, NULL, 0, NULL, 0, NULL, 0));
    // on zm_bad _zm's patch script calls a function nothing has
    bad_asset = add_patch(bad_patch, "scripts/aae_level/zm_bad/zm/_zm.gsc", "scripts/zm/_zm.gsc", 0xac, 0x1,
                          "scripts/zm/_zm", NULL, 0xac, 0xdead);
}

static uint8_t *body_of(script_asset_t *asset) {
    return asset->buffer + 16 + Gsic_Entry_Size;
}
static uint64_t call_target(const uint8_t *obj, uint32_t call, uint8_t type) {
    return get64(obj + gsc_call_slot(call, type));
}
// a new map load: the game empties its table
static void new_map(const char *map) {
    *game_count() = 0;
    map_dvar = map;
}
// a patch script asset is as the fastfile had it: the header first, the bogus include still in the table
static void check_untouched(script_asset_t *asset, size_t length) {
    CHECK(gsic_is(asset->buffer, (size_t)asset->length) && (size_t)asset->length == length);
    CHECK(gsc_obj_include_count(body_of(asset)) == (asset == level_patch_asset ? 3u : 2u));
    CHECK(strcmp(gsc_obj_include(body_of(asset), 0), "#using") == 0);
}

#pragma mark - tests

static void test_zombies_map(void) {
    size_t zm_patch_length = (size_t)zm_patch_asset->length, level_length = (size_t)utility_level_asset->length;
    new_map("zm_test");
    // nothing happens before a scripts/zm/ script has loaded
    CHECK(fake_Scr_LoadScript(Script_Server, "scripts/shared/first") == 1 && *game_count() == 1);
    CHECK(fake_Scr_LoadScript(Script_Server, "scripts/zm/missing") == 0 && *game_count() == 1);
    // the client's scripts are left alone
    CHECK(fake_Scr_LoadScript(1, "scripts/zm/missing") == 0 && *game_count() == 1);

    CHECK(fake_Scr_LoadScript(Script_Server, "scripts/zm/zm_test") == 1);
    // _zm's patch script for every map, _zm_utility's and zm_test's for this map, and both main scripts
    CHECK(*game_count() == 9);
    CHECK(is_loaded(body_of(zm_patch_asset)) && is_loaded(body_of(utility_level_asset)));
    CHECK(is_loaded(body_of(level_patch_asset)) && !is_loaded(body_of(utility_patch_asset)));
    CHECK(is_loaded(common_main) && is_loaded(level_main));
    CHECK(unresolved == 0);
    check_untouched(zm_patch_asset, zm_patch_length);
    check_untouched(utility_level_asset, level_length);
    check_untouched(utility_patch_asset, (size_t)utility_patch_asset->length);
    check_untouched(level_patch_asset, (size_t)level_patch_asset->length);

    uint64_t f1 = (uint64_t)(zm + F1), f2 = (uint64_t)(zm + F2), m1 = (uint64_t)(level + F2);
    uint64_t r1 = (uint64_t)(body_of(zm_patch_asset) + Replacement);
    uint64_t t1 = (uint64_t)(body_of(utility_level_asset) + Replacement);
    uint64_t u1 = (uint64_t)(body_of(level_patch_asset) + Replacement);
    CHECK(call_target(zm, Code_At + 0x120, Gsc_Call_Function) == r1);
    CHECK(call_target(level, Code_At + 0x20, Gsc_Call_Function) == r1);
    CHECK(call_target(level, Code_At + 0x40, Gsc_Call_Thread) == t1);
    CHECK(call_target(level, Code_At + 0x60, Gsc_Call_Address) == u1);
    // a replacement's call of what it replaces stays, its other calls and the rest of the patch script are redirected
    CHECK(call_target(body_of(zm_patch_asset), Replacement + 0x10, Gsc_Call_Function) == f1);
    CHECK(call_target(body_of(zm_patch_asset), Code_At + 0x300, Gsc_Call_Function) == r1);
    CHECK(call_target(body_of(zm_patch_asset), Replacement + 0x30, Gsc_Call_Function) == f2);
    CHECK(call_target(body_of(level_patch_asset), Replacement + 0x10, Gsc_Call_Function) == m1);
    CHECK(call_target(body_of(level_patch_asset), Replacement + 0x30, Gsc_Call_Function) == r1);
    CHECK(rules.count == 3 && rules.resolved == 3);

    // a script the game loads afterwards gets its calls redirected too, and nothing loads twice
    CHECK(fake_Scr_LoadScript(Script_Server, "scripts/zm/late") == 1 && *game_count() == 10);
    CHECK(call_target(late, Code_At + 0x50, Gsc_Call_Method) == r1);
    CHECK(call_target(late, Code_At + 0x30, Gsc_Call_Function) == f2);
    CHECK(fake_Scr_LoadScript(Script_Server, "scripts/zm/late") == 1 && *game_count() == 10);
}

static void test_next_map(void) {
    // another zombies map in the same game: the map's own patch scripts are gone, the ones for every map load again
    new_map("zm_other");
    CHECK(fake_Scr_LoadScript(Script_Server, "scripts/zm/zm_test") == 1);
    CHECK(*game_count() == 6); // _zm, _zm_utility, zm_test, their two patch scripts, the common main
    CHECK(is_loaded(body_of(zm_patch_asset)) && is_loaded(body_of(utility_patch_asset)));
    CHECK(!is_loaded(body_of(utility_level_asset)) && !is_loaded(body_of(level_patch_asset)));
    CHECK(is_loaded(common_main) && !is_loaded(level_main));
    CHECK(call_target(level, Code_At + 0x20, Gsc_Call_Function) == (uint64_t)(body_of(zm_patch_asset) + Replacement));
    CHECK(call_target(level, Code_At + 0x40, Gsc_Call_Thread) == (uint64_t)(body_of(utility_patch_asset) + Replacement));
    CHECK(call_target(level, Code_At + 0x60, Gsc_Call_Address) == (uint64_t)(level + F2));
    CHECK(rules.count == 2 && rules.resolved == 2 && unresolved == 0);
}

static void test_no_patch_scripts(void) {
    const char *maps[] = { "mp_test", "", "ZM_TEST/..", "zm_plain", "zm_bad" };
    for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
        // not a zombies map; and maps where _zm's own patch script is a plain script, or one the game couldn't link:
        // neither is loaded, and the one for every map doesn't take its place
        bool zombies = i >= 3;
        size_t bad_length = (size_t)bad_asset->length;
        new_map(maps[i]);
        CHECK(fake_Scr_LoadScript(Script_Server, "scripts/zm/zm_test") == 1);
        CHECK(*game_count() == (zombies ? 5u : 3u)); // with _zm_utility's patch script and the common main
        CHECK(!is_loaded(body_of(zm_patch_asset)) && !is_loaded(body_of(bad_asset)) && !is_loaded(plain));
        CHECK(is_loaded(body_of(utility_patch_asset)) == zombies);
        CHECK(call_target(zm, Code_At + 0x120, Gsc_Call_Function) == (uint64_t)(zm + F1));
        CHECK(call_target(level, Code_At + 0x20, Gsc_Call_Function) == (uint64_t)(zm + F1));
        CHECK(unresolved == 0);
        check_untouched(bad_asset, bad_length);
    }
    // the menus: no scripts/zm/ script loads
    new_map("zm_test");
    CHECK(fake_Scr_LoadScript(Script_Server, "scripts/shared/first") == 1 && *game_count() == 1);
}

static void test_full_table(void) {
    // the game's table has room for two more scripts once the map's three are in: two patch scripts load, the third
    // and the main scripts are left out, and nothing is written past the table
    new_map("zm_test");
    for (uint32_t i = 0; i < Obj_Max - 5; i++) {
        uint8_t *obj = dummy;
        memcpy(game_entry((*game_count())++), &obj, sizeof(obj));
    }
    CHECK(fake_Scr_LoadScript(Script_Server, "scripts/zm/zm_test") == 1);
    CHECK(*game_count() == Obj_Max);
    CHECK(is_loaded(body_of(zm_patch_asset)) && is_loaded(body_of(utility_level_asset)));
    CHECK(!is_loaded(body_of(level_patch_asset)) && !is_loaded(common_main) && !is_loaded(level_main));
    CHECK(call_target(level, Code_At + 0x20, Gsc_Call_Function) == (uint64_t)(body_of(zm_patch_asset) + Replacement));
    CHECK(call_target(level, Code_At + 0x60, Gsc_Call_Address) == (uint64_t)(level + F2));
    check_untouched(level_patch_asset, (size_t)level_patch_asset->length);
    CHECK(unresolved == 0);
}

int main(void) {
    make_game();
    test_zombies_map();
    test_next_map();
    test_no_patch_scripts();
    test_full_table();
    if (failures != 0) {
        printf("test_gsc_detours: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_gsc_detours: all passed\n");
    return 0;
}
