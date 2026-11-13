// T7Overcharged's changes to sprinting, which All-around Enhancement switches on (AAEMovement): omni-movement, the
// strafe speed scale, fast_sprintlerpin, canslidethensprint and fast_sprintleap. See aae_move_sprint.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// puts in the hooks, once, when the setting AAEMovement is on (and, for a debug run, when BO3MACFIX_MOVEDEBUG is set)
void aae_move_sprint_init(uint64_t game_base_address);

// ---- omni-movement: T7Overcharged's wrapper of the game's sprint decision, PM_UpdateSprint(pmove_t *, pml_t *)

// the forward stick the decision sees: the larger of the forward and the side stick, 127 at most
int8_t aae_move_sprint_omni_forward(int8_t forward, int8_t right);
// the buttons it sees: the sprint key (bit 18) counts as the sprint button (bit 30) when that stick is moved at all
uint32_t aae_move_sprint_omni_buttons(uint32_t buttons, int8_t omni_forward);
// Runs the decision, update, for the move pm: as it is when omni is false; otherwise with the forward stick and the
// buttons above, and afterwards both put back as they were, whatever update did with them.
void aae_move_sprint_omni(uint8_t *pm, uint8_t *pml, bool omni, void (*update)(uint8_t *pm, uint8_t *pml));

// ---- the strafe speed scale

// T7Overcharged's BG_GetSprintStrafeSpeedScale for a dvar t7o_strafe_speed_scale with this value: the value when it
// is above the game's 0.667, 0.667 otherwise (also for what isn't a number)
float aae_move_sprint_strafe_scale(float dvar_value);

// ---- the three compares: what T7Overcharged's changed instruction decides, and the value the Mac's register is to
// hold for the Mac's own compare to decide the same. fast is the patch being on.

// Jump_Start's two "elapsed > 1500" (ms of sprint): whether the sprint leap is at full strength. fast: elapsed > 500.
bool aae_move_sprint_leap_full(int32_t elapsed, bool fast);
int32_t aae_move_sprint_leap_register(int32_t elapsed, bool fast);
// the move speed's "elapsed >= 350" (ms of sprint): whether the sprint speed has stopped ramping up. fast: >= 10.
bool aae_move_sprint_lerp_done(int32_t elapsed, bool fast);
int32_t aae_move_sprint_lerp_register(int32_t elapsed, bool fast);
// the test before a sprint starts, "movement state == 3" (the slide state): whether it keeps the sprint from
// starting. fast: never.
bool aae_move_sprint_slide_state_blocks(int32_t state, bool fast);
