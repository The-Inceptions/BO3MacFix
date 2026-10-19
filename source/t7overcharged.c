// macOS replacement for T7Overcharged (https://github.com/JariKCoding/T7Overcharged),
// a Windows DLL that workshop mods such as All-around Enhancement load through Lua.
#include <errno.h>
#include <os/lock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include "dobby.h"
#include "havok.h"
#include "offsets.h"
#include "t7overcharged.h"
#include "t7overcharged_lua.h"
#include "utilities.h"

// defined in bo3macfix.c
extern uint64_t game_base_address;
void lua_correct_path(const char *in_path, char *out_path, size_t out_path_len);
// defined in bo3macnative.m
char *copy_clipboard_text();
void set_clipboard_text(const char *text);
bool http_get_async(const char *url, const char *key);
char *http_copy_result(const char *key);

#define ASSET_TYPE_COUNT 0x6B
#define MAX_ASSET_LIMIT 100000

typedef struct _XAssetPool {
    void *pool;
    uint32_t itemSize;
    int32_t itemCount;
    bool isSingleton;
    int32_t itemAllocCount;
    void *freeHead;
} XAssetPool;
_Static_assert(sizeof(XAssetPool) == 0x20, "XAssetPool must match the game's layout");

// the folder the mod's T7Overcharged DLL is in
static char dll_dir[4096];

bool t7o_is_overcharged_dll(const char *path) {
    const char *name = filename_from_path(path);
    return strcasecmp(name, "T7Overcharged.ff") == 0 || strcasecmp(name, "T7Overcharged.dll") == 0;
}

// Raises an asset pool's limit by chaining a new block of entries onto its free list.
static void extend_asset_pool(int type, const char *name, int new_limit) {
    if (type < 0 || type >= ASSET_TYPE_COUNT || new_limit > MAX_ASSET_LIMIT)
        return;
    // make sure the mod means the same asset type as us
    const char *pool_name = ((const char **)(game_base_address + ADDR_g_assetNames))[type];
    if (strcmp(pool_name, name) != 0) {
        printf("T7Overcharged: asset type %i is '%s', not '%s'\n", type, pool_name, name);
        return;
    }
    XAssetPool *pool = (XAssetPool *)(game_base_address + ADDR_DB_XAssetPool) + type;
    if (pool->isSingleton || pool->itemSize < sizeof(void *) || new_limit <= pool->itemCount)
        return;
    int added = new_limit - pool->itemCount;
    uint8_t *block = calloc(added, pool->itemSize);
    if (block == NULL)
        return;
    for (int i = 0; i < added; i++) {
        void **entry = (void **)(block + (size_t)i * pool->itemSize);
        *entry = (i + 1 < added) ? block + (size_t)(i + 1) * pool->itemSize : pool->freeHead;
    }
    pool->freeHead = block;
    printf("T7Overcharged: raised %s limit from %i to %i\n", name, pool->itemCount, new_limit);
    pool->itemCount = new_limit;
}

// Applies the mod's T7Overcharged/assetlimits.txt, made of "<type> <name> <default limit> <new limit>" lines.
static void apply_asset_limits() {
    char path[4200];
    snprintf(path, sizeof(path), "%s/T7Overcharged/assetlimits.txt", dll_dir);
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        int type, default_limit, new_limit;
        char name[64];
        if (sscanf(line, "%i %63s %i %i", &type, name, &default_limit, &new_limit) == 4)
            extend_asset_pool(type, name, new_limit);
    }
    fclose(fp);
}

static int lua_create_directory(lua_State *s) {
    char path[4096];
    lua_correct_path(hks_check_string(s, 1), path, sizeof(path));
    // like mkdir -p, create any missing parent folders too
    bool ok = true;
    for (char *p = path + 1; ok && *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            ok = mkdir(path, 0755) == 0 || errno == EEXIST;
            *p = '/';
        }
    }
    if (ok)
        ok = mkdir(path, 0755) == 0 || errno == EEXIST;
    hks_push_boolean(s, ok);
    return 1;
}

