// T7Overcharged's movement changes (AAEMovement): the switches, as T7Overcharged's movement component keeps them.
//
// All-around Enhancement loads its own build of T7Overcharged, and that changes how the player moves: the pieces in
// aae_move_sprint.c, aae_move_jump.c, aae_move_fastjump.c and aae_move_slide.c. Each of them asks here, every time
// it could act, whether its change is on. The answers are T7Overcharged's:
//
// - Nothing is on until the mod has loaded T7Overcharged. Its movement component starts then, once per launch (its
//   components' starts are guarded; the mod's loader runs again at every map, and that doesn't start them again),
//   and only when the dvar hook_movement_perks is 1 at that moment (the mod sets it to 1 just before; registered by
//   T7Overcharged as an int from 0 to 1 with 1 as its default, so a game where nobody set it counts as 1).
// - At its start it registers its other three dvars: t7o_perk_fastads_multiplier (a float, 2.15, from 1 to 3),
//   t7o_strafe_speed_scale (a float, 0.667, from 0.667 to 0.97) and t7o_omnimovement (a bool, off). They are
//   registered here the same way, through the game's own Dvar_Register functions, so the game treats a value the mod
//   set before (it sets the fast ADS multiplier to 1.4 before loading T7Overcharged: the game keeps such a value as
//   text and turns it into a number inside the domain when the dvar is registered) and after (a value outside the
//   domain is refused) as it does on Windows.
// - Its console commands switch eleven patches: "movement_patch <name> <0/1>" and "movement_patch_all <0/1>". All
//   are off at its start. The mod's loader then runs "movement_patch_all 1", "movement_patch jumpslowdown 1" and
//   "movement_patch fastswap 0", after setting t7o_omnimovement 1 and t7o_strafe_speed_scale 0.9: in an ordinary game
//   everything is on but fastswap. A map can turn them off again (the mod's HUD runs "movement_patch_all 0" when the
//   server's no_movement_patch is "1"; one of its options runs "movement_patch jumpslowdown 0").
// - Omni-movement and the strafe scale follow their dvars as they are at each moment. The fast ADS multiplier is the
//   dvar's value at the moment fastads was switched on: T7Overcharged copies it into the game's code then, every
//   time it is switched on (also when it was on already), and later changes of the dvar do nothing until then.
//
// On Windows the switches stay as they are when the mod is unloaded (T7Overcharged stays in the game until it
// quits); here every answer is also "off" while the loaded mod isn't All-around Enhancement with its plugin, as for
// every other piece of the mod's Windows code. The commands are there from the start, whatever the setting says, so
// that the mod's lines never say "Unknown command"; on Windows they come with the component's start. They do nothing
// before that start and with AAEMovement at 0.
//
// The movement code asks on two threads at once (the server's and the client's prediction), many times a frame,
// while a command may be switching: the answers are atomics, and nothing on that side takes a lock.
//
// BO3MACFIX_MOVEFORCE=1 in the environment: everything as the mod leaves it in an ordinary game, at once and whatever
// the mod's switches say (all patches but fastswap, omni-movement, a strafe scale of 0.9, a fast ADS multiplier of
// 1.4), for trying one of the pieces in the game. BO3MACFIX_MOVEDEBUG=1: the start and every switch that changes.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aae_movement.h"
#include "aae_plugin.h"
#include "gsc_obj.h"
#include "offsets.h"

// the sums below are T7Overcharged's, in its order: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Strafe_Least 0.667f      // the game's own sprint strafe scale (0x3f2ac083), T7Overcharged's least one
#define Fastads_Default 2.15f    // t7o_perk_fastads_multiplier's default
#define Dvar_Name 0x8            // dvar_t: the name (the game keeps only the hash of the dvars registered here)
#define Dvar_Value 0x28          // dvar_t: the current value (a bool's byte, an int, a float)

// ---- T7Overcharged's rules

