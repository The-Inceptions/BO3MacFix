// T7Overcharged's slide changes, which All-around Enhancement switches on for every player (AAEMovement): its own
// slide push, no slowdown in and after slides, a slide asked for in the air that starts on landing at any speed (one on
// the ground still needs a sprint), and the view free while sliding. See aae_move_slide.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// slide_preserve_momentum: T7Overcharged's own push of a sliding player, which replaces the game's
typedef enum {
    Slide_Push_None,    // neither of the two pushes applies
    Slide_Push_Surface, // the player is on a slide surface
    Slide_Push_Flagged, // the other push (the player's movement flag 51, and the move's +0x64)
} aae_move_slide_push_kind;

// what the push gives the game's PM_Accelerate
typedef struct {
    float direction[3];
    float wish_speed, acceleration;
} aae_move_slide_push_t;

// Which push applies and what it gives PM_Accelerate. slide_surface: the move's flag 2 (+0x54); flags: the player's
// movement flags; flagged_move: the move's int at +0x64; along: the move's two floats at +0x40 the push goes along;
// velocity: the player's.
aae_move_slide_push_kind aae_move_slide_push(bool slide_surface, uint64_t flags, int32_t flagged_move,
                                             const float along[2], const float velocity[3],
                                             aae_move_slide_push_t *push);
// After the other push: whether the velocity from before it is to be put back
bool aae_move_slide_push_undone(const float velocity[3]);

// slideslowdown: the length of the slow speed ramp after a slide that the game's compare "time since the slide >=
// length" is to see (the ramp is over when that holds)
uint32_t aae_move_slide_ramp_length(bool on, uint32_t length);
// slideslowdown: the factor of the extra friction on a slide surface (the game's 1.5)
float aae_move_slide_surface_friction(bool on, float factor);
// instant_slide: the least horizontal speed a slide may start at (the game's 150)
float aae_move_slide_least_speed(bool on, float least);
// slideangles: the view pitch a slide's start compares with 10.05 degrees, to pull the view to 10 when it is more
float aae_move_slide_start_pitch(bool on, float pitch);
// slideangles: whether the 20 degree pitch clamp while sliding is left out
bool aae_move_slide_clamp_left_out(bool on);

// installs the hooks, once, when AAEMovement is on; call behind aae_movement_init
void aae_move_slide_init(uint64_t game_base_address);
