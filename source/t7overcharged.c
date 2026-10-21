// macOS replacement for T7Overcharged (https://github.com/JariKCoding/T7Overcharged),
// a Windows DLL that workshop mods such as All-around Enhancement load through Lua.
#include <errno.h>
#include <os/lock.h>
#include <stdatomic.h>
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
#include "t7o_subtitle.h"
#include "weapon_anim.h"
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
        // BO3MACFIX_LUAMISSING=1: log the Engine and CoD functions scripts look for that this game doesn't have
        const char *log_missing = getenv("BO3MACFIX_LUAMISSING");
        hks_push_boolean(s, log_missing != NULL && atoi(log_missing) != 0);
        hks_pcall(s, 8, 0);
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
// AAE's other three, startweaponanim, playanim and anim_event, play weapon animations (inferred from aae_core.csc:
// the server sends "firstraise", "reloadempty" and "lowready" on its "deadshot_keyline" client system, and the client
// starts the weapon's animation and its event; a debug loop also plays any set in the anim2 and anim3 dvars). The real
// DLL's code isn't in the mod. They get the 0 the game's isprofilebuild gave them, without the missing line; the
// callers start them as threads or statements and don't use the result (inferred from the import flags).
// - startweaponanim(localclientnum, state, hand): with the WeaponAnims setting on (BO3MACFIX_WEAPONANIMS=1), the
//   client's viewmodel update starts that weapon animation state (weapon_anim.h); on by default. Otherwise
//   the weapon just keeps its usual animation.
// - anim_event(x, event): the event is one of the player's third-person anim script events (15 FIRSTRAISEWEAPON,
//   17 RELOAD_EMPTY, 101 FLOURISH in the game's table at 0x1020fba20), which first person doesn't show: still a no-op.
// - playanim: only AAE's debug loop calls it, still a no-op.
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
// both script VMs' threads log through it, so a native is logged once
static atomic_bool native_logged[Native_Count];

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

#pragma mark - startweaponanim

#define WeaponAnim_LocalClients 4
#define Cg_PredictedWeapAnim0 0x11a934
#define Cg_PredictedWeapAnim1 0x11a950
#define ViewWeaponState_Size 0x3a0
#define ViewWeaponState_LastWeapAnim0 0x36c
#define ViewWeaponState_LastWeapAnim1 0x374
#define WeaponAnim_LogLines 12

static bool weapon_anims_enabled;
// the state each local client's hand is to start at its next viewmodel update, 0 for none
static atomic_int weapon_anim_requests[WeaponAnim_LocalClients][2];
static atomic_int weapon_anim_log_count;

static void log_weapon_anim(const char *what, int local_client, int hand, int state) {
    if (atomic_fetch_add_explicit(&weapon_anim_log_count, 1, memory_order_relaxed) < WeaponAnim_LogLines)
        printf("T7Overcharged: startweaponanim %s (local client %d, hand %d, state %d)\n", what, local_client, hand,
               state);
}

// startweaponanim(localclientnum, state, hand), from the client's scripts: for its next viewmodel update
static void request_weapon_anim(int inst) {
    if (inst != Script_Client || !param_is_int(inst, 1) || !param_is_int(inst, 2) || !param_is_int(inst, 3))
        return;
    int local_client = Scr_GetInt(inst, 1), state = Scr_GetInt(inst, 2), hand = Scr_GetInt(inst, 3);
    if (local_client < 0 || local_client >= WeaponAnim_LocalClients || !weapon_anim_request_valid(hand, state)) {
        log_weapon_anim("ignored", local_client, hand, state);
        return;
    }
    atomic_store_explicit(&weapon_anim_requests[local_client][hand], state, memory_order_relaxed);
}

