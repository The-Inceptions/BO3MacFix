// T7Overcharged's movement changes, which All-around Enhancement switches on for every player (AAEMovement): the
// switches. See aae_movement.c. The changes themselves are in aae_move_sprint.c, aae_move_jump.c and
// aae_move_slide.c, which ask here whether each is on.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// T7Overcharged's eleven movement patches, by its names and in its order
typedef enum {
    Move_SlidePreserveMomentum,
    Move_JumpSlowdown,
    Move_FastSprintLeap,
    Move_FastSprintLerpIn,
    Move_SlideSlowdown,
    Move_CanSlideThenSprint,
    Move_InstantSlide,
    Move_FastSwap,
    Move_FastAds,
    Move_FastJump,
    Move_SlideAngles,
    Move_Patch_Count
} move_patch;

void aae_movement_init(uint64_t game_base_address, int enabled);

// The setting AAEMovement is on: the pieces install their hooks only then (once, at start). Whether a change acts
// is asked each time it could, with the functions below.
bool aae_movement_enabled(void);
// T7Overcharged's movement component has started: the mod has loaded T7Overcharged and its dvar hook_movement_perks
// was 1 then. Nothing below is on before.
bool aae_movement_started(void);
// one of the eleven patches, as the mod's commands movement_patch and movement_patch_all left it
bool aae_movement_patch_on(move_patch patch);
// the dvar t7o_omnimovement, as it is now
bool aae_movement_omni(void);
// the dvar t7o_strafe_speed_scale, as it is now: never below 0.667, and 0.667 for what isn't a number
float aae_movement_strafe_scale(void);
// the dvar t7o_perk_fastads_multiplier as it was when the fastads patch was switched on
float aae_movement_fastads_multiplier(void);

// ---- the switches' own (aae_movement.c)

// The mod has loaded T7Overcharged (the stand-in's init, t7overcharged.c): the first time, T7Overcharged's movement
// component starts, as it does on Windows: its dvars, and the switches from then on.
void aae_movement_t7o_started(void);

// T7Overcharged's rules for its switches, as plain functions.

// std::stoi as T7Overcharged's compiler has it (strtol in base 10 with a 32-bit long): white space, a sign, digits,
// and whatever follows them is left alone. Where std::stoi throws, *value is left as it was.
typedef enum { Move_Number_Ok, Move_Number_None, Move_Number_Out_Of_Range } move_number;
move_number aae_movement_stoi(const char *text, int *value);
// a patch's name, as the commands take it
const char *aae_movement_patch_name(move_patch patch);
// the patch a name means, after lower-casing A to Z as T7Overcharged does; -1 for none
int aae_movement_patch_named(const char *name);
// the order movement_patch_all switches the patches in
extern const move_patch aae_movement_all_order[Move_Patch_Count];
// movement_patch <name> <0/1>: false when it changes nothing (not three words, a number std::stoi throws for, or
// no such patch); otherwise the patch and whether it is switched on (any number but 0)
bool aae_movement_patch_command(int argc, const char *const *argv, move_patch *patch, bool *on);
// movement_patch_all <0/1>: every patch switched on (any number but 0) or off, nothing for a number std::stoi throws
// for, and T7Overcharged's usage line for any other number of words than two
typedef enum { Move_All_Nothing, Move_All_Switch, Move_All_Usage } move_all_command;
move_all_command aae_movement_patch_all_command(int argc, const char *const *argv, bool *on);
// T7Overcharged's sprint strafe scale: the dvar's value when there is the dvar and its value is above 0.667, else
// 0.667 (also for what isn't a number)
float aae_movement_strafe_rule(bool have_dvar, float value);
