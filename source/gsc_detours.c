// All-around Enhancement 4's patch scripts, the half that talks to the game (gsc_obj.h says what they are and has
// the parts that only read and write script bytes).
//
// On Windows the mod's plugin (AAEUnified.dll, its gsc_detours component) does this, and this follows it:
// - It watches the game load the server's scripts. Once a scripts/zm/ script has loaded on a zm_ map, every loaded
//   script scripts/<x>.gsc is looked up as scripts/aae_level/<map>/<x>.gsc and, only if the mod has none by that
//   name, as scripts/aae_common/<x>.gsc.
// - A patch script found that way is loaded through the game's own Scr_LoadScript, which links it like any script.
//   The asset starts with the "GSIC" header, so for the length of that call the asset points at the script behind
//   it, and the "#using" its compiler left in the include table is taken out. Both are put back afterwards.
// - The game stops a map load over a script it can't link, so one whose includes or calls the Mac build can't
//   resolve is left out instead, with the reason in log.txt.
// - The mod's scripts/aae_common/main.gsc and scripts/aae_level/<map>/main.gsc, plain scripts that only hold an
//   autoexec function (empty in 4.1.7), are loaded after them.
// The plugin then redirects calls as the script VM reaches them, from hooks on eight opcode handlers. Here the
// calls are changed once instead, right after loading: the linker has written each called function's address
// behind the call's opcode, and gsc_rules_patch swaps the address of a replaced function for its replacement's in
// every loaded script. Nothing runs between two loads, so it comes to the same, without a hook in the VM's hot path.
// Like the plugin's, this reaches the calls scripts make, not the ones the game itself makes by function handle,
// and only the server's scripts (the mod ships no client patch scripts).
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "dobby.h"
#include "offsets.h"
#include "gsc_obj.h"
#include "gsc_detours.h"

#define Script_Server 0
#define Asset_Script 0x36 // scriptparsetree
// the game's table of loaded scripts: 500 entries of 0x60 bytes per script instance, a script's bytes at +0
#define Obj_Entry_Size 0x60
#define Obj_Max 500

typedef struct {
    const char *name;
    int32_t length;
    int32_t pad;
    uint8_t *buffer;
} script_asset_t;

static uint64_t base;
static script_asset_t *(*DB_FindXAssetHeader)(int type, const char *name, bool error_if_missing, int wait_time);
static void *(*Dvar_FindByHash)(uint32_t hash);
static const char *(*Dvar_GetString)(const void *dvar);
typedef void *(*builtin_lookup_t)(int inst, uint32_t name, int *kind, int *min_params, int *max_params);
static builtin_lookup_t builtin_function, builtin_method;
static char (*linker_skips_dev_calls)(void);

// this map load's state; the game empties its table of loaded scripts before each one
static gsc_rules_t rules;
static bool zm_seen;       // a scripts/zm/ script has loaded
static bool map_known;     // map_name was read, "" for a map without patch scripts
static char map_name[64];
static bool mains_loaded;
static uint32_t looked_at; // how many of the loaded scripts were checked for a patch script
static uint32_t patched;   // how many had their calls redirected
static uint32_t patch_scripts, calls_changed;

static uint32_t script_count(void) {
    return *(uint32_t *)(base + ADDR_scrObjCount + Script_Server * sizeof(uint32_t));
}

static uint8_t *script_at(uint32_t index) {
    return *(uint8_t **)(base + ADDR_scrObjTable + (uint64_t)Script_Server * Obj_Max * Obj_Entry_Size +
                         (uint64_t)index * Obj_Entry_Size);
}

static bool is_loaded(const uint8_t *obj) {
    for (uint32_t i = 0, count = script_count(); i < count && i < Obj_Max; i++)
        if (script_at(i) == obj)
            return true;
    return false;
}

// the script asset of that name (given without ".gsc"), NULL when no loaded fastfile has one
static script_asset_t *find_script(const char *name) {
    char asset_name[192];
    if (snprintf(asset_name, sizeof(asset_name), "%s.gsc", name) >= (int)sizeof(asset_name))
        return NULL;
    script_asset_t *asset = DB_FindXAssetHeader(Asset_Script, asset_name, false, 0);
    return asset != NULL && asset->buffer != NULL && asset->length > 0 ? asset : NULL;
}

#pragma mark - what the linker would find

static const uint8_t *game_script(void *context, const char *include) {
    (void)context;
    script_asset_t *asset = find_script(include);
    uint64_t magic = 0;
    if (asset != NULL && asset->length >= Gsc_Header_Size)
        memcpy(&magic, asset->buffer, sizeof(magic));
    return magic == Gsc_Magic ? asset->buffer : NULL;
}