static const char *const patch_names[Move_Patch_Count] = {
    [Move_SlidePreserveMomentum] = "slide_preserve_momentum",
    [Move_JumpSlowdown] = "jumpslowdown",
    [Move_FastSprintLeap] = "fast_sprintleap",
    [Move_FastSprintLerpIn] = "fast_sprintlerpin",
    [Move_SlideSlowdown] = "slideslowdown",
    [Move_CanSlideThenSprint] = "canslidethensprint",
    [Move_InstantSlide] = "instant_slide",
    [Move_FastSwap] = "fastswap",
    [Move_FastAds] = "fastads",
    [Move_FastJump] = "fastjump",
    [Move_SlideAngles] = "slideangles",
};

// T7Overcharged's movement_patch_all (T7Overcharged.dll 0x1800f2a6c): jumpslowdown last
const move_patch aae_movement_all_order[Move_Patch_Count] = {
    Move_SlidePreserveMomentum, Move_FastSprintLeap, Move_FastSprintLerpIn, Move_SlideSlowdown,
    Move_CanSlideThenSprint,    Move_InstantSlide,   Move_FastSwap,         Move_FastAds,
    Move_FastJump,              Move_SlideAngles,    Move_JumpSlowdown,
};

const char *aae_movement_patch_name(move_patch patch) {
    return (unsigned)patch < Move_Patch_Count ? patch_names[patch] : "?";
}

int aae_movement_patch_named(const char *name) {
    // T7Overcharged lower-cases the name with the C library's tolower first. In the C locale that is A to Z only; in
    // any other the names, all plain lower-case letters and _, can't come out of anything but the same letters.
    char lower[32];
    size_t length = strlen(name);
    if (length >= sizeof(lower))
        return -1; // longer than any patch's name
    for (size_t i = 0; i <= length; i++)
        lower[i] = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + ('a' - 'A')) : name[i];
    for (int patch = 0; patch < Move_Patch_Count; patch++)
        if (strcmp(lower, patch_names[patch]) == 0)
            return patch;
    return -1;
}

move_number aae_movement_stoi(const char *text, int *value) {
    // the C library's white space (space, \t, \n, \v, \f, \r), then an optional sign
    const char *p = text;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    bool negative = *p == '-';
    if (*p == '+' || *p == '-')
        p++;
    // no digit: strtol hands back the text's start as its end, and std::stoi throws invalid_argument
    if (*p < '0' || *p > '9')
        return Move_Number_None;
    // beyond what a 32-bit long holds: strtol says ERANGE, and std::stoi throws out_of_range
    int64_t number = 0;
    bool too_big = false;
    for (; *p >= '0' && *p <= '9'; p++) {
        number = number * 10 + (*p - '0');
        if (number > (int64_t)INT32_MAX + 1) {
            too_big = true;
            number = (int64_t)INT32_MAX + 1;
        }
    }
    if (too_big || number > (negative ? (int64_t)INT32_MAX + 1 : (int64_t)INT32_MAX))
        return Move_Number_Out_Of_Range;
    *value = (int)(negative ? -number : number);
    return Move_Number_Ok;
}

bool aae_movement_patch_command(int argc, const char *const *argv, move_patch *patch, bool *on) {
    // T7Overcharged.dll 0x1800f355c: exactly three words, or nothing at all (not even its usage)
    if (argc != 3 || argv == NULL)
        return false;
    // the number is read before the name is looked at: one std::stoi throws for leaves the command for any name.
    // On Windows that is a C++ exception out of a console command; here the command just changes nothing.
    int value = 0;
    if (aae_movement_stoi(argv[2], &value) != Move_Number_Ok)
        return false;
    int named = aae_movement_patch_named(argv[1]);
    if (named < 0)
        return false;
    *patch = (move_patch)named;
    *on = value != 0;
    return true;
}

