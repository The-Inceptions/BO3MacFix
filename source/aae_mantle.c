// AAEMantling: mantling in zombies as All-around Enhancement 4 sets it up, which on Windows its plugin does (the
// t7_traversal component).
//
// The game has two ways over an obstacle. The old one, mantling, works where the map's maker put a mantle brush. The
// new one from multiplayer and the campaign, traversal, finds ledges by itself: while the player jumps and pushes
// towards something, it traces forward and upward for a ledge within the game type's trm_maxHeight. The dvar
// traverse_mode picks one of them, and zombies has the old one.
//
// Since 4.0 the mod sets traverse_mode to traversal on most maps and gives its rules in dvars of its own, which only
// its plugin reads. Without the plugin the game's own rules stay, and those leave nothing: zombies' trm_maxHeight is 0,
// so no ledge is ever low enough, and with the old way switched off the player can't get over anything, mantle brushes
// included. The plugin's rules, as read from it:
//   aae_t7_mantle_height          the highest ledge, in place of trm_maxHeight (the mod sets 60, or 35 or 99 by map)
//   aae_t7_ignore_mantle_checks   0: only where the first trace hits a mantle brush, the places the old way worked;
//                                 1: anywhere (the game's own rule is the opposite of 0: anywhere but mantle brushes)
//   aae_t7_move_input_threshold   how much move input counts as pushing, in place of the game's 50 (the mod sets 0:
//                                 any input, and none is needed to start)
//   aae_t7_high_height_offset     added to the heights 96, 200 and 350 at which the kind of traversal changes
// A mantle brush marked for mantling over and mounting also sets the traversal's "over" flag.
//
// The plugin patches instructions inside the game's traversal scan. Here the functions around those places are hooked
// instead: the game type setting's getter (for trm_maxHeight), the scan and the test before it (the move input they
// see is scaled so that the game's own "more than 50" comes out as the mod's rule), the player trace (the scan's first
// one: its contents are shown to the game as its own test needs them), and the choice of the kind of traversal.
// Everything passes through untouched unless the mod that's loaded has the plugin (looked at when a map starts) and
// has set aae_t7_mantle_height.
//
// The choice of the kind of traversal is only hooked when the game runs from the pre-patched executable (see
// aae_mantle_init); it matters for ledges from 96 units up, and the mod allows 99 on one custom map.
//
// And one thing the plugin does as soon as it's loaded: it turns traversal on for zombies at all (trm_enabled, see
// traversal_for_zombies). Here that happens at each map's start while a mod with the plugin is loaded, and is taken
// back at the first map's start without one.
//
// Not ported: the plugin's rule for a first trace that hits a vehicle (it asks the game whether the player may use
// it).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aae_mantle.h"
#include "dobby.h"
#include "gsc_obj.h"
#include "offsets.h"
#include "prepatch.h"
#include "t7overcharged.h"

#define Pm_Forwardmove 0x40    // pmove_t: the command's forwardmove, a signed char, and rightmove after it
#define Ps_Origin_Z 0x38       // playerState_s
#define Ps_TraversalFlags 0x7f4
#define Trace_Fraction 0x10
#define Trace_SurfaceFlags 0x14
#define Trace_Contents 0x18
#define Trace_AllSolid 0x38
#define Trace_StartSolid 0x39
#define Contents_Mantle 0x1000000
#define Surface_OverMask 0x38000000 // mantle on, mantle over, mount
#define Surface_Over 0x18000000     // mantle over and mount
#define Traversal_Over 1
#define Game_Push 50 // the game's "pushing": forwardmove and rightmove together above this

typedef struct {
    float x, y, z;
} vec3_t;

void aae_mantle_scan_input(int threshold, signed char *forward, signed char *right) {
    int f = *forward, r = *right, size = abs(f) + abs(r);
    bool pushing = size != 0 && size > threshold;
    if (pushing == (size > Game_Push))
        return;
    if (pushing) {
        // up, until the game takes it for pushing. The size is 1 to 50 here, so neither gets beyond 100
        int scale = Game_Push / size + 1;
        f *= scale, r *= scale;
    } else {
        int divide = (size + Game_Push - 1) / Game_Push;
        f /= divide, r /= divide;
    }
    *forward = (signed char)f;
    *right = (signed char)r;
}

void aae_mantle_start_input(int threshold, signed char *forward, signed char *right) {
    int size = abs(*forward) + abs(*right);
    bool allowed = threshold <= 0 || size > threshold;
    if (allowed == (size > Game_Push))
        return;
    // only the size is looked at there
    *forward = allowed ? 127 : 0;
    *right = 0;
}