// the client's viewmodel update, both hands: a requested state is put in the predicted player state for this one call,
// so the engine starts it its own way (blend, notetracks, sounds), then the real value goes back in both places
install_hook_name(CG_UpdateViewWeaponAnim, uint64_t, int local_client, int arg) {
    int states[2] = { 0, 0 };
    if (local_client >= 0 && local_client < WeaponAnim_LocalClients) {
        for (int hand = 0; hand < 2; hand++) {
            if (atomic_load_explicit(&weapon_anim_requests[local_client][hand], memory_order_relaxed) != 0)
                states[hand] =
                    atomic_exchange_explicit(&weapon_anim_requests[local_client][hand], 0, memory_order_relaxed);
        }
    }
    if (states[0] == 0 && states[1] == 0)
        return orig_CG_UpdateViewWeaponAnim(local_client, arg);
    uint8_t *cg = *(uint8_t **)(game_base_address + ADDR_cgArray);
    uint8_t *views = *(uint8_t **)(game_base_address + ADDR_cgViewWeaponStates);
    if (cg == NULL || views == NULL || local_client >= *(const int *)(game_base_address + ADDR_LOCAL_CLIENT_COUNT))
        return orig_CG_UpdateViewWeaponAnim(local_client, arg);
    cg += (size_t)local_client * Cg_Size;
    uint8_t *view = views + (size_t)local_client * ViewWeaponState_Size;
    // the update leaves at once without the viewmodel's object (0x100d8734a)
    if (*(void **)view == NULL)
        return orig_CG_UpdateViewWeaponAnim(local_client, arg);
    static const size_t predicted[2] = { Cg_PredictedWeapAnim0, Cg_PredictedWeapAnim1 };
    static const size_t last[2] = { ViewWeaponState_LastWeapAnim0, ViewWeaponState_LastWeapAnim1 };
    uint16_t real[2] = { 0, 0 };
    bool injected[2] = { false, false };
    for (int hand = 0; hand < 2; hand++) {
        if (states[hand] == 0)
            continue;
        real[hand] = *(uint16_t *)(cg + predicted[hand]);
        uint16_t last_started = *(uint16_t *)(view + last[hand]);
        if (!weapon_anim_can_inject(real[hand], last_started)) {
            log_weapon_anim("left to the engine's own new one", local_client, hand, states[hand]);
            continue;
        }
        *(uint16_t *)(cg + predicted[hand]) = weapon_anim_injected(real[hand], last_started, states[hand]);
        injected[hand] = true;
        log_weapon_anim("started", local_client, hand, states[hand]);
    }
    uint64_t result = orig_CG_UpdateViewWeaponAnim(local_client, arg);
    for (int hand = 0; hand < 2; hand++) {
        if (injected[hand]) {
            *(uint16_t *)(cg + predicted[hand]) = real[hand];
            *(uint16_t *)(view + last[hand]) = real[hand];
        }
    }
    return result;
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
        // a plain load once it's logged; the exchange makes only one thread log it
        if (!atomic_load_explicit(&native_logged[native], memory_order_relaxed) &&
            !atomic_exchange_explicit(&native_logged[native], true, memory_order_relaxed)) {
            bool played = native == Native_StartWeaponAnim && weapon_anims_enabled;
            printf("T7Overcharged: scripts use %s%s\n", native_names[native],
                   no_op && !played ? " (no weapon animations here, they get 0)" : "");
        }
        if (no_op) {
            if (native == Native_StartWeaponAnim && weapon_anims_enabled)
                request_weapon_anim(inst);
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

void t7o_install_script_functions(bool weapon_anims) {
    Scr_GetInt = (void *)(game_base_address + ADDR_Scr_GetInt);
    Scr_GetString = (void *)(game_base_address + ADDR_Scr_GetString);
    Scr_AddInt = (void *)(game_base_address + ADDR_Scr_AddInt);
    Scr_AddVector = (void *)(game_base_address + ADDR_Scr_AddVector);
    for (int native = 0; native < Native_Count; native++)
        native_hashes[native] = script_hash(native_names[native]);
    install_hook_GScr_IsProfileBuild((void *)(game_base_address + ADDR_GScr_IsProfileBuild));
    // WeaponAnims (BO3MACFIX_WEAPONANIMS, on by default): startweaponanim plays the animation. The hook has a slot
    // in the pre-patched copy, and is only installed with the setting on
    if (weapon_anims) {
        weapon_anims_enabled = true;
        install_hook_CG_UpdateViewWeaponAnim((void *)(game_base_address + ADDR_CG_UpdateViewWeaponAnim));
        printf("T7Overcharged: WeaponAnims on, startweaponanim plays through the viewmodel update\n");
    }
}

#pragma mark - printsubtitle

// All-around Enhancement shows the game's messages (iprintln: the bank's "Current Balance", its deposits, the chat
// commands' answers...) in a game message window of its own HUD (window 0, in its own font). With T7Overcharged there
// (its DiscordRPC table, which ours has too), AAE's HUD takes each new line from the game's list and runs
//     printsubtitle "<message>" 1 5000 <controller> 730 0
// to print it there, then empties the line in the list. The Mac build has no printsubtitle command, so every one of
// those messages was lost: "Unknown command" in the console, nothing on screen. Ours prints the message the way its
// arguments read for the game's CL_ConsolePrint: channel 1 (gamenotify) is what window 0 shows, for 5 seconds, 730
// pixels wide.
static void (*CL_ConsolePrint)(int local_client, int channel, const char *text, int duration, int pixel_width,
                                int flags);
static void *(*Cmd_FindCommand)(const char *name);
static void (*Cmd_AddCommandInternal)(const char *name, void (*function)(void), void *command);
static uint8_t *(*Sys_GetTLS)(void);
// the game's cmd_function_s for it, which Cmd_AddCommandInternal links into its list
static _Alignas(8) uint8_t printsubtitle_command[0x30];
static atomic_bool printsubtitle_logged;

#define TLS_CmdArgs 0x18
#define CmdArgs_Argc 0x64
#define CmdArgs_Argv 0x88
#define CmdArgs_Nesting_Max 8

static void printsubtitle_f(void) {
    // the arguments of the command being run, read like the game's own commands do
    uint8_t *tls = Sys_GetTLS();
    const uint8_t *args = tls != NULL ? *(const uint8_t *const *)(tls + TLS_CmdArgs) : NULL;
    if (args == NULL)
        return;
    int nesting = *(const int *)args;
    if (nesting < 0 || nesting >= CmdArgs_Nesting_Max)
        return;
    int argc = *(const int *)(args + CmdArgs_Argc + 4 * (size_t)nesting);
    const char *const *argv = *(const char *const *const *)(args + CmdArgs_Argv + 8 * (size_t)nesting);
    t7o_subtitle message;
    if (argv == NULL ||
        !t7o_parse_printsubtitle(argc, argv, *(const int *)(game_base_address + ADDR_LOCAL_CLIENT_COUNT), &message))
        return;
    if (!atomic_load_explicit(&printsubtitle_logged, memory_order_relaxed) &&
        !atomic_exchange_explicit(&printsubtitle_logged, true, memory_order_relaxed))
        printf("T7Overcharged: printsubtitle shows the mod's messages\n");
    CL_ConsolePrint(message.local_client, message.channel, message.text, message.duration, message.pixel_width,
                    message.flags);
}

#pragma mark - SendMenuResponse

// All-around Enhancement's key binds ("Enhanced Controls" in its pause menu: flashlight, ping, third person, the
// overhead map, hiding the HUD...) are console binds like
//     bind Y "SendMenuResponse StartMenu_Main aae_flashlight 200"
// and its GSC waits for those menu responses. The Mac build has no SendMenuResponse console command (only the Lua
// function), so every one of those keys printed "Unknown command" and did nothing. Ours sends the response the way
// Engine.SendMenuResponse does for controller 0: the menu's and the response's indexes in the client's config
// strings, then "cmd mrp"/"cmd mr" queued for the server. The number after the response is left alone.
static int (*CL_ControllerToLocalClient)(int controller);
static int (*CL_MenuIndex)(int instance, const char *menu);
static int (*CL_MenuResponseIndex)(int instance, const char *response);
static void (*Cbuf_AddText)(int local_client, const char *text);
static _Alignas(8) uint8_t sendmenuresponse_command[0x30];
static atomic_bool sendmenuresponse_logged;

static void sendmenuresponse_f(void) {
    uint8_t *tls = Sys_GetTLS();
    const uint8_t *args = tls != NULL ? *(const uint8_t *const *)(tls + TLS_CmdArgs) : NULL;
    if (args == NULL)
        return;
    int nesting = *(const int *)args;
    if (nesting < 0 || nesting >= CmdArgs_Nesting_Max)
        return;
    int argc = *(const int *)(args + CmdArgs_Argc + 4 * (size_t)nesting);
    const char *const *argv = *(const char *const *const *)(args + CmdArgs_Argv + 8 * (size_t)nesting);
    t7o_menu_response response;
    if (argv == NULL || !t7o_parse_sendmenuresponse(argc, argv, &response))
        return;
    int local_client = CL_ControllerToLocalClient(0);
    if (local_client < 0 || local_client >= *(const int *)(game_base_address + ADDR_LOCAL_CLIENT_COUNT))
        local_client = 0;
    // 1 is the client's config strings, as in Engine.SendMenuResponse; 0 means the menu isn't there (not in a game)
    int menu_index = CL_MenuIndex(1, response.menu);
    if (menu_index == 0) {
        printf("T7Overcharged: SendMenuResponse: no menu %s here\n", response.menu);
        return;
    }
    int response_index = CL_MenuResponseIndex(1, response.response);
    char line[320];
    if (!t7o_format_menu_response(line, sizeof(line), *(const int *)(game_base_address + ADDR_CL_SERVER_ID), menu_index,
                                  response_index, response.response))
        return;
    if (!atomic_load_explicit(&sendmenuresponse_logged, memory_order_relaxed) &&
        !atomic_exchange_explicit(&sendmenuresponse_logged, true, memory_order_relaxed))
        printf("T7Overcharged: SendMenuResponse sends the mod's key binds (%s)\n", response.response);
    Cbuf_AddText(local_client, line);
}

void t7o_install_commands(void) {
    CL_ConsolePrint = (void *)(game_base_address + ADDR_CL_ConsolePrint);
    CL_ControllerToLocalClient = (void *)(game_base_address + ADDR_CL_ControllerToLocalClient);
    CL_MenuIndex = (void *)(game_base_address + ADDR_CL_MenuIndex);
    CL_MenuResponseIndex = (void *)(game_base_address + ADDR_CL_MenuResponseIndex);
    Cbuf_AddText = (void *)(game_base_address + ADDR_Cbuf_AddText);
    Cmd_FindCommand = (void *)(game_base_address + ADDR_Cmd_FindCommand);
    Cmd_AddCommandInternal = (void *)(game_base_address + ADDR_Cmd_AddCommandInternal);
    Sys_GetTLS = (void *)(game_base_address + ADDR_Sys_GetTLS);
    if (Cmd_FindCommand("printsubtitle") == NULL)
        Cmd_AddCommandInternal("printsubtitle", printsubtitle_f, printsubtitle_command);
    if (Cmd_FindCommand("SendMenuResponse") == NULL)
        Cmd_AddCommandInternal("SendMenuResponse", sendmenuresponse_f, sendmenuresponse_command);
}