move_all_command aae_movement_patch_all_command(int argc, const char *const *argv, bool *on) {
    // T7Overcharged.dll 0x1800f33f4
    if (argc != 2 || argv == NULL)
        return Move_All_Usage;
    int value = 0;
    if (aae_movement_stoi(argv[1], &value) != Move_Number_Ok)
        return Move_All_Nothing;
    *on = value != 0;
    return Move_All_Switch;
}

float aae_movement_strafe_rule(bool have_dvar, float value) {
    // T7Overcharged.dll 0x1800f2acc: comiss, so a value that isn't a number isn't above it
    float least = Strafe_Least;
    if (have_dvar && value > least)
        return value;
    return least;
}

// ---- the switches

// What T7Overcharged's toggles leave behind, as the movement code reads it: bit n for patch n, and in the upper half
// the bits of the fast ADS multiplier (a float) copied at the last switch-on of fastads. One word, so that whoever
// reads it sees a patch and the multiplier as one toggle left them.
typedef struct {
    _Atomic uint64_t state;
} move_switches;
#define Multiplier_Shift 32

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}
static float bits_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static uint32_t patch_bits(uint64_t state) {
    return (uint32_t)state;
}
static float multiplier_of(uint64_t state) {
    return bits_float((uint32_t)(state >> Multiplier_Shift));
}

// One patch switched as T7Overcharged's toggle for it does. Switching fastads on also copies the dvar's value as it
// is now (its 4 bytes as they are), every time. Returns whether the patch's bit changed.
static bool switch_patch(move_switches *switches, move_patch patch, bool on, uint32_t fastads_value_bits) {
    uint64_t bit = 1ull << patch;
    uint64_t before = atomic_load_explicit(&switches->state, memory_order_relaxed), after;
    do {
        after = on ? before | bit : before & ~bit;
        if (on && patch == Move_FastAds)
            after = (after & UINT32_MAX) | (uint64_t)fastads_value_bits << Multiplier_Shift;
    } while (!atomic_compare_exchange_weak_explicit(&switches->state, &before, after, memory_order_release,
                                                    memory_order_relaxed));
    return ((before & bit) != 0) != on;
}

// ---- the game's side

static bool enabled, forced, debug;
// T7Overcharged's movement component has started (and hook_movement_perks was 1 then)
static _Atomic bool started;
// its start has been looked at: the first time the mod loaded T7Overcharged
static _Atomic bool start_looked_at;
// nothing on; 2.15 (0x4009999a), the dvar's default, until fastads is switched on
static move_switches switches = { (uint64_t)0x4009999a << Multiplier_Shift };
// T7Overcharged's three dvars as the game registered them, NULL until its start
static _Atomic(const uint8_t *) omni_dvar, strafe_dvar, fastads_dvar;

static void *(*Cmd_FindCommand)(const char *name);
static void (*Cmd_AddCommandInternal)(const char *name, void (*function)(void), void *command);
static uint8_t *(*Sys_GetTLS)(void);
static uint8_t *(*Dvar_RegisterInt)(uint32_t hash, const char *name, int value, int min, int max, uint32_t flags,
                                    const char *description);
static uint8_t *(*Dvar_RegisterFloat)(uint32_t hash, const char *name, float value, float min, float max,
                                      uint32_t flags, const char *description);
static uint8_t *(*Dvar_RegisterBool)(uint32_t hash, const char *name, bool value, uint32_t flags,
                                     const char *description);
// the game's cmd_function_s for each command, which Cmd_AddCommandInternal links into its list
static _Alignas(8) uint8_t patch_command[0x30], patch_all_command[0x30];

// a dvar's current value, read while the game may be setting it on another thread
static uint32_t dvar_bits(const uint8_t *dvar) {
    return __atomic_load_n((const uint32_t *)(dvar + Dvar_Value), __ATOMIC_RELAXED);
}
static bool dvar_bool(const uint8_t *dvar) {
    return __atomic_load_n(dvar + Dvar_Value, __ATOMIC_RELAXED) != 0;
}

bool aae_movement_enabled(void) {
    return enabled;
}