uint32_t aae_mantle_first_hit(bool ignore_checks, uint32_t contents, uint32_t surface_flags, uint32_t *traversal_flags) {
    if ((surface_flags & Surface_OverMask) == Surface_Over)
        *traversal_flags |= Traversal_Over;
    bool look_for_ledge = ignore_checks || (contents & Contents_Mantle) != 0;
    // the game goes on when the hit isn't a mantle brush
    return look_for_ledge ? contents & ~(uint32_t)Contents_Mantle : contents | Contents_Mantle;
}

#define High_Step_1 96.0f
#define High_Step_2 200.0f
#define High_Step_3 350.0f

int aae_mantle_high_type(float height, float offset, uint32_t traversal_flags) {
    if (height < High_Step_1 + offset)
        return -1;
    if ((traversal_flags & Traversal_Over) != 0)
        return height < High_Step_2 + offset ? 0x11 : 0x12 + (height >= High_Step_3 + offset);
    return 0x14 + (height >= High_Step_3 + offset);
}

float aae_mantle_low_height(float height) {
    return height < High_Step_1 ? height : High_Step_1 - 0.125f;
}

// ---- the game's side

static uint64_t base_address;
static void *(*Dvar_FindByHash)(uint32_t hash);
static float (*Dvar_GetFloat)(void *dvar); // a dvar that a "set" made is a string, read as atof of its text
static const char *loaded_mod; // the game's: the folder name of the mod that's loaded, "" without one
static _Atomic bool active;    // that mod has the plugin whose work this is: found out at each map's start
static bool debug;             // BO3MACFIX_MANTLEDEBUG=1: the log says what each scan found

enum { Opt_Height, Opt_IgnoreChecks, Opt_Threshold, Opt_HighOffset, Opt_Count };
static const char *const option_names[Opt_Count] = { "aae_t7_mantle_height", "aae_t7_ignore_mantle_checks",
                                                     "aae_t7_move_input_threshold", "aae_t7_high_height_offset" };
static void *option_dvars[Opt_Count]; // found once each: a dvar stays where it is

// the mod's dvar, or missing when nothing has set it (yet)
static bool option(int which, float *value) {
    void *dvar = __atomic_load_n(&option_dvars[which], __ATOMIC_RELAXED);
    if (dvar == NULL) {
        dvar = Dvar_FindByHash(gsc_hash(option_names[which]));
        if (dvar == NULL)
            return false;
        __atomic_store_n(&option_dvars[which], dvar, __ATOMIC_RELAXED);
    }
    *value = Dvar_GetFloat(dvar);
    return true;
}

// the plugin's limits and what it takes for a value outside them. Every hook asks for the height first: without
// it, or without the plugin's mod, the game's own rules stay.
static bool mantle_height(float *height) {
    if (!atomic_load_explicit(&active, memory_order_relaxed) || !option(Opt_Height, height))
        return false;
    if (!(*height >= 0.0f && *height <= 4096.0f))
        *height = 57.0f;
    return true;
}
static int move_threshold(void) {
    float value = 0.0f;
    option(Opt_Threshold, &value);
    return value < -1.0f ? -1 : value > 256.0f ? 0 : (int)value;
}
static float high_offset(void) {
    float value = 0.0f;
    option(Opt_HighOffset, &value);
    return value < -45.0f ? -45.0f : value > 512.0f ? 512.0f : value;
}
static bool ignore_checks(void) {
    float value = 0.0f;
    return option(Opt_IgnoreChecks, &value) && value != 0.0f;
}

// GameTypeSettings_GetFloat(name, which): a setting of the game type's. Only the scan asks for trm_maxHeight, which
// is 0 in zombies.
install_hook_name(GameTypeSettings_GetFloat, float, const char *name, bool which) {
    float height;
    if (name != NULL && strcmp(name, "trm_maxHeight") == 0 && mantle_height(&height))
        return height;
    return orig_GameTypeSettings_GetFloat(name, which);
}

// a scan on this thread, for the player trace's hook: the scan's first trace is the one whose hit decides
static _Thread_local struct {
    bool first_trace;
    uint8_t *ps;
} scan;

// PM_PlayerTrace(pm, results, start, mins, maxs, end, entity to pass through, contents to hit)
install_hook_name(PM_PlayerTrace, void, uint8_t *pm, uint8_t *results, const float *start, const float *mins,
                  const float *maxs, const float *end, int pass_entity, int contents_mask) {
    orig_PM_PlayerTrace(pm, results, start, mins, maxs, end, pass_entity, contents_mask);
    if (!scan.first_trace)
        return;
    scan.first_trace = false;
    float fraction;
    memcpy(&fraction, results + Trace_Fraction, sizeof(fraction));
    if (results[Trace_AllSolid] != 0 || results[Trace_StartSolid] != 0 || fraction == 1.0f)
        return; // nothing hit, or from inside something: the game stops here by itself
    uint32_t contents, surface_flags, flags;
    memcpy(&contents, results + Trace_Contents, sizeof(contents));
    memcpy(&surface_flags, results + Trace_SurfaceFlags, sizeof(surface_flags));
    memcpy(&flags, scan.ps + Ps_TraversalFlags, sizeof(flags));
    uint32_t shown = aae_mantle_first_hit(ignore_checks(), contents, surface_flags, &flags);
    memcpy(results + Trace_Contents, &shown, sizeof(shown));
    memcpy(scan.ps + Ps_TraversalFlags, &flags, sizeof(flags));
}