static bool game_builtin(void *context, uint32_t name, bool method, int *kind, int *min_params, int *max_params) {
    (void)context;
    *kind = *min_params = *max_params = 0;
    return (method ? builtin_method : builtin_function)(Script_Server, name, kind, min_params, max_params) != NULL;
}

// How many entries of the game's table loading a script takes: one for itself and one for every script it includes,
// at any depth, that isn't loaded yet. The game doesn't check the table's end (the client's table starts there).
static uint32_t entries_needed(const uint8_t *obj, const uint8_t **counted, uint32_t *counted_count) {
    for (uint32_t i = 0; i < *counted_count; i++)
        if (counted[i] == obj)
            return 0;
    if (is_loaded(obj))
        return 0;
    if (*counted_count == Obj_Max)
        return Obj_Max;
    counted[(*counted_count)++] = obj;
    uint32_t needed = 1;
    for (unsigned i = 0, includes = gsc_obj_include_count(obj); i < includes && needed < Obj_Max; i++) {
        const uint8_t *included = game_script(NULL, gsc_obj_include(obj, i));
        if (included != NULL)
            needed += entries_needed(included, counted, counted_count);
    }
    return needed;
}

// whether the game can take one more script with these includes and calls, the reason in why if not
static bool can_load(const uint8_t *obj, char *why, size_t why_size) {
    gsc_link_env_t env = { game_script, game_builtin, NULL, linker_skips_dev_calls() != 0 };
    if (!gsc_obj_links(obj, &env, why, why_size))
        return false;
    const uint8_t *counted[Obj_Max];
    uint32_t counted_count = 0, needed = entries_needed(obj, counted, &counted_count);
    if (needed > Obj_Max || script_count() > Obj_Max - needed) {
        snprintf(why, why_size, "the game's table of loaded scripts has %u of %u entries left, it needs %u",
                 Obj_Max - (script_count() < Obj_Max ? script_count() : Obj_Max), Obj_Max, needed);
        return false;
    }
    return true;
}

#pragma mark - loading

static void after_load(void);

static void new_map_load(void) {
    if (patch_scripts != 0)
        printf("script patches: the last map had %u patch scripts, %u of %u functions replaced, %u calls redirected\n",
               patch_scripts, rules.resolved, rules.count, calls_changed);
    gsc_rules_clear(&rules);
    zm_seen = map_known = mains_loaded = false;
    looked_at = patched = patch_scripts = calls_changed = 0;
}

// Scr_LoadScript(inst, name): loads the script asset <name>.gsc (.csc for the client's), the scripts it includes
// first, and links it. Returns whether it did.
install_hook_name(Scr_LoadScript, int, int inst, const char *name) {
    if (inst != Script_Server)
        return orig_Scr_LoadScript(inst, name);
    if (script_count() == 0 || script_count() < looked_at)
        new_map_load();
    int loaded = orig_Scr_LoadScript(inst, name);
    if (loaded == 0)
        return loaded;
    if (name != NULL && (strncasecmp(name, "scripts/zm/", 11) == 0 || strncasecmp(name, "scripts\\zm\\", 11) == 0) &&
        name[11] != '\0')
        zm_seen = true;
    after_load();
    return loaded;
}

// Loads the patch script of that name (without ".gsc"). Returns false only when the mod has no asset by that name;
// one it has but can't use is reported and counts as found, as on Windows, where the map's own then still keeps the
// one for every map out.
static bool load_patch_script(const char *name) {
    script_asset_t *asset = find_script(name);
    if (asset == NULL)
        return false;
    if (!gsic_is(asset->buffer, (size_t)asset->length))
        return true; // a plain script under that name patches nothing
    gsic_t gsic;
    if (!gsic_parse(asset->buffer, (size_t)asset->length, &gsic)) {
        printf("script patches: %s.gsc isn't a sound patch script, left out\n", name);
        return true;
    }
    if (gsic.count == 0 || is_loaded(gsic.body))
        return true;
    uint8_t *whole = asset->buffer, *body = whole + (gsic.body - whole);
    int32_t length = asset->length;
    char why[200] = "";
    gsc_include_backup_t includes;
    gsc_includes_strip(body, &includes);
    bool ok = can_load(body, why, sizeof(why));
    if (ok && gsic.count > Gsic_Max_Detours - rules.count) {
        snprintf(why, sizeof(why), "more than %u replaced functions", Gsic_Max_Detours);
        ok = false;
    }
    if (ok) {
        // the game looks the asset up by name and takes its bytes as the script
        asset->buffer = body;
        asset->length = (int32_t)gsic.body_size;
        ok = orig_Scr_LoadScript(Script_Server, name) != 0 && is_loaded(body);
        asset->buffer = whole;
        asset->length = length;
        if (!ok)
            snprintf(why, sizeof(why), "the game didn't load it");
    }
    gsc_includes_restore(body, &includes);
    if (!ok) {
        printf("script patches: %s.gsc left out: %s\n", name, why);
        return true;
    }
    gsc_rules_add(&rules, &gsic);
    patch_scripts++;
    printf("script patches: loaded %s.gsc, it replaces %u functions\n", name, gsic.count);
    return true;
}