bool aae_movement_started(void) {
    if (forced)
        return true;
    return atomic_load_explicit(&started, memory_order_acquire) && aae_plugin_loaded_lately();
}

bool aae_movement_patch_on(move_patch patch) {
    if ((unsigned)patch >= Move_Patch_Count)
        return false;
    if (forced)
        return patch != Move_FastSwap;
    // the bit first: when it is off, that is all there is to look at
    return (atomic_load_explicit(&switches.state, memory_order_acquire) >> patch & 1) != 0 && aae_movement_started();
}

bool aae_movement_omni(void) {
    if (forced)
        return true;
    const uint8_t *dvar = atomic_load_explicit(&omni_dvar, memory_order_acquire);
    return dvar != NULL && dvar_bool(dvar) && aae_movement_started();
}

float aae_movement_strafe_scale(void) {
    if (forced)
        return 0.9f;
    const uint8_t *dvar = atomic_load_explicit(&strafe_dvar, memory_order_acquire);
    // before the start, and without the mod, the game's own scale
    if (dvar == NULL || !aae_movement_started())
        return Strafe_Least;
    return aae_movement_strafe_rule(true, bits_float(dvar_bits(dvar)));
}

float aae_movement_fastads_multiplier(void) {
    if (forced)
        return 1.4f;
    return multiplier_of(atomic_load_explicit(&switches.state, memory_order_acquire));
}

// what each switch does, as it happens: the debug lines (a test puts its own here)
static void print_switch(move_patch patch, bool on, bool changed, float multiplier, float multiplier_before) {
    if (!debug)
        return;
    if (patch == Move_FastAds && on && (changed || float_bits(multiplier) != float_bits(multiplier_before)))
        printf("movement: fastads on, fast ADS multiplier %g%s\n", (double)multiplier,
               changed ? "" : " (copied again)");
    else if (changed)
        printf("movement: %s %s\n", patch_names[patch], on ? "on" : "off");
}
static void (*switched)(move_patch patch, bool on, bool changed, float multiplier, float multiplier_before) =
    print_switch;

// a toggle of T7Overcharged's, with the fast ADS dvar as it is now
static void toggle(move_patch patch, bool on) {
    const uint8_t *fastads = atomic_load_explicit(&fastads_dvar, memory_order_acquire);
    float before = multiplier_of(atomic_load_explicit(&switches.state, memory_order_acquire));
    // the dvar is registered at the start, before any toggle; without it the multiplier stays as it was
    uint32_t value = fastads != NULL ? dvar_bits(fastads) : float_bits(before);
    bool changed = switch_patch(&switches, patch, on, value);
    switched(patch, on, changed, multiplier_of(atomic_load_explicit(&switches.state, memory_order_acquire)), before);
}

static void toggle_all(bool on) {
    for (int i = 0; i < Move_Patch_Count; i++)
        toggle(aae_movement_all_order[i], on);
}

// after a command, with BO3MACFIX_MOVEDEBUG=1: what is on and the dvars, for the log
static void print_state(int argc, const char *const *argv) {
    char line[160] = "", on[256] = "", off[256] = "";
    for (int i = 0; i < argc && argv != NULL; i++)
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s", i > 0 ? " " : "", argv[i]);
    uint64_t state = atomic_load_explicit(&switches.state, memory_order_acquire);
    for (int patch = 0; patch < Move_Patch_Count; patch++) {
        char *list = patch_bits(state) >> patch & 1 ? on : off;
        snprintf(list + strlen(list), sizeof(on) - strlen(list), "%s%s", list[0] != '\0' ? " " : "",
                 patch_names[patch]);
    }
    const uint8_t *omni = atomic_load_explicit(&omni_dvar, memory_order_acquire);
    const uint8_t *strafe = atomic_load_explicit(&strafe_dvar, memory_order_acquire);
    float strafe_value = strafe != NULL ? bits_float(dvar_bits(strafe)) : 0.0f;
    printf("movement: after \"%s\": on [%s], off [%s]; t7o_omnimovement %d, t7o_strafe_speed_scale %g (strafe scale "
           "%g), fast ADS multiplier %g\n",
           line, on, off, omni != NULL && dvar_bool(omni), (double)strafe_value,
           (double)aae_movement_strafe_rule(strafe != NULL, strafe_value),
           (double)multiplier_of(state));
}