// TRM_Scan(pm, pml, ledge out, normal out): looks for a ledge in front of the player to get on or over
install_hook_name(TRM_Scan, bool, uint8_t *pm, void *pml, vec3_t *ledge, vec3_t *normal) {
    float height;
    if (!mantle_height(&height))
        return orig_TRM_Scan(pm, pml, ledge, normal);
    scan.ps = *(uint8_t **)pm;
    signed char forward = (signed char)pm[Pm_Forwardmove], right = (signed char)pm[Pm_Forwardmove + 1];
    signed char shown_forward = forward, shown_right = right;
    aae_mantle_scan_input(move_threshold(), &shown_forward, &shown_right);
    pm[Pm_Forwardmove] = (uint8_t)shown_forward;
    pm[Pm_Forwardmove + 1] = (uint8_t)shown_right;
    scan.first_trace = true;
    bool found = orig_TRM_Scan(pm, pml, ledge, normal);
    scan.first_trace = false;
    pm[Pm_Forwardmove] = (uint8_t)forward;
    pm[Pm_Forwardmove + 1] = (uint8_t)right;
    if (debug && found) {
        float feet;
        memcpy(&feet, scan.ps + Ps_Origin_Z, sizeof(feet));
        printf("mantle: a ledge %.1f above the player's feet (the mod allows %.0f)\n", ledge->z - feet, height);
    }
    return found;
}

// TRM_CanStart(pm, pml): whether the player is in a state to start a traversal, pushing with the move input for one
install_hook_name(TRM_CanStart, bool, uint8_t *pm, void *pml) {
    float height;
    if (!mantle_height(&height))
        return orig_TRM_CanStart(pm, pml);
    signed char forward = (signed char)pm[Pm_Forwardmove], right = (signed char)pm[Pm_Forwardmove + 1];
    signed char shown_forward = forward, shown_right = right;
    aae_mantle_start_input(move_threshold(), &shown_forward, &shown_right);
    pm[Pm_Forwardmove] = (uint8_t)shown_forward;
    pm[Pm_Forwardmove + 1] = (uint8_t)shown_right;
    bool can = orig_TRM_CanStart(pm, pml);
    pm[Pm_Forwardmove] = (uint8_t)forward;
    pm[Pm_Forwardmove + 1] = (uint8_t)right;
    return can;
}

// TRM_Type(ps, ledge): which kind of traversal (animation) takes the player up to this ledge
install_hook_name(TRM_Type, int, uint8_t *ps, vec3_t ledge) {
    float height;
    if (!mantle_height(&height))
        return orig_TRM_Type(ps, ledge);
    float feet;
    uint32_t flags;
    memcpy(&feet, ps + Ps_Origin_Z, sizeof(feet));
    memcpy(&flags, ps + Ps_TraversalFlags, sizeof(flags));
    int type = aae_mantle_high_type(ledge.z - feet, high_offset(), flags);
    if (type < 0) {
        ledge.z = feet + aae_mantle_low_height(ledge.z - feet);
        type = orig_TRM_Type(ps, ledge);
    }
    if (debug)
        printf("mantle: traversal %d\n", type);
    return type;
}

// A dvar with a value for each mode is one dvar per mode behind a parent: the parent's +0x28 is zombies', +0x30
// multiplayer's and +0x38 the campaign's. A dvar's value is at +0x28, with the latched one and the default after it.
#define Dvar_Type 0x1c
#define Dvar_Type_Bool 1
#define Dvar_Type_PerMode 0xf
#define Dvar_Modes 0x28
#define Dvar_Value 0x28
#define Dvar_Latched 0x40
#define Dvar_Default 0x58
#define Plugin_Path "T7Overcharged/plugins/AAEUnified.dll" // in the mod's folder: T7Overcharged loads it on Windows
#define Mod_Name_Size 0x20 // the game's array for the loaded mod's name

static int dvar_type(const uint8_t *dvar) {
    int type;
    memcpy(&type, dvar + Dvar_Type, sizeof(type));
    return type;
}