// one of the mod's two main scripts, if it has it
static void load_main(const char *name) {
    script_asset_t *asset = find_script(name);
    if (asset == NULL || !gsc_obj_check(asset->buffer, (size_t)asset->length) || is_loaded(asset->buffer))
        return;
    char why[200] = "";
    gsc_include_backup_t includes;
    gsc_includes_strip(asset->buffer, &includes);
    bool ok = can_load(asset->buffer, why, sizeof(why));
    if (ok && orig_Scr_LoadScript(Script_Server, name) == 0) {
        snprintf(why, sizeof(why), "the game didn't load it");
        ok = false;
    }
    gsc_includes_restore(asset->buffer, &includes);
    if (ok)
        printf("script patches: loaded %s.gsc\n", name);
    else
        printf("script patches: %s.gsc left out: %s\n", name, why);
}

static void read_map_name(void) {
    map_known = true;
    map_name[0] = '\0';
    void *dvar = Dvar_FindByHash(gsc_hash("mapname"));
    const char *value = dvar != NULL ? Dvar_GetString(dvar) : NULL;
    if (value == NULL || strlen(value) >= sizeof(map_name))
        return;
    for (size_t i = 0; i <= strlen(value); i++)
        map_name[i] = value[i] >= 'A' && value[i] <= 'Z' ? (char)(value[i] + 0x20) : value[i];
    // patch scripts are for zombies maps only
    if (!gsc_map_name_valid(map_name))
        map_name[0] = '\0';
}

// after the game loaded a server script, with everything it includes
static void after_load(void) {
    if (!zm_seen)
        return;
    if (!map_known)
        read_map_name();
    if (map_name[0] == '\0' || script_count() > Obj_Max)
        return;
    // the patch script of each script loaded since the last look, and of the ones those loads brought in
    while (looked_at < script_count()) {
        char name[160];
        const char *script = gsc_obj_name(script_at(looked_at++));
        // the map's own patch script for it, else the one for every map
        if (gsc_mirror_name(script, map_name, name, sizeof(name)) && load_patch_script(name))
            continue;
        if (gsc_mirror_name(script, NULL, name, sizeof(name)))
            load_patch_script(name);
    }
    if (!mains_loaded) {
        char name[160];
        mains_loaded = true;
        load_main("scripts/aae_common/main");
        snprintf(name, sizeof(name), "scripts/aae_level/%s/main", map_name);
        load_main(name);
        looked_at = script_count();
    }
    if (rules.count == 0)
        return;
    const uint8_t *scripts[Obj_Max];
    uint32_t count = script_count();
    for (uint32_t i = 0; i < count; i++)
        scripts[i] = script_at(i);
    // a replacement found now applies to the scripts loaded before it too
    uint32_t found = gsc_rules_resolve(&rules, scripts, count), changed = 0;
    for (uint32_t i = found != 0 ? 0 : patched; i < count; i++)
        changed += gsc_rules_patch(&rules, (uint8_t *)scripts[i]);
    patched = count;
    calls_changed += changed;
    if (found != 0 || changed != 0)
        printf("script patches: %u of %u functions replaced, %u calls redirected in %u scripts (the game holds %u)\n",
               rules.resolved, rules.count, calls_changed, count, Obj_Max);
}

void gsc_detours_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base = game_base_address;
    DB_FindXAssetHeader = (void *)(base + ADDR_DB_FindXAssetHeader);
    Dvar_FindByHash = (void *)(base + ADDR_Dvar_FindByHash);
    Dvar_GetString = (void *)(base + ADDR_Dvar_GetString);
    builtin_function = (builtin_lookup_t)(base + ADDR_Scr_GetBuiltinFunction);
    builtin_method = (builtin_lookup_t)(base + ADDR_Scr_GetBuiltinMethod);
    linker_skips_dev_calls = (void *)(base + ADDR_Scr_LinkerSkipsDevCalls);
    install_hook_Scr_LoadScript((void *)(base + ADDR_Scr_LoadScript));
    printf("All-around Enhancement's patch scripts load with the scripts they patch\n");
}