// the words of the console command being run, read the way t7overcharged.c's commands read them (the thread's own
// block, +0x18: the arguments, with one argc and argv for each level of commands run from commands)
#define TLS_CmdArgs 0x18
#define CmdArgs_Argc 0x64
#define CmdArgs_Argv 0x88
#define CmdArgs_Nesting_Max 8
static const char *const *command_words(int *argc) {
    uint8_t *tls = Sys_GetTLS();
    const uint8_t *args = tls != NULL ? *(const uint8_t *const *)(tls + TLS_CmdArgs) : NULL;
    if (args == NULL)
        return NULL;
    int nesting = *(const int *)args;
    if (nesting < 0 || nesting >= CmdArgs_Nesting_Max)
        return NULL;
    *argc = *(const int *)(args + CmdArgs_Argc + 4 * (size_t)nesting);
    return *(const char *const *const *)(args + CmdArgs_Argv + 8 * (size_t)nesting);
}

// whether a command may switch: only once T7Overcharged's movement component has started
static bool commands_work(int argc, const char *const *argv) {
    if (!enabled)
        return false;
    if (atomic_load_explicit(&started, memory_order_acquire))
        return true;
    if (debug)
        printf("movement: \"%s%s%s\" does nothing: T7Overcharged's movement component hasn't started\n",
               argc > 0 && argv != NULL ? argv[0] : "", argc > 1 && argv != NULL ? " " : "",
               argc > 1 && argv != NULL ? argv[1] : "");
    return false;
}

static void movement_patch_f(void) {
    int argc = 0;
    const char *const *argv = command_words(&argc);
    if (argv == NULL || !commands_work(argc, argv))
        return;
    move_patch patch;
    bool on;
    if (aae_movement_patch_command(argc, argv, &patch, &on))
        toggle(patch, on);
    if (debug)
        print_state(argc, argv);
}

static void movement_patch_all_f(void) {
    int argc = 0;
    const char *const *argv = command_words(&argc);
    if (argv == NULL || !commands_work(argc, argv))
        return;
    bool on;
    switch (aae_movement_patch_all_command(argc, argv, &on)) {
    case Move_All_Usage:
        // T7Overcharged's log line, which on Windows also goes to the game's console
        printf("movement_patch_all <0/1>\n");
        return;
    case Move_All_Switch:
        toggle_all(on);
        break;
    case Move_All_Nothing:
        break;
    }
    if (debug)
        print_state(argc, argv);
}

// T7Overcharged's dvars, with the name written into the dvar after it is registered, as T7Overcharged does
static uint8_t *named(uint8_t *dvar, const char *name) {
    if (dvar != NULL)
        memcpy(dvar + Dvar_Name, &name, sizeof(name));
    return dvar;
}

