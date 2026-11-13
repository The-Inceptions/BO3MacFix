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

// For the debug line (BO3MACFIX_MOVEDEBUG=1): what the game's slide test (its slide function, before it starts a
// slide) sees for a player, and its answer, in the game's order. Read in the Mac game's 0x100e8189d (Windows
// 0x1401683a0); T7Overcharged changes nothing in it but the 150 of the landing's test.
typedef struct {
    uint64_t flags;        // the player's movement flags
    int32_t server_time;   // the move's time
    int32_t slide_time;    // when the last slide started, or ended
    int32_t ground_entity; // what the player stands on, 0x3ff for nothing
    int32_t ground_plane;  // the move's +0x30: non-zero on a ground plane
    bool slide_button;     // the command's slide button
    int32_t sprint_start;  // when the sprint began
    float velocity[3];
} aae_move_slide_asked_t;

typedef enum {
    Slide_Answer_Sliding,        // already sliding
    Slide_Answer_Not_Allowed,    // movement flag 16 or 40 (40 is what a script's allowslide(false) sets)
    Slide_Answer_Too_Soon,       // the last slide's time is still ahead
    Slide_Answer_In_Air,         // in the air: with the slide button down the game asks for a slide on landing
    Slide_Answer_No_Ground,      // not on a ground plane
    Slide_Answer_Landing,        // a slide was asked for in the air: the landing's test decides (150 units/s)
    Slide_Answer_No_Button,      // the slide button isn't down
    Slide_Answer_Not_Sprinting,
    Slide_Answer_Sprint_Too_New, // under 200 ms into the sprint
    Slide_Answer_Too_Slow,       // under 100 units/s
    Slide_Answer_Later,          // all of these passed: the game's later tests decide (weapon, room ahead, aiming ...)
} aae_move_slide_answer;

aae_move_slide_answer aae_move_slide_answer_of(const aae_move_slide_asked_t *asked);

// installs the hooks, once, when AAEMovement is on; call behind aae_movement_init
void aae_move_slide_init(uint64_t game_base_address);
