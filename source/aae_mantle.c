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
// The mantle prompt is the plugin's mantle_hint component, see "the mantle prompt" below.
//
// Not ported: the plugin's rule for a first trace that hits a vehicle (it asks the game whether the player may use
// it).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

#define Hint_Fresh_Ms 20 // how long a probe's find counts

bool aae_mantle_probe_result(bool found, bool was_found, uint64_t now, uint64_t last_found_at) {
    if (found)
        return true;
    // a miss right after a find: several moves are made within a frame, and the prompt shouldn't flicker with them.
    // (A find later than now is as old as can be: the difference wraps around.)
    return was_found && last_found_at != 0 && now - last_found_at <= Hint_Fresh_Ms;
}

int aae_mantle_hint_state(bool traversing, const bool found[2], const uint64_t found_at[2], uint64_t now) {
    if (traversing)
        return Aae_Hint_Hide;
    for (int handler = 0; handler < 2; handler++)
        if (found[handler] && found_at[handler] != 0 && now - found_at[handler] <= Hint_Fresh_Ms)
            return Aae_Hint_Show;
    return Aae_Hint_Unchanged;
}

// ---- the game's side

static uint64_t base_address;
static void *(*Dvar_FindByHash)(uint32_t hash);
static float (*Dvar_GetFloat)(void *dvar); // a dvar that a "set" made is a string, read as atof of its text
static const char *loaded_mod; // the game's: the folder name of the mod that's loaded, "" without one
static _Atomic bool active;    // that mod has the plugin whose work this is: found out at each map's start
static int debug;              // BO3MACFIX_MANTLEDEBUG=1: the log says what each scan found

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

// ---- the mantle prompt (the plugin's mantle_hint component)
//
// The game's prompt for mantling ("Press [jump] to" and an arrow) goes by a flag that only its old way of mantling
// sets, so traversal has none. The plugin gives it one. At every move of a player who isn't in a jump it lets the
// scan run anyway, as a probe that starts nothing: the test before the scan is answered with yes, and the scan's
// answer is kept and replaced by no. A probe's first trace is done 30 units up, about where a jump takes the player,
// so a ledge lower than that gets no prompt. While the last probe found a ledge the prompt is shown, and during a
// traversal it's hidden. A probe takes about 4 microseconds (10,000 of them 41 ms, at one or two a frame).
//
// The plugin only probes in a LAN game: it looks at the session's network mode as the game stores it, where a game
// from the online lobby is "online" (the game's own question for it would say "system link" for any game with a
// mod). So in a game started from the online lobby there is no prompt, with the plugin as here.
//
// The plugin also takes out a check of the game's that leaves the prompt away in zombies when its icon isn't loaded.
// With the mod loaded the icon is there (on Kino as on a custom map), so that isn't done here.
#define Hint_Clients 64
#define Hint_Rise 30.0f     // how far above the player's feet a probe's first trace is done
#define Hint_Settle_Ms 2000 // no probes for this long after a player's first move
#define Ps_Flags 0x10
#define Ps_Flag_Traversing 0x200
#define Pm_Handler 0x2ac          // pmove_t: 0 the server's move, 1 the client's prediction
#define Mode_Network_Mask 0x3c0 // of the session mode's bits: how the game is networked (0 offline, 2 online)
#define Mode_Network_Lan 0x40   // 1, "system link": a game from the menus' "play local", or one started by devmap
#define Cg_Size 0x342a80
#define Cg_PlayerState 0x11a8d0
#define Cg_MantleFlags 0x11b0b4 // the predicted player state's mantle flags
#define Mantle_Flag_Prompt 0x10

static struct {
    _Atomic uint64_t seen_at;     // the player's first move, in ms
    _Atomic uint64_t found_at[2]; // by handler: the last probe that found a ledge
    _Atomic bool found[2];
    _Atomic bool traversing;
} hints[Hint_Clients];
static _Atomic uint32_t hint_height; // the height the finds were made with (its bits)

static _Thread_local struct {
    bool wanted;  // the test before the scan asked for a probe: the scan that follows is one
    bool running; // inside a probe's scan
    uint32_t client, handler;
} probe;

