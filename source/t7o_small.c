// T7Overcharged's smaller pieces that All-around Enhancement uses, see t7o_small.h.
//
// customkeymap: All-around Enhancement's loading screen runs
//     customkeymap loadingskip1 27
//     customkeymap loadingskip2 200
// so that Escape (key 27) and a left click (200) skip the map's loading movie: T7Overcharged then runs a line of Lua
// that calls CoD.Mod.CallCustomFunction("loadingskip1"), which the mod's loading screen answers with CoD.SkipLoading().
// The Mac build has no such command, so only the controller's A button skipped. T7Overcharged (its keybind
// component) hooks the game's Sys_QueEvent, where the window's key and mouse messages become the game's input events,
// and for each key that comes *up* (not down), with local client 0's console closed, compares the keys the game holds
// as down plus that key with each mapping's keys; on an exact match it runs the Lua right there, in the UI's Lua VM
// under the VM's own critical section. That is the game's window thread, and the game closes and rebuilds the UI's VM
// on its main thread without that critical section (around a map's start, a renderer restart, a disconnect), so a
// release at that moment would run Lua on freed or half-built memory; Windows has the same race. Here the Mac's
// Sys_QueEvent (the same function: the same event queue, the same calls from the window's message handler) only
// notes the key and the keys held, lock-free; the UI's own Lua takes it on the game's main thread, in a timer of its
// roots (t7overcharged.lua), where this file finds the mapping, applies T7Overcharged's debounce and gives the name,
// and the Lua calls the mod's function as T7Overcharged's line does.
#include <errno.h>
#include <limits.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dobby.h"
#include "gsc_obj.h"
#include "havok.h"
#include "offsets.h"
#include "t7o_small.h"
#include "t7overcharged.h"

#pragma mark - numbers