static int lua_directory_exists(lua_State *s) {
    char path[4096];
    lua_correct_path(hks_check_string(s, 1), path, sizeof(path));
    struct stat st;
    hks_push_boolean(s, stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    return 1;
}

static int lua_clipboard_get(lua_State *s) {
    char *text = copy_clipboard_text();
    hks_push_string(s, text != NULL ? text : "");
    free(text);
    return 1;
}

static int lua_clipboard_set(lua_State *s) {
    set_clipboard_text(hks_check_string(s, 1));
    return 0;
}

static int aae_online = 0;

void t7o_set_online(int enabled) {
    aae_online = enabled;
}

// Axios.GetUpdate(url, key): only with AAEOnline = 1, and only to the hosts http_get_async allows
static int lua_http_get(lua_State *s) {
    const char *url = hks_check_string(s, 1), *key = hks_check_string(s, 2);
    bool started = aae_online && url != NULL && key != NULL && http_get_async(url, key);
    hks_push_boolean(s, started);
    return 1;
}

// Axios.GetRetrieve(key): the response text, or nil until there is one
static int lua_http_result(lua_State *s) {
    const char *key = hks_check_string(s, 1);
    char *text = key != NULL ? http_copy_result(key) : NULL;
    if (text != NULL)
        hks_push_string(s, text);
    else
        hks_push_nil(s);
    free(text);
    return 1;
}

static int lua_print(lua_State *s) {
    printf("%s\n", hks_check_string(s, 1));
    return 0;
}

// Our version of T7Overcharged's init(): raises the mod's asset limits, then sets up the Lua libraries it provides.
static int t7o_init(lua_State *s) {
    // the stack can be reallocated while our Lua runs, so remember the top as an index
    ptrdiff_t top = HKS_TOP(s) - HKS_BASE(s);
    apply_asset_limits();
    if (hks_load_source(s, (const char *)t7overcharged_lua, t7overcharged_lua_len, "=BO3MacFix T7Overcharged")) {
        hks_push_cfunction(s, lua_create_directory, "CreateDirectory");
        hks_push_cfunction(s, lua_directory_exists, "DirectoryExists");
        hks_push_cfunction(s, lua_clipboard_get, "ClipboardGet");
        hks_push_cfunction(s, lua_clipboard_set, "ClipboardSet");
        hks_push_cfunction(s, lua_print, "Print");
        hks_push_cfunction(s, lua_http_get, "HttpGet");
        hks_push_cfunction(s, lua_http_result, "HttpResult");
        hks_pcall(s, 7, 0);
    }
    HKS_TOP(s) = HKS_BASE(s) + top;
    return 0;
}

void t7o_push_init(lua_State *s, const char *dll_path) {
    strncpy(dll_dir, dll_path, sizeof(dll_dir) - 1);
    char *last_slash = strrchr(dll_dir, '/');
    if (last_slash != NULL)
        *last_slash = '\0';
    hks_push_cfunction(s, t7o_init, "init");
}

#pragma mark - script functions

// T7Overcharged also gives GSC and CSC new natives, through the isprofilebuild builtin: a script calls it through a
// function pointer, so the linker doesn't check the parameters, with the native's hash first. All-around
// Enhancement's scripts wrap them like this:
//     function hash(name) { return [[ &isprofilebuild ]](0x22ea46c7, name); }
//     function getvieworigin(localclientnum) { return [[ &isprofilebuild ]](hash("getvieworigin"), localclientnum); }
// The game's isprofilebuild ignores its parameters and returns 0, so on the Mac AAE's client scripts got 0 for the view
// origin and indexed it every frame (its HUD movement and the view code its smoke trails and zombie health bars use):
// about 2,000 script errors a second in a match, each one a call stack in console_mp.log. We answer hash and the
// natives below; any other call still gets 0, and is logged once as missing.
//
// AAE's other three, startweaponanim, playanim and anim_event, play viewmodel animations (inferred from aae_core.csc:
// the server sends "firstraise", "reloadempty" and "lowready" on its "deadshot_keyline" client system, and the client
// starts the weapon's animation and its event; a debug loop also plays any set in the anim2 and anim3 dvars). The real
// DLL's code isn't in the mod, and the game has no script call for a viewmodel animation, so they stay no-ops here:
// they get the 0 the game's isprofilebuild gave them, without the missing line. The callers start them as threads or
// statements and don't use the result (inferred from the import flags), so the weapon just keeps its usual animation.
#define Hash_Native 0x22ea46c7u

typedef struct {
    uint64_t value;
    uint32_t type;
    uint32_t pad;
} VariableValue;
_Static_assert(sizeof(VariableValue) == 16, "VariableValue must match the game's layout");

enum { Var_String = 2, Var_Hash = 5, Var_Int = 7 };
enum { Script_Server = 0, Script_Client = 1 };
#define VmPub_Size 0xa048
#define VmPub_Top 0x20
#define VmPub_OutParamCount 0x38
#define Cg_Size 0x342a80
#define Cg_ViewOrigin 0x131dc8
#define Cg_ViewAngles 0x2d8930

static int (*Scr_GetInt)(int inst, unsigned int index);
static const char *(*Scr_GetString)(int inst, unsigned int index);
static void (*Scr_AddInt)(int inst, int value);
static void (*Scr_AddVector)(int inst, const float *value);

// the type of a parameter, or -1 if there isn't one
static int param_type(int inst, unsigned int index) {
    const uint8_t *vm = (const uint8_t *)(game_base_address + ADDR_scrVmPub) + (size_t)inst * VmPub_Size;
    if (index >= *(const uint32_t *)(vm + VmPub_OutParamCount))
        return -1;
    const VariableValue *top = *(VariableValue *const *)(vm + VmPub_Top);
    return (int)(top - index)->type;
}

static bool param_is_int(int inst, unsigned int index) {
    int type = param_type(inst, index);
    return type == Var_Int || type == Var_Hash;
}

// the game's canonical hash (the one it names script functions by), so a native's hash is the same as its name's
static uint32_t script_hash(const char *name) {
    uint32_t hash = 0x4B9ACE2F;
    for (; *name != '\0'; name++)
        hash = ((uint32_t)tolower((unsigned char)*name) ^ hash) * 0x1000193;
    return hash * 0x1000193;
}

// the natives, by hash, and whether each was logged yet
typedef enum {
    Native_GetViewOrigin, Native_GetViewAngles, // vectors from the local client's cg_t
    Native_StartWeaponAnim, Native_PlayAnim, Native_AnimEvent, // no-ops, 0
    Native_Count
} native_t;
static const char *const native_names[Native_Count] = { "getvieworigin", "getviewang", "startweaponanim", "playanim",
                                                        "anim_event" };
static uint32_t native_hashes[Native_Count];
static bool native_logged[Native_Count];

// names scripts asked hash() for, so a native we don't have can be logged by name
#define Hashed_Names 32
static struct {
    uint32_t hash;
    bool logged;
    char name[48];
} hashed_names[Hashed_Names];
static unsigned int hashed_name_count;
// the client's and the server's script VMs run on different threads and both use the table
static os_unfair_lock hashed_names_lock = OS_UNFAIR_LOCK_INIT;

static void remember_hashed_name(uint32_t hash, const char *name) {
    os_unfair_lock_lock(&hashed_names_lock);
    bool known = false;
    for (unsigned int i = 0; i < hashed_name_count && !known; i++)
        known = hashed_names[i].hash == hash;
    if (!known && hashed_name_count < Hashed_Names) {
        hashed_names[hashed_name_count].hash = hash;
        strncpy(hashed_names[hashed_name_count].name, name, sizeof(hashed_names[0].name) - 1);
        hashed_name_count++;
    }
    os_unfair_lock_unlock(&hashed_names_lock);
}

static void log_unknown_native(uint32_t hash) {
    char name[sizeof(hashed_names[0].name)];
    bool log = false;
    os_unfair_lock_lock(&hashed_names_lock);
    for (unsigned int i = 0; i < hashed_name_count; i++) {
        if (hashed_names[i].hash == hash) {
            if (!hashed_names[i].logged) {
                hashed_names[i].logged = log = true;
                memcpy(name, hashed_names[i].name, sizeof(name));
            }
            break;
        }
    }
    os_unfair_lock_unlock(&hashed_names_lock);
    if (log)
        printf("T7Overcharged: no script function \"%s\" here, scripts get 0\n", name);
}

// the local client's cg_t, or NULL
static const uint8_t *local_client_cg(int inst) {
    if (inst != Script_Client || !param_is_int(inst, 1))
        return NULL;
    int local_client = Scr_GetInt(inst, 1);
    const uint8_t *cg = *(const uint8_t *const *)(game_base_address + ADDR_cgArray);
    if (cg == NULL || local_client < 0 || local_client >= *(const int *)(game_base_address + ADDR_LOCAL_CLIENT_COUNT))
        return NULL;
    return cg + (size_t)local_client * Cg_Size;
}

// answers a native, returns false to leave it to the game
static bool call_native(int inst, uint32_t hash) {
    if (hash == Hash_Native) {
        if (param_type(inst, 1) != Var_String)
            return false;
        const char *name = Scr_GetString(inst, 1);
        uint32_t name_hash = script_hash(name);
        remember_hashed_name(name_hash, name);
        Scr_AddInt(inst, (int)name_hash);
        return true;
    }
    for (int native = 0; native < Native_Count; native++) {
        if (hash != native_hashes[native])
            continue;
        bool no_op = native >= Native_StartWeaponAnim;
        const uint8_t *cg = no_op ? NULL : local_client_cg(inst);
        if (!no_op && cg == NULL)
            return false;
        if (!native_logged[native]) {
            native_logged[native] = true;
            printf("T7Overcharged: scripts use %s%s\n", native_names[native],
                   no_op ? " (no weapon animations here, they get 0)" : "");
        }
        if (no_op) {
            Scr_AddInt(inst, 0);
            return true;
        }
        Scr_AddVector(inst, (const float *)(cg + (native == Native_GetViewOrigin ? Cg_ViewOrigin : Cg_ViewAngles)));
        return true;
    }
    log_unknown_native(hash);
    return false;
}

install_hook_name(GScr_IsProfileBuild, void, int inst) {
    if ((inst == Script_Server || inst == Script_Client) && param_is_int(inst, 0) &&
        call_native(inst, (uint32_t)Scr_GetInt(inst, 0)))
        return;
    orig_GScr_IsProfileBuild(inst);
}

void t7o_install_script_functions(void) {
    Scr_GetInt = (void *)(game_base_address + ADDR_Scr_GetInt);
    Scr_GetString = (void *)(game_base_address + ADDR_Scr_GetString);
    Scr_AddInt = (void *)(game_base_address + ADDR_Scr_AddInt);
    Scr_AddVector = (void *)(game_base_address + ADDR_Scr_AddVector);
    for (int native = 0; native < Native_Count; native++)
        native_hashes[native] = script_hash(native_names[native]);
    install_hook_GScr_IsProfileBuild((void *)(game_base_address + ADDR_GScr_IsProfileBuild));
}
