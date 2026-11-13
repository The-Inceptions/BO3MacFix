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