static int (*Dvar_GetInt)(void *dvar);
static const uint32_t *session_mode; // the game's
static uint8_t *const *cg_clients;   // the game's: the local clients' cg_t, one after the other
static const int *cg_client_count;
static uint64_t (*now_ms)(void);

static uint64_t uptime_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}

static void forget_finds(uint32_t client) {
    for (int handler = 0; handler < 2; handler++) {
        atomic_store(&hints[client].found[handler], false);
        atomic_store(&hints[client].found_at[handler], 0);
    }
}

// a new mantle height makes what the probes found worthless
static void finds_for_height(float height) {
    uint32_t bits;
    memcpy(&bits, &height, sizeof(bits));
    if (atomic_exchange(&hint_height, bits) != bits)
        for (uint32_t client = 0; client < Hint_Clients; client++)
            forget_finds(client);
}

static bool traversal_mode(void) {
    static void *dvar; // found once: a dvar stays where it is
    void *mode = __atomic_load_n(&dvar, __ATOMIC_RELAXED);
    if (mode == NULL) {
        mode = Dvar_FindByHash(gsc_hash("traverse_mode"));
        if (mode == NULL)
            return false;
        __atomic_store_n(&dvar, mode, __ATOMIC_RELAXED);
    }
    return Dvar_GetInt(mode) == 1;
}

// at every move, before the scan: keeps up with whether the player is in a traversal, and says whether a probe may
// be made now if the game's own test before the scan says no (the player isn't in a jump)
static bool probe_allowed(uint8_t *pm) {
    if ((*session_mode & Mode_Network_Mask) != Mode_Network_Lan || !traversal_mode() || pm[Pm_Handler] > 1)
        return false;
    const uint8_t *ps = *(uint8_t **)pm;
    uint32_t client;
    uint64_t flags;
    memcpy(&client, ps, sizeof(client));
    memcpy(&flags, ps + Ps_Flags, sizeof(flags));
    if (client >= Hint_Clients)
        return false;
    bool traversing = (flags & Ps_Flag_Traversing) != 0;
    atomic_store(&hints[client].traversing, traversing);
    if (traversing) {
        forget_finds(client);
        return false;
    }
    uint64_t now = now_ms(), seen = 0;
    if (atomic_compare_exchange_strong(&hints[client].seen_at, &seen, now))
        return false; // the first move
    if (now < seen || now - seen < Hint_Settle_Ms)
        return false;
    probe.client = client;
    probe.handler = pm[Pm_Handler];
    return true;
}

static void probe_found(bool found) {
    uint64_t now = now_ms();
    bool counts = aae_mantle_probe_result(found, atomic_load(&hints[probe.client].found[probe.handler]), now,
                                          atomic_load(&hints[probe.client].found_at[probe.handler]));
    if (found)
        atomic_store(&hints[probe.client].found_at[probe.handler], now);
    atomic_store(&hints[probe.client].found[probe.handler], counts);
}