void aae_movement_t7o_started(void) {
    if (!enabled || atomic_exchange_explicit(&start_looked_at, true, memory_order_acq_rel))
        return;
    // T7Overcharged.dll 0x1800f2b80, in its order
    const char *perks_name = "hook_movement_perks";
    const uint8_t *perks =
        named(Dvar_RegisterInt(gsc_hash(perks_name), perks_name, 1, 0, 1, 0, "enhance player movements"), perks_name);
    int perks_value = perks != NULL ? (int)dvar_bits(perks) : 0;
    if (perks_value != 1) {
        if (debug)
            printf("movement: the mod started T7Overcharged: hook_movement_perks is %d, its movement component stays "
                   "off\n", perks_value);
        return;
    }
    const char *fastads_name = "t7o_perk_fastads_multiplier", *strafe_name = "t7o_strafe_speed_scale",
               *omni_name = "t7o_omnimovement";
    const uint8_t *fastads = named(Dvar_RegisterFloat(gsc_hash(fastads_name), fastads_name, Fastads_Default, 1.0f,
                                                      3.0f, 0, "fastads multiplier"), fastads_name);
    atomic_store_explicit(&fastads_dvar, fastads, memory_order_release);
    const uint8_t *strafe = named(Dvar_RegisterFloat(gsc_hash(strafe_name), strafe_name, Strafe_Least, Strafe_Least,
                                                     0.97f, 0, "strafe speed scale"), strafe_name);
    atomic_store_explicit(&strafe_dvar, strafe, memory_order_release);
    const uint8_t *omni =
        named(Dvar_RegisterBool(gsc_hash(omni_name), omni_name, false, 0, "enable omni-movement"), omni_name);
    atomic_store_explicit(&omni_dvar, omni, memory_order_release);
    atomic_store_explicit(&started, true, memory_order_release);
    // then all eleven off and fastswap off once more, as T7Overcharged does: they are off already
    toggle_all(false);
    toggle(Move_FastSwap, false);
    if (debug)
        printf("movement: the mod started T7Overcharged: hook_movement_perks is 1, its movement component is on: "
               "t7o_perk_fastads_multiplier %g, t7o_strafe_speed_scale %g, t7o_omnimovement %d; every patch off\n",
               fastads != NULL ? (double)bits_float(dvar_bits(fastads)) : 0.0,
               strafe != NULL ? (double)bits_float(dvar_bits(strafe)) : 0.0, omni != NULL && dvar_bool(omni));
}

// the game's functions, at its base address (a test puts a made-up game's here instead)
static void game_functions(uint64_t game_base_address) {
    Cmd_FindCommand = (void *)(game_base_address + ADDR_Cmd_FindCommand);
    Cmd_AddCommandInternal = (void *)(game_base_address + ADDR_Cmd_AddCommandInternal);
    Sys_GetTLS = (void *)(game_base_address + ADDR_Sys_GetTLS);
    Dvar_RegisterInt = (void *)(game_base_address + ADDR_Dvar_RegisterInt);
    Dvar_RegisterFloat = (void *)(game_base_address + ADDR_Dvar_RegisterFloat);
    Dvar_RegisterBool = (void *)(game_base_address + ADDR_Dvar_RegisterBool);
}
static void (*find_game)(uint64_t game_base_address) = game_functions;

void aae_movement_init(uint64_t game_base_address, int setting) {
    const char *force = getenv("BO3MACFIX_MOVEFORCE"), *debug_env = getenv("BO3MACFIX_MOVEDEBUG");
    debug = debug_env != NULL && atoi(debug_env) != 0;
    enabled = setting != 0;
    forced = enabled && force != NULL && atoi(force) != 0;
    aae_plugin_init(game_base_address);
    find_game(game_base_address);
    // the commands whatever the setting says, at start-up like the stand-in's other commands
    if (Cmd_FindCommand("movement_patch") == NULL)
        Cmd_AddCommandInternal("movement_patch", movement_patch_f, patch_command);
    if (Cmd_FindCommand("movement_patch_all") == NULL)
        Cmd_AddCommandInternal("movement_patch_all", movement_patch_all_f, patch_all_command);
    if (!enabled) {
        if (debug)
            printf("movement: AAEMovement is 0: T7Overcharged's movement changes stay off; movement_patch and "
                   "movement_patch_all are there and do nothing\n");
        return;
    }
    if (forced)
        printf("movement: BO3MACFIX_MOVEFORCE=1: every change on as the mod leaves it in an ordinary game (all "
               "patches but fastswap, omni-movement, strafe scale 0.9, fast ADS multiplier 1.4), whatever its switches "
               "say\n");
    else if (debug)
        printf("movement: AAEMovement is 1: the switches wait for the mod to start T7Overcharged\n");
}