// trm_enabled says whether a mode has traversal at all: on in multiplayer and the campaign, off in zombies. The mod's
// plugin gives zombies multiplayer's default as soon as it's loaded. Without that the game doesn't make the
// traversals' animations at a map's start, and crashes on the missing ones at the first traversal. Without the mod
// zombies gets its own value back: the game stops a map's start with an error for each of those animations it
// doesn't find.
static struct {
    pthread_mutex_t lock; // a map starts on the server's thread and on the client's
    bool changed;
    uint8_t value, latched, reset;
} zombies_own = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void set_traversal_for_zombies(bool on) {
    uint8_t *dvar = Dvar_FindByHash(gsc_hash("trm_enabled"));
    if (dvar == NULL || dvar_type(dvar) != Dvar_Type_PerMode)
        return;
    uint8_t *modes[3];
    memcpy(modes, dvar + Dvar_Modes, sizeof(modes));
    uint8_t *zombies = modes[0], *other = modes[1] != NULL ? modes[1] : modes[2];
    if (zombies == NULL || other == NULL || dvar_type(zombies) != Dvar_Type_Bool || dvar_type(other) != Dvar_Type_Bool)
        return;
    if (on) {
        if (!zombies_own.changed) {
            zombies_own.value = zombies[Dvar_Value];
            zombies_own.latched = zombies[Dvar_Latched];
            zombies_own.reset = zombies[Dvar_Default];
            zombies_own.changed = true;
            printf("BO3MacFix: traversal is on for zombies now (trm_enabled), as All-around Enhancement's plugin sets "
                   "it (AAEMantling)\n");
        }
        zombies[Dvar_Value] = zombies[Dvar_Latched] = zombies[Dvar_Default] = other[Dvar_Default];
    } else if (zombies_own.changed) {
        zombies[Dvar_Value] = zombies_own.value;
        zombies[Dvar_Latched] = zombies_own.latched;
        zombies[Dvar_Default] = zombies_own.reset;
        zombies_own.changed = false;
        printf("BO3MacFix: the mod with the plugin is gone, traversal for zombies is the game's own again\n");
    }
}

static void traversal_for_zombies(bool on) {
    pthread_mutex_lock(&zombies_own.lock);
    set_traversal_for_zombies(on);
    pthread_mutex_unlock(&zombies_own.lock);
}

// whether the mod that's loaded has the plugin whose work this is
static bool mod_has_plugin(void) {
    char path[4096 + sizeof(Plugin_Path) + 1], loaded[Mod_Name_Size + 1];
    const char *folder = t7o_mod_folder(), *name = strrchr(folder, '/');
    name = name != NULL ? name + 1 : folder;
    memcpy(loaded, loaded_mod, Mod_Name_Size);
    loaded[Mod_Name_Size] = '\0';
    // the folder T7Overcharged was last asked for from has to be the loaded mod's, not one from earlier on
    if (loaded[0] == '\0' || strcmp(name, loaded) != 0)
        return false;
    snprintf(path, sizeof(path), "%s/%s", folder, Plugin_Path);
    return access(path, F_OK) == 0;
}

// TRM_Init(alloc): at a map's start, on the server and on the client, makes the traversals' animations if the mode
// has traversal
install_hook_name(TRM_Init, void, void *alloc) {
    bool on = mod_has_plugin();
    atomic_store_explicit(&active, on, memory_order_relaxed);
    traversal_for_zombies(on);
    orig_TRM_Init(alloc);
}

void aae_mantle_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    const char *env = getenv("BO3MACFIX_MANTLEDEBUG");
    debug = env != NULL && atoi(env) != 0;
    Dvar_FindByHash = (void *)(base_address + ADDR_Dvar_FindByHash);
    Dvar_GetFloat = (void *)(base_address + ADDR_Dvar_GetFloat);
    loaded_mod = (const char *)(base_address + ADDR_Mods_LoadedName);
    install_hook_GameTypeSettings_GetFloat((void *)(base_address + ADDR_GameTypeSettings_GetFloat));
    install_hook_PM_PlayerTrace((void *)(base_address + ADDR_PM_PlayerTrace));
    install_hook_TRM_Scan((void *)(base_address + ADDR_TRM_Scan));
    install_hook_TRM_CanStart((void *)(base_address + ADDR_TRM_CanStart));
    install_hook_TRM_Init((void *)(base_address + ADDR_TRM_Init));
    // TRM_Type starts with an SSE instruction that the hooking library can't move out of the way (it crashes on
    // it), so that one only exists in the pre-patched executable, whose stubs are made here
    if (prepatch_has_hook(ADDR_TRM_Type))
        install_hook_TRM_Type((void *)(base_address + ADDR_TRM_Type));
    else
        printf("mantle: not running from the pre-patched executable: the kind of traversal for ledges from 96 units "
               "up stays the game's own choice\n");
}