// CG_DrawMantleHint(local client, the HUD's element, ...): draws the prompts for mantling, for a hatch and for
// mounting, by the predicted player state's mantle flags
install_hook_name(CG_DrawMantleHint, void, int local_client, void *element, void *a, void *b) {
    float height;
    uint8_t *clients = *cg_clients;
    if (clients == NULL || local_client < 0 || local_client > 1 || local_client >= *cg_client_count ||
        !mantle_height(&height) || !traversal_mode()) {
        orig_CG_DrawMantleHint(local_client, element, a, b);
        return;
    }
    uint8_t *cg = clients + (uint64_t)local_client * Cg_Size;
    uint32_t client, flags;
    uint64_t ps_flags;
    memcpy(&client, cg, sizeof(client));
    memcpy(&ps_flags, cg + Cg_PlayerState + Ps_Flags, sizeof(ps_flags));
    int state = Aae_Hint_Unchanged;
    if (client < Hint_Clients) {
        finds_for_height(height);
        bool found[2] = { atomic_load(&hints[client].found[0]), atomic_load(&hints[client].found[1]) };
        uint64_t found_at[2] = { atomic_load(&hints[client].found_at[0]), atomic_load(&hints[client].found_at[1]) };
        state = aae_mantle_hint_state(atomic_load(&hints[client].traversing) || (ps_flags & Ps_Flag_Traversing) != 0,
                                      found, found_at, now_ms());
    }
    memcpy(&flags, cg + Cg_MantleFlags, sizeof(flags));
    uint32_t shown = state == Aae_Hint_Show   ? flags | Mantle_Flag_Prompt
                     : state == Aae_Hint_Hide ? flags & ~(uint32_t)Mantle_Flag_Prompt
                                              : flags;
    if (debug) {
        static int said[2];
        if (said[local_client] != state) {
            said[local_client] = state;
            printf("mantle: the prompt is %s\n", state == Aae_Hint_Show ? "shown: a ledge is there" :
                   state == Aae_Hint_Hide ? "hidden for a traversal" : "the game's own again");
        }
    }
    memcpy(cg + Cg_MantleFlags, &shown, sizeof(shown));
    orig_CG_DrawMantleHint(local_client, element, a, b);
    memcpy(cg + Cg_MantleFlags, &flags, sizeof(flags));
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
    if (scan.first_trace && probe.running) {
        // the scan's own two points: it goes on from them to where the trace hit
        ((float *)start)[2] += Hint_Rise;
        ((float *)end)[2] += Hint_Rise;
    }
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
    if (!mantle_height(&height)) {
        if (probe.wanted) { // the mod went between the test and the scan: this one was never to start anything
            probe.wanted = false;
            return false;
        }
        return orig_TRM_Scan(pm, pml, ledge, normal);
    }
    scan.ps = *(uint8_t **)pm;
    signed char forward = (signed char)pm[Pm_Forwardmove], right = (signed char)pm[Pm_Forwardmove + 1];
    signed char shown_forward = forward, shown_right = right;
    aae_mantle_scan_input(move_threshold(), &shown_forward, &shown_right);
    pm[Pm_Forwardmove] = (uint8_t)shown_forward;
    pm[Pm_Forwardmove + 1] = (uint8_t)shown_right;
    scan.first_trace = true;
    probe.running = probe.wanted;
    probe.wanted = false;
    uint64_t started = debug && probe.running ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
    bool found = orig_TRM_Scan(pm, pml, ledge, normal);
    scan.first_trace = false;
    pm[Pm_Forwardmove] = (uint8_t)forward;
    pm[Pm_Forwardmove + 1] = (uint8_t)right;
    if (probe.running) {
        probe.running = false;
        probe_found(found);
        if (debug) {
            // what the prompt costs: the probes are the only work it adds to a move
            static _Atomic unsigned probes, finds;
            static _Atomic uint64_t spent;
            uint64_t took = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started;
            uint64_t in_all = atomic_fetch_add(&spent, took) + took;
            unsigned count = atomic_fetch_add(&probes, 1) + 1, with_ledge = atomic_fetch_add(&finds, found) + found;
            if (count % 2000 == 0)
                printf("mantle: %u probes for the prompt so far, %u found a ledge, %.1f ms in all\n", count,
                       with_ledge, (double)in_all / 1e6);
        }
        return false;
    }
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
    finds_for_height(height);
    probe.wanted = probe_allowed(pm) && !can;
    return can || probe.wanted; // for a probe the game goes on to the scan as well
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
    debug = env != NULL ? atoi(env) : 0;
    Dvar_FindByHash = (void *)(base_address + ADDR_Dvar_FindByHash);
    Dvar_GetFloat = (void *)(base_address + ADDR_Dvar_GetFloat);
    Dvar_GetInt = (void *)(base_address + ADDR_Dvar_GetInt);
    loaded_mod = (const char *)(base_address + ADDR_Mods_LoadedName);
    session_mode = (const uint32_t *)(base_address + ADDR_sSessionModeState);
    cg_clients = (uint8_t *const *)(base_address + ADDR_cgArray);
    cg_client_count = (const int *)(base_address + ADDR_LOCAL_CLIENT_COUNT);
    now_ms = uptime_ms;
    install_hook_CG_DrawMantleHint((void *)(base_address + ADDR_CG_DrawMantleHint));
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