t7o_number t7o_stoi(const char *text, int *value) {
    if (text == NULL)
        return T7o_Number_Invalid;
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (end == text)
        return T7o_Number_Invalid;
    // a long is 32 bits on Windows, so strtol itself says ERANGE for anything beyond an int there
    if (errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
        return T7o_Number_Range;
    *value = (int)parsed;
    return T7o_Number_Ok;
}

#pragma mark - customkeymap's rules

void t7o_keyset_add(t7o_keyset *set, int key) {
    if (key >= 0 && key < T7o_Keys)
        set->bits[key / 64] |= (uint64_t)1 << (key % 64);
}

bool t7o_keyset_has(const t7o_keyset *set, int key) {
    return key >= 0 && key < T7o_Keys && (set->bits[key / 64] >> (key % 64) & 1) != 0;
}

bool t7o_keyset_equal(const t7o_keyset *a, const t7o_keyset *b) {
    return memcmp(a->bits, b->bits, sizeof(a->bits)) == 0;
}

// a name that can stand between the single quotes of the Lua line as it is. T7Overcharged puts any name there; one
// with a quote or a backslash would change what the line does, so here it isn't taken
static bool lua_quotable(const char *name) {
    size_t length = strnlen(name, T7o_Keymap_Name_Max);
    if (length == 0 || length >= T7o_Keymap_Name_Max)
        return false;
    for (const unsigned char *c = (const unsigned char *)name; *c != '\0'; c++) {
        if (*c < 0x20 || *c == 0x7f || *c == '\'' || *c == '\\')
            return false;
    }
    return true;
}

t7o_keymap_result t7o_keymap_command(t7o_keymap *map, int argc, const char *const *argv) {
    if (map == NULL || argv == NULL || argc < 3)
        return T7o_Keymap_Usage;
    const char *name = argv[1] != NULL ? argv[1] : "", *keys_text = argv[2] != NULL ? argv[2] : "";
    // the parts up to each '+', as std::getline gives them: the text after the last '+' only when there is some
    t7o_keyset keys;
    memset(&keys, 0, sizeof(keys));
    for (const char *part = keys_text; *part != '\0';) {
        const char *plus = strchr(part, '+');
        size_t length = plus != NULL ? (size_t)(plus - part) : strlen(part);
        char *number = strndup(part, length);
        if (number == NULL)
            return T7o_Keymap_Bad_Number;
        int key = 0;
        t7o_number read = t7o_stoi(number, &key);
        free(number);
        if (read != T7o_Number_Ok)
            return T7o_Keymap_Bad_Number;
        // T7Overcharged keeps a key only if (unsigned)key <= 255
        if (key >= 0 && key < T7o_Keys)
            t7o_keyset_add(&keys, key);
        if (plus == NULL)
            break;
        part = plus + 1;
    }
    int debounce = T7o_Keymap_Debounce_Ms;
    if (argc >= 4 && t7o_stoi(argv[3] != NULL ? argv[3] : "", &debounce) != T7o_Number_Ok)
        return T7o_Keymap_Bad_Number;
    if (!lua_quotable(name))
        return T7o_Keymap_Bad_Name;
    // map[keys] = the new callback: the same keys again replace the mapping
    t7o_keymapping *mapping = NULL;
    for (int i = 0; i < map->count && mapping == NULL; i++) {
        if (t7o_keyset_equal(&map->mappings[i].keys, &keys))
            mapping = &map->mappings[i];
    }
    t7o_keymap_result result = mapping != NULL ? T7o_Keymap_Replaced : T7o_Keymap_Added;
    if (mapping == NULL) {
        if (map->count >= T7o_Keymap_Max)
            return T7o_Keymap_Full;
        mapping = &map->mappings[map->count++];
    }
    mapping->keys = keys;
    strcpy(mapping->name, name);
    mapping->debounce_ms = debounce;
    return result;
}

bool t7o_keymap_event_counts(int type, int value2, int key, int quit_in_progress, int key_catchers) {
    return quit_in_progress == 0 && type == 1 && (value2 & 1) == 0 && (key_catchers & 1) == 0 && key >= 0 &&
           key < T7o_Keys;
}

const t7o_keymapping *t7o_keymap_find(const t7o_keymap *map, const t7o_keyset *held, int key) {
    if (map == NULL || held == NULL || key < 0 || key >= T7o_Keys)
        return NULL;
    t7o_keyset wanted = *held;
    t7o_keyset_add(&wanted, key);
    for (int i = 0; i < map->count; i++) {
        if (t7o_keyset_equal(&map->mappings[i].keys, &wanted))
            return &map->mappings[i];
    }
    return NULL;
}

// T7Overcharged's steady_clock difference in whole milliseconds, cut towards zero as its signed division does
static int64_t elapsed_ms(int64_t now_ns, int64_t last_ns) {
    return (now_ns - last_ns) / 1000000;
}

bool t7o_keymap_debounce_passes(t7o_debounce *debounce, int mapping_debounce_ms, int64_t now_ns) {
    // the static is set up by the first call that gets this far, with that mapping's debounce, and keeps it
    if (!debounce->started) {
        debounce->started = true;
        debounce->first = true;
        debounce->debounce_ms = mapping_debounce_ms;
        debounce->last_ns = now_ns;
    }
    if (!debounce->first && elapsed_ms(now_ns, debounce->last_ns) < (int64_t)debounce->debounce_ms)
        return false;
    debounce->last_ns = now_ns;
    debounce->first = false;
    return true;
}

bool t7o_key_queue_put(t7o_key_queue *queue, const t7o_key_event *event) {
    unsigned written = atomic_load_explicit(&queue->written, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&queue->read, memory_order_acquire);
    if (written - read >= T7o_Key_Queue_Size)
        return false;
    queue->events[written % T7o_Key_Queue_Size] = *event;
    atomic_store_explicit(&queue->written, written + 1, memory_order_release);
    return true;
}

bool t7o_key_queue_take(t7o_key_queue *queue, t7o_key_event *event) {
    unsigned read = atomic_load_explicit(&queue->read, memory_order_relaxed);
    unsigned written = atomic_load_explicit(&queue->written, memory_order_acquire);
    if (read == written)
        return false;
    *event = queue->events[read % T7o_Key_Queue_Size];
    atomic_store_explicit(&queue->read, read + 1, memory_order_release);
    return true;
}

const t7o_keymapping *t7o_keymap_fire(t7o_keymap *map, const t7o_key_event *event, int64_t now_ns) {
    if (map == NULL || event == NULL || elapsed_ms(now_ns, event->time_ns) > T7o_Key_Event_Stale_Ms)
        return NULL;
    const t7o_keymapping *mapping = t7o_keymap_find(map, &event->held, event->key);
    // the debounce counts from the key's release, as T7Overcharged's does
    if (mapping == NULL || !t7o_keymap_debounce_passes(&map->debounce, mapping->debounce_ms, event->time_ns))
        return NULL;
    return mapping;
}

#pragma mark - SendMenuResponse's rules

t7o_number t7o_menuresponse_debounce(int argc, const char *const *argv, int *debounce_ms) {
    if (argc < 4 || argv == NULL) {
        *debounce_ms = T7o_MenuResponse_Debounce_Ms;
        return T7o_Number_Ok;
    }
    return t7o_stoi(argv[3] != NULL ? argv[3] : "", debounce_ms);
}

bool t7o_menuresponse_debounce_passes(t7o_debounce *debounce, int call_debounce_ms, int64_t now_ns) {
    // the static is set up at the first call; unlike customkeymap's, every call puts its own debounce in it
    if (!debounce->started) {
        debounce->started = true;
        debounce->first = true;
        debounce->last_ns = now_ns;
    }
    debounce->debounce_ms = call_debounce_ms;
    if (!debounce->first && elapsed_ms(now_ns, debounce->last_ns) < (int64_t)debounce->debounce_ms)
        return false;
    debounce->last_ns = now_ns;
    debounce->first = false;
    return true;
}

#pragma mark - DB_FreeUnusedResources's rules

t7o_number t7o_free_unused_args(int argc, const char *const *argv, bool *first, bool *second) {
    if (argc < 3 || argv == NULL) {
        *first = false;
        *second = true;
        return T7o_Number_Ok;
    }
    int a = 0, b = 0;
    t7o_number read = t7o_stoi(argv[1] != NULL ? argv[1] : "", &a);
    if (read != T7o_Number_Ok)
        return read;
    read = t7o_stoi(argv[2] != NULL ? argv[2] : "", &b);
    if (read != T7o_Number_Ok)
        return read;
    *first = a != 0;
    *second = b != 0;
    return T7o_Number_Ok;
}

#pragma mark - dvars

// T7Overcharged's patches component (T7Overcharged.dll 0x1800f5a37): sv_maxRate and sv_network_fps lose all their
// flags and get 50000 and 125 (the Mac's defaults are 20000 and 100, its limits 1000 to 50000 and 20 to 200); the
// other three are only set. The limits aren't touched.
const t7o_dvar_change t7o_network_defaults[] = {
    { "sv_maxRate", true, 0, "50000" },
    { "sv_network_fps", true, 0, "125" },
    { "g_allowvote", false, 0, "0" },
    { "sv_mapswitch", false, 0, "0" },
    { "maxvoicepacketsperframe", false, 0, "0" },
};
const size_t t7o_network_default_count = sizeof(t7o_network_defaults) / sizeof(t7o_network_defaults[0]);

// T7Overcharged's dvar_patches component (T7Overcharged.dll 0x18008e35c, scheduled once on its main pipeline 10 s
// after it starts): the flags are replaced, not some bits cleared: 0x1 (only "archive", which nothing in the Mac's game
// saves a dvar by: `dvarlist` and `dvardump` show it), 0 or 0x40. On the Mac as on Windows 0x100 is "cheat
// protected", 0x20 read only, 0x80 write protected and 0x10 latched for a set from the console (the Mac's
// Dvar_SetVariant, 0x100fa0c1d). Three get a value after their flags; T7Overcharged sets com_disable_popups with
// Dvar_SetFromStringByNameFromSource(name, "0", 0, 0), which is what the Mac's Dvar_SetFromStringByName (source 0,
// flags 0) calls. r_glow_allowed isn't in the Mac's game, so it is left out there as T7Overcharged leaves out a dvar it
// doesn't find.
const t7o_dvar_change t7o_dvar_patches[] = {
    { "cg_fovscale", true, 0x1, NULL },
    { "ui_enabled", true, 0x1, NULL },
    { "fx_drawclouds", true, 0x0, NULL },
    { "fx_enable", true, 0x0, NULL },
    { "gpad_stick_deadzone_max", true, 0x1, NULL },
    { "gpad_stick_deadzone_min", true, 0x1, NULL },
    { "r_dof_enable", true, 0x0, NULL },
    { "r_fog", true, 0x0, NULL },
    { "r_glow_allowed", true, 0x0, NULL },
    { "r_lodbiasrigid", true, 0x0, NULL },
    { "r_lodThresholdPixelArea", true, 0x0, NULL },
    { "r_spotLightEntityShadows", true, 0x0, NULL },
    { "r_viewModelSelfShadow", true, 0x0, NULL },
    { "r_viewModelSpotOmniShadowResolution", true, 0x0, NULL },
    { "r_viewModelSunShadowResolution", true, 0x0, NULL },
    { "r_volumetric_lighting_mode", true, 0x0, NULL },
    { "r_zfar", true, 0x0, NULL },
    { "ui_error_callstack_ship", true, 0x0, "1" },
    { "ui_error_report_delay", true, 0x0, "0" },
    { "com_disable_popups", true, 0x0, "0" },
    { "doublejump_enabled", true, 0x40, NULL },
    { "juke_enabled", true, 0x40, NULL },
    { "mantle_enabled", true, 0x40, NULL },
    { "playerEnergy_enabled", true, 0x40, NULL },
    { "sprintLeap_enabled", true, 0x40, NULL },
    { "traverse_mode", true, 0x40, NULL },
    { "trm_enabled", true, 0x40, NULL }, // only the flags: the mantling piece (aae_mantle.c) sets its zombies value
    { "steamLobby_frameEnable", true, 0x0, NULL },
    { "steamP2P_Enable", true, 0x0, NULL },
    { "live_presence_platform", true, 0x0, NULL },
    { "live_enableCounters", true, 0x0, NULL },
    { "lobbyAdvertiseShowInMatchmaking", true, 0x0, NULL },
    { "lobbySearchShowInMatchmaking", true, 0x0, NULL },
    { "groups_enabled", true, 0x0, NULL },
    { "noDW", true, 0x0, NULL },
};
const size_t t7o_dvar_patch_count = sizeof(t7o_dvar_patches) / sizeof(t7o_dvar_patches[0]);

int t7o_session_mode(uint32_t session_mode_state) {
    return (int32_t)(session_mode_state << 28) >> 28;
}

#define Dvar_Flags 0x18          // uint32_t in dvar_t
#define Dvar_Type 0x1c           // uint32_t
#define Dvar_Modes 0x28          // a per-mode dvar's dvar_t * for each mode (the game's 0x100fa35ab reads them)
#define Dvar_Type_PerMode 15
#define Session_Modes 3          // zombies, multiplayer, campaign

uint8_t *t7o_mode_dvar(uint8_t *dvar, int mode) {
    uint32_t type;
    memcpy(&type, dvar + Dvar_Type, sizeof(type));
    if (type != Dvar_Type_PerMode)
        return dvar;
    if (mode < 0 || mode >= Session_Modes)
        return NULL;
    uint8_t *own;
    memcpy(&own, dvar + Dvar_Modes + 8 * (size_t)mode, sizeof(own));
    return own != NULL ? own : dvar;
}

#pragma mark - the game's side

#define ClientUIActive_KeyCatchers 0x4 // int in local client 0's clientUIActive_t (0x1078 bytes each)
#define KeyCatcher_Console 0x1
#define PlayerKeys_Keys 0x138          // KeyState keys[256] in local client 0's PlayerKeyState (0x1940 bytes each)
#define KeyState_Size 0x18             // int down at +0

static bool debug;
static const int *quit_in_progress;        // the game's com_quitInProgress
static const uint8_t *client_ui_actives;   // the game's clientUIActives
static const uint8_t *player_keys;         // the game's playerKeys
static void (*CritSec_ScopedLock)(void *lock, int critical_section, int mode);
static void (*CritSec_ScopedUnlock)(void *lock);
static void *(*Cmd_FindCommand)(const char *name);
static void (*Cmd_AddCommandInternal)(const char *name, void (*function)(void), void *command);

static int64_t now_ns(void) {
    return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// the mappings, made and looked up on the game's main thread (the console's commands, the UI's Lua), under keymap_lock
static t7o_keymap keymap;
static os_unfair_lock keymap_lock = OS_UNFAIR_LOCK_INIT;
static atomic_int keymap_count;            // for the input hook, which has nothing to do while there's no mapping
// the window thread puts the keys that came up in it, the main thread takes them
static t7o_key_queue key_queue;
static atomic_uint key_events_lost;

// a key or mouse event on its way into the game's queue, on the window thread: the key and the keys held, noted
static void note_key_event(int type, int key, int value2) {
    if (!t7o_keymap_event_counts(type, value2, key, *quit_in_progress,
                                 *(const int *)(client_ui_actives + ClientUIActive_KeyCatchers)))
        return;
    t7o_key_event event;
    memset(&event, 0, sizeof(event));
    event.key = key;
    event.time_ns = now_ns();
    for (int k = 0; k < T7o_Keys; k++) {
        if (*(const int *)(player_keys + PlayerKeys_Keys + (size_t)k * KeyState_Size) != 0)
            t7o_keyset_add(&event.held, k);
    }
    if (!t7o_key_queue_put(&key_queue, &event))
        atomic_fetch_add_explicit(&key_events_lost, 1, memory_order_relaxed);
}

// Sys_QueEvent(time, type, value, value2, ...): type 1 is a key, value its number, value2 1 for down and 0 for up.
// The event goes on to the game in any case, as in T7Overcharged
install_hook_name(Sys_QueEvent, void, uint64_t time, int type, int value, int value2, uint64_t length,
                  uint64_t extra, uint64_t pointer) {
    if (atomic_load_explicit(&keymap_count, memory_order_relaxed) > 0)
        note_key_event(type, value, value2);
    orig_Sys_QueEvent(time, type, value, value2, length, extra, pointer);
}

// T7OTakeCustomKey() -> the name of the mapping a key that came up fires, or nil: the UI's timers ask it on the
// game's main thread (t7overcharged.lua), where the game runs the UI's Lua itself and closes and builds it. It also
// applies T7Overcharged's dvar flags once they're due
static void dvar_patches_if_due(int64_t now);

int t7o_small_lua_take_custom_key(lua_State *s) {
    int64_t now = now_ns();
    // the dvar flags, once they're due (T7Overcharged's 10 s), here on the game's main thread
    dvar_patches_if_due(now);
    char name[T7o_Keymap_Name_Max] = "";
    bool fire = false;
    t7o_key_event event;
    while (!fire && t7o_key_queue_take(&key_queue, &event)) {
        os_unfair_lock_lock(&keymap_lock);
        int64_t before = keymap.debounce.last_ns;
        bool was_started = keymap.debounce.started;
        const t7o_keymapping *mapping = t7o_keymap_find(&keymap, &event.held, event.key);
        const t7o_keymapping *fired = t7o_keymap_fire(&keymap, &event, now);
        if (fired != NULL) {
            strcpy(name, fired->name);
            fire = true;
        } else if (debug && mapping != NULL) {
            strcpy(name, mapping->name);
        }
        os_unfair_lock_unlock(&keymap_lock);
        if (fire)
            printf("T7Overcharged: customkeymap: key %d came up, calling %s\n", event.key, name);
        else if (debug && elapsed_ms(now, event.time_ns) > T7o_Key_Event_Stale_Ms)
            printf("T7Overcharged: customkeymap: key %d came up %lld ms ago, too long ago\n", event.key,
                   (long long)elapsed_ms(now, event.time_ns));
        else if (debug && mapping != NULL)
            printf("T7Overcharged: customkeymap: key %d came up, too soon for %s (%lld ms after the last one)\n",
                   event.key, name, was_started ? (long long)elapsed_ms(event.time_ns, before) : -1LL);
        else if (debug)
            printf("T7Overcharged: customkeymap: key %d came up, no mapping for it with the keys held\n", event.key);
    }
    unsigned lost = atomic_exchange_explicit(&key_events_lost, 0, memory_order_relaxed);
    if (lost != 0 && debug)
        printf("T7Overcharged: customkeymap: %u keys came up while nothing took them\n", lost);
    if (fire)
        hks_push_string(s, name);
    else
        hks_push_nil(s);
    return 1;
}

static _Alignas(8) uint8_t customkeymap_command[0x30];

static void customkeymap_f(void) {
    int argc = 0;
    const char *const *argv = t7o_command_args(&argc);
    if (argv == NULL)
        return;
    os_unfair_lock_lock(&keymap_lock);
    t7o_keymap_result result = t7o_keymap_command(&keymap, argc, argv);
    int count = keymap.count;
    os_unfair_lock_unlock(&keymap_lock);
    atomic_store_explicit(&keymap_count, count, memory_order_relaxed);
    switch (result) {
    case T7o_Keymap_Added:
    case T7o_Keymap_Replaced:
        printf("T7Overcharged: customkeymap: key %s calls %s (%s)\n", argv[2], argv[1],
               result == T7o_Keymap_Added ? "new" : "again");
        break;
    case T7o_Keymap_Usage:
        printf("usage: customkeymap functionName keyIndex(s) (debounceTime)\nexample: customkeymap test 168+169\n");
        break;
    case T7o_Keymap_Bad_Number:
        printf("T7Overcharged: customkeymap: a key or the time in \"%s %s\" isn't a number, nothing changed\n",
               argv[2], argc >= 4 ? argv[3] : "");
        break;
    case T7o_Keymap_Bad_Name:
        printf("T7Overcharged: customkeymap: the name doesn't fit in the Lua it goes into, nothing changed\n");
        break;
    case T7o_Keymap_Full:
        printf("T7Overcharged: customkeymap: %d mappings already, nothing changed\n", T7o_Keymap_Max);
        break;
    }
}

// SendMenuResponse: AAE's key binds pass 200 or 50 ms (bind Y "SendMenuResponse StartMenu_Main aae_flashlight 200"),
// so on Windows a bind pressed again, or another one pressed, that soon after the last one is dropped
static t7o_debounce menuresponse_debounce;     // under menuresponse_lock
static os_unfair_lock menuresponse_lock = OS_UNFAIR_LOCK_INIT;
static atomic_bool menuresponse_number_logged;

bool t7o_small_menuresponse_passes(int argc, const char *const *argv) {
    if (argc < 3 || argv == NULL)
        return false;
    int debounce = 0;
    if (t7o_menuresponse_debounce(argc, argv, &debounce) != T7o_Number_Ok) {
        if (!atomic_exchange_explicit(&menuresponse_number_logged, true, memory_order_relaxed))
            printf("T7Overcharged: SendMenuResponse: \"%s\" isn't a time in milliseconds, nothing sent\n", argv[3]);
        return false;
    }
    int64_t now = now_ns();
    os_unfair_lock_lock(&menuresponse_lock);
    int64_t since = menuresponse_debounce.started ? elapsed_ms(now, menuresponse_debounce.last_ns) : -1;
    bool passes = t7o_menuresponse_debounce_passes(&menuresponse_debounce, debounce, now);
    os_unfair_lock_unlock(&menuresponse_lock);
    if (debug) {
        if (passes)
            printf("T7Overcharged: SendMenuResponse %s %s goes on (debounce %d ms, %lld ms after the last one)\n",
                   argv[1], argv[2], debounce, (long long)since);
        else
            printf("T7Overcharged: SendMenuResponse %s %s dropped: %lld ms after the last one, debounce %d ms\n",
                   argv[1], argv[2], (long long)since, debounce);
    }
    return passes;
}

// cursoroverride <file>: All-around Enhancement's conditions.lua runs "cursoroverride 9.ani", or the file its UI colour
// option picks, at every UI start. T7Overcharged (its cursor component) loads that Windows animated cursor from its
// cursors folder (LoadCursorFromFileA), puts it in the two places the window's WM_SETCURSOR takes its cursor from and
// sets it. The Mac's game takes its cursor from the same two places and Aspyr's layer reads .ani files for its own,
// but setting one would mean calling into that layer's cursor objects, from a command the mod runs at every start, for
// a cosmetic option. So the command is here so that the console doesn't say "Unknown command", and does nothing.
static _Alignas(8) uint8_t cursoroverride_command[0x30];
static atomic_bool cursoroverride_logged;

static void cursoroverride_f(void) {
    int argc = 0;
    const char *const *argv = t7o_command_args(&argc);
    if (argv == NULL)
        return;
    if (argc < 2) {
        printf("usage: cursoroverride <cursorfile>\n");
        return;
    }
    if (!atomic_exchange_explicit(&cursoroverride_logged, true, memory_order_relaxed))
        printf("T7Overcharged: cursoroverride %s: the Mac keeps its own mouse cursor\n", argv[1]);
}

// updateVidConfig: AAE's graphics options run it after a change. T7Overcharged's ultrawide component then reads its own
// players\t7.json (vidConfig: allowUltrawide, aspectRatioUI) again for its ultrawide fix of the menus, at the game's
// next video update. That fix isn't on the Mac (its display handling is Aspyr's and BO3MacFix's), so the command is
// there so that the console doesn't say "Unknown command", and does nothing.
static _Alignas(8) uint8_t updatevidconfig_command[0x30];
static atomic_bool updatevidconfig_logged;

static void updatevidconfig_f(void) {
    if (!atomic_exchange_explicit(&updatevidconfig_logged, true, memory_order_relaxed))
        printf("T7Overcharged: updateVidConfig: no ultrawide settings of T7Overcharged's on the Mac, nothing to do\n");
}

// DB_FreeUnusedResources 0 1: AAE's lobby runs it when a match starts from it (lobbyRoot.hideMenusForGameStart), and
// T7Overcharged's command (its experimental component) calls the game's DB_FreeUnusedResources(first, second) with
// it. The Mac's DB_FreeUnusedResources (text "unloaded inuse", the same three callers) takes only the second flag: it
// branches on its one flag where the Windows one branches on its second, and the part the Windows one skips for a
// true first flag (the game always passes false) is always done. So the Mac's gets the second flag.
// The function takes no lock itself: it walks and unlinks the asset hash's chains and publishes a buffer on its own
// stack through globals for the whole walk. The game calls it only with the database's critical sections held: its
// zone load and unload function (0x1013471e5) takes 0x28 (0x10134763b), then 0x60, the asset hash's (0x101347653),
// and releases them the other way round (0x1013478a2, 0x1013478ae); its database shutdown holds 0x60. T7Overcharged
// calls it bare, while AAE starts a match, which is when the database unloads and loads zones; here it takes the
// zone function's two locks in its order first. The game's critical sections are recursive per thread, so a caller
// that holds them already doesn't wait for itself.
#define Critical_Section_Zones 0x28
#define Critical_Section_Assets 0x60
static void (*DB_FreeUnusedResources)(bool second);
static _Alignas(8) uint8_t freeunused_command[0x30];
static atomic_bool freeunused_first_logged;

static void freeunused_f(void) {
    int argc = 0;
    const char *const *argv = t7o_command_args(&argc);
    if (argv == NULL)
        return;
    bool first = false, second = true;
    if (t7o_free_unused_args(argc, argv, &first, &second) != T7o_Number_Ok) {
        printf("T7Overcharged: DB_FreeUnusedResources: \"%s %s\" aren't two numbers, nothing done\n", argv[1], argv[2]);
        return;
    }
    if (first && !atomic_exchange_explicit(&freeunused_first_logged, true, memory_order_relaxed))
        printf("T7Overcharged: DB_FreeUnusedResources: the Mac's has no first flag, it does what it does without\n");
    if (debug)
        printf("T7Overcharged: DB_FreeUnusedResources(%d, %d)\n", first, second);
    uint64_t zones_lock[2] = { 0, 0 }, assets_lock[2] = { 0, 0 };
    CritSec_ScopedLock(zones_lock, Critical_Section_Zones, 0);
    CritSec_ScopedLock(assets_lock, Critical_Section_Assets, 0);
    DB_FreeUnusedResources(second);
    CritSec_ScopedUnlock(assets_lock);
    CritSec_ScopedUnlock(zones_lock);
}

// T7Overcharged's changes when it starts: on Windows its components start once, when the mod's Lua loads it
static void *(*Dvar_FindByHash)(uint32_t hash);
static void (*Dvar_SetFromStringByName)(const char *name, const char *value, bool create_if_missing);
static const uint32_t *session_mode_state; // the game's
static atomic_bool mod_loaded_once;

static void apply_dvar_changes(const char *what, const t7o_dvar_change *changes, size_t count) {
    int mode = t7o_session_mode(*session_mode_state);
    int flags_set = 0;
    for (size_t i = 0; i < count; i++) {
        const t7o_dvar_change *change = &changes[i];
        if (change->set_flags) {
            uint8_t *dvar = Dvar_FindByHash(gsc_hash(change->name));
            uint8_t *target = dvar != NULL ? t7o_mode_dvar(dvar, mode) : NULL;
            if (target != NULL) {
                uint32_t before;
                memcpy(&before, target + Dvar_Flags, sizeof(before));
                memcpy(target + Dvar_Flags, &change->flags, sizeof(change->flags));
                flags_set++;
                if (debug)
                    printf("T7Overcharged: %s: %s%s flags 0x%x -> 0x%x\n", what, change->name,
                           target != dvar ? " (this session mode's)" : "", before, change->flags);
            } else if (debug) {
                printf("T7Overcharged: %s: %s %s\n", what, change->name,
                       dvar == NULL ? "isn't in this game" : "has no dvar for this session mode");
            }
        }
        if (change->value != NULL) {
            Dvar_SetFromStringByName(change->name, change->value, true);
            if (debug)
                printf("T7Overcharged: %s: %s set to %s\n", what, change->name, change->value);
        }
    }
    printf("T7Overcharged: %s: %d dvars' flags set, in session mode %d\n", what, flags_set, mode);
}

// T7Overcharged schedules the dvar flags on its scheduler's main pipeline, 10 s after it starts. Here they're due then,
// and applied by the first look of the UI's timers after that (t7o_small_lua_take_custom_key) or by a later init of
// the stand-in, both on the game's main thread: setting a dvar can print (a dvar the game doesn't have, a value it
// refuses), and the game's printing needs its own threads' thread-local data, which a thread of macOS's doesn't have
// (crash E of crash-sweep.md). 0 until T7Overcharged starts.
static _Atomic int64_t dvar_patches_due_ns;
static atomic_bool dvar_patches_done;

static void dvar_patches_if_due(int64_t now) {
    int64_t due = atomic_load_explicit(&dvar_patches_due_ns, memory_order_relaxed);
    if (due == 0 || now < due || atomic_exchange_explicit(&dvar_patches_done, true, memory_order_relaxed))
        return;
    apply_dvar_changes("dvar flags", t7o_dvar_patches, t7o_dvar_patch_count);
}

void t7o_small_mod_loaded(void) {
    // nothing before t7o_small_install
    if (session_mode_state == NULL)
        return;
    int64_t now = now_ns();
    // the first time only: the network defaults now, the dvar flags due in 10 s
    if (!atomic_exchange_explicit(&mod_loaded_once, true, memory_order_relaxed)) {
        apply_dvar_changes("network defaults", t7o_network_defaults, t7o_network_default_count);
        int64_t due = now + (int64_t)T7o_Dvar_Patch_Delay_S * 1000000000;
        atomic_store_explicit(&dvar_patches_due_ns, due, memory_order_relaxed);
    }
    dvar_patches_if_due(now);
}

void t7o_small_install(uint64_t base_address) {
    const char *debug_env = getenv("BO3MACFIX_T7ODEBUG");
    debug = debug_env != NULL && atoi(debug_env) != 0;
    quit_in_progress = (const int *)(base_address + ADDR_com_quitInProgress);
    // the game's clientUIActives (0x1078 bytes each): the key catchers at +4, the connection state at +8
    client_ui_actives = (const uint8_t *)(base_address + ADDR_CLIENT_CONNECTIONS);
    player_keys = (const uint8_t *)(base_address + ADDR_playerKeys);
    CritSec_ScopedLock = (void *)(base_address + ADDR_CritSec_ScopedLock);
    CritSec_ScopedUnlock = (void *)(base_address + ADDR_CritSec_ScopedUnlock);
    DB_FreeUnusedResources = (void *)(base_address + ADDR_DB_FreeUnusedResources);
    Dvar_FindByHash = (void *)(base_address + ADDR_Dvar_FindByHash);
    Dvar_SetFromStringByName = (void *)(base_address + ADDR_Dvar_SetFromStringByName);
    session_mode_state = (const uint32_t *)(base_address + ADDR_sSessionModeState);
    Cmd_FindCommand = (void *)(base_address + ADDR_Cmd_FindCommand);
    Cmd_AddCommandInternal = (void *)(base_address + ADDR_Cmd_AddCommandInternal);
    if (Cmd_FindCommand("customkeymap") == NULL)
        Cmd_AddCommandInternal("customkeymap", customkeymap_f, customkeymap_command);
    if (Cmd_FindCommand("cursoroverride") == NULL)
        Cmd_AddCommandInternal("cursoroverride", cursoroverride_f, cursoroverride_command);
    if (Cmd_FindCommand("updateVidConfig") == NULL)
        Cmd_AddCommandInternal("updateVidConfig", updatevidconfig_f, updatevidconfig_command);
    if (Cmd_FindCommand("DB_FreeUnusedResources") == NULL)
        Cmd_AddCommandInternal("DB_FreeUnusedResources", freeunused_f, freeunused_command);
    install_hook_Sys_QueEvent((void *)(base_address + ADDR_Sys_QueEvent));
}
