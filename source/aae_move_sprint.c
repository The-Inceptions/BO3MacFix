// T7Overcharged's changes to sprinting, part of the movement changes All-around Enhancement switches on (AAEMovement):
//
// - omni-movement (the dvar t7o_omnimovement, which the mod sets to 1): a sprint in any direction. T7Overcharged wraps
//   the game's sprint decision, PM_UpdateSprint. While it runs, the forward stick is the larger of the forward and
//   the side stick (so a stick held fully sideways or back counts as fully forward), and the sprint key (button bit
//   18) counts as the sprint button (bit 30) when the stick is moved at all. Afterwards both are put back as they
//   were, whatever the decision did with them. Here PM_UpdateSprint is hooked the same way.
// - the strafe speed scale (the dvar t7o_strafe_speed_scale, the mod sets 0.9): while sprinting, the game multiplies
//   the side stick by what BG_GetSprintStrafeSpeedScale gives, 0.667. T7Overcharged replaces that function with one
//   that gives the dvar's value when it is above 0.667. The Mac's function can't be hooked (it begins with a load
//   relative to its own address), so the place right behind its only call is detoured, where its result is about to
//   be multiplied, and the result is changed there.
// - fast_sprintlerpin: when a sprint starts, the game ramps the speed up over its first 350 ms; T7Overcharged makes
//   that 10 ms.
// - canslidethensprint: the game's test whether a sprint may start says no while the player is in the slide state
//   (movement state 3); T7Overcharged takes that answer out.
// - fast_sprintleap: a jump out of a sprint gets more height and forward speed the longer the sprint was, at full
//   strength from 1500 ms on; T7Overcharged makes that 500 ms (below it the strength is still elapsed / 1500).
//
// T7Overcharged changes those three tests by writing other constants into the game's compare instructions, or nops
// over a jump. The Mac's instructions are laid out differently (the speed's "elapsed >= 350" is "elapsed > 349" here),
// so here each compare is detoured instead: the register it compares is changed so that the Mac's own compare comes
// to T7Overcharged's answer. Where a sprint is already past the game's own limit the register is rewritten too (to
// 350 or 1501): the game takes the same jump either way, and nothing behind the compare reads the register.
// tests/test_aae_move_sprint.c goes through the values the register can hold.
//
// Each change acts only while T7Overcharged's movement component has started and its own switch is on
// (aae_movement.h, asked each time); otherwise the game runs its own instructions with its own values. The hooks are
// put in once and stay, which stands for T7Overcharged putting its hooks in and taking them out. Only from the
// pre-patched executable, like every detour inside a function.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aae_move_sprint.h"
#include "aae_movement.h"
#include "detour.h"
#include "dobby.h"
#include "offsets.h"
#include "prepatch.h"

// the sums below are T7Overcharged's and the game's, in their order: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

// the move, pmove_t: the player state's address first, then
#define Pm_ServerTime 0x8 // int: the time of the move's command, ms
#define Pm_Buttons 0xc    // uint32_t: the command's buttons
#define Pm_Forward 0x40   // signed char: the forward stick
#define Pm_Right 0x41     // signed char: the side stick
#define Button_Sprint_Key (1u << 18) // held for sprinting (and for holding the breath)
#define Button_Sprint (1u << 30)     // what the sprint decision takes for "sprint"
#define Stick_Most 127
// the player state
#define Ps_Flags 0x10        // uint64_t: movement flags
#define Ps_Velocity 0x3c     // three floats
#define Ps_MoveState 0x8c    // int
#define Flag_Sprinting 0x20
#define Slide_State 3
// the first local client's cg_t, where the client's prediction keeps its player state
#define Cg_Size 0x342a80

// the game's limits, and T7Overcharged's
#define Game_Leap_Full_Ms 1500 // Jump_Start: full strength after this long a sprint
#define Fast_Leap_Full_Ms 500
#define Game_Lerp_Ms 350 // the move speed: no more ramping up from this long a sprint on
#define Fast_Lerp_Ms 10
#define Strafe_Least 0.667f // BG_GetSprintStrafeSpeedScale's constant, 0x3f2ac083 in all three programs

// ---- the rules

int8_t aae_move_sprint_omni_forward(int8_t forward, int8_t right) {
    // as ints, as T7Overcharged has them: -128 counts as 128, and then as 127
    int forward_size = abs(forward), right_size = abs(right);
    int most = forward_size > right_size ? forward_size : right_size;
    return (int8_t)(most < Stick_Most ? most : Stick_Most);
}

uint32_t aae_move_sprint_omni_buttons(uint32_t buttons, int8_t omni_forward) {
    return (buttons & Button_Sprint_Key) != 0 && omni_forward > 0 ? buttons | Button_Sprint : buttons;
}

void aae_move_sprint_omni(uint8_t *pm, uint8_t *pml, bool omni, void (*update)(uint8_t *pm, uint8_t *pml)) {
    if (!omni) {
        update(pm, pml);
        return;
    }
    int8_t forward = (int8_t)pm[Pm_Forward];
    int8_t seen = aae_move_sprint_omni_forward(forward, (int8_t)pm[Pm_Right]);
    uint32_t buttons, held;
    memcpy(&buttons, pm + Pm_Buttons, sizeof(buttons));
    held = aae_move_sprint_omni_buttons(buttons, seen);
    pm[Pm_Forward] = (uint8_t)seen;
    memcpy(pm + Pm_Buttons, &held, sizeof(held));
    update(pm, pml);
    // T7Overcharged puts back only these two: what the decision did to the side stick stays
    pm[Pm_Forward] = (uint8_t)forward;
    memcpy(pm + Pm_Buttons, &buttons, sizeof(buttons));
}

// not what isn't a number (infinities are numbers here)
static bool is_number(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7fffffff) <= 0x7f800000;
}

float aae_move_sprint_strafe_scale(float dvar_value) {
    // its comiss: what isn't a number isn't above. That is asked by the bits first: otherwise a compiler may make a
    // max instruction of the compare, and arm64's (fmaxnm) gives a signalling NaN back instead of 0.667
    return is_number(dvar_value) && dvar_value > Strafe_Least ? dvar_value : Strafe_Least;
}

bool aae_move_sprint_leap_full(int32_t elapsed, bool fast) {
    return elapsed > (fast ? Fast_Leap_Full_Ms : Game_Leap_Full_Ms);
}

int32_t aae_move_sprint_leap_register(int32_t elapsed, bool fast) {
    // the Mac compares with 1500: just past it is past 500 as well
    return fast && aae_move_sprint_leap_full(elapsed, true) ? Game_Leap_Full_Ms + 1 : elapsed;
}

bool aae_move_sprint_lerp_done(int32_t elapsed, bool fast) {
    return elapsed >= (fast ? Fast_Lerp_Ms : Game_Lerp_Ms);
}

int32_t aae_move_sprint_lerp_register(int32_t elapsed, bool fast) {
    // the Mac's test is "elapsed > 349"
    return fast && aae_move_sprint_lerp_done(elapsed, true) ? Game_Lerp_Ms : elapsed;
}

bool aae_move_sprint_slide_state_blocks(int32_t state, bool fast) {
    return !fast && state == Slide_State;
}

// ---- in the game

// T7Overcharged's movement component is at work. The setting is asked as well: a debug run puts the hooks in without
// it, to show the game's own numbers.
static bool at_work(void) {
    return aae_movement_enabled() && aae_movement_started();
}
static bool patch_on(move_patch patch) {
    return at_work() && aae_movement_patch_on(patch);
}

// BO3MACFIX_MOVEDEBUG=1: a line when what a place does changes, for each thread (the server's moves and the client's
// prediction run on threads of their own), never one for every move: there are several moves a frame
static int debug;
static uint8_t *const *cg_clients; // the game's: the local clients' cg_t
enum { Said_Decision, Said_Strafe, Said_Speed, Said_Slide, Said_Leap, Said_Places };
static _Thread_local struct {
    bool said;
    uint64_t what;
} said[Said_Places];
static bool news(int place, uint64_t what) {
    if (said[place].said && said[place].what == what)
        return false;
    said[place].said = true;
    said[place].what = what;
    return true;
}
static const char *side_of(const uint8_t *ps) {
    uintptr_t cg = cg_clients != NULL ? (uintptr_t)*cg_clients : 0, at = (uintptr_t)ps;
    return cg != 0 && at >= cg && at - cg < Cg_Size ? "client" : "server";
}
static uint8_t *player_of(const uint8_t *pm) {
    uint8_t *ps;
    memcpy(&ps, pm, sizeof(ps));
    return ps;
}
static bool sprinting(const uint8_t *ps) {
    uint64_t flags;
    memcpy(&flags, ps + Ps_Flags, sizeof(flags));
    return (flags & Flag_Sprinting) != 0;
}
// where a stick is, for telling one way of holding it from another
static int way(int8_t stick) {
    return stick > 64 ? 2 : stick < -64 ? 0 : 1;
}
static int32_t read_int(const uint8_t *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static void say_decision(uint8_t *pm, int8_t forward, int8_t right, uint32_t buttons, bool omni) {
    int8_t seen = omni ? aae_move_sprint_omni_forward(forward, right) : forward;
    uint32_t held = omni ? aae_move_sprint_omni_buttons(buttons, seen) : buttons;
    const uint8_t *ps = player_of(pm);
    bool key = buttons & Button_Sprint_Key, button = buttons & Button_Sprint, now = sprinting(ps);
    uint64_t what = (uint64_t)way(forward) | (uint64_t)way(right) << 2 | (uint64_t)key << 4 | (uint64_t)button << 5 |
                    (uint64_t)omni << 6 | (uint64_t)(seen != forward || held != buttons) << 7 | (uint64_t)now << 8;
    if (!news(Said_Decision, what))
        return;
    char seen_as[96];
    if (omni)
        snprintf(seen_as, sizeof(seen_as), "seen as forward %d, sprint button %s (omni-movement)", seen,
                 held & Button_Sprint ? "held" : "not held");
    else
        snprintf(seen_as, sizeof(seen_as), "seen as it is (omni-movement off)");
    printf("movement: sprint decision (%s): stick forward %d side %d, sprint key %s, sprint button %s; %s; "
           "sprinting: %s\n", side_of(ps), forward, right, key ? "held" : "not held", button ? "held" : "not held",
           seen_as, now ? "yes" : "no");
}

// PM_UpdateSprint(pmove_t *, pml_t *): the game's sprint decision, all of it (what Windows has in functions of their
// own is inlined into it here)
install_hook_name(PM_UpdateSprint, void, uint8_t *pm, uint8_t *pml) {
    bool omni = at_work() && aae_movement_omni();
    if (!debug) {
        aae_move_sprint_omni(pm, pml, omni, orig_PM_UpdateSprint);
        return;
    }
    int8_t forward = (int8_t)pm[Pm_Forward], right = (int8_t)pm[Pm_Right];
    uint32_t buttons;
    memcpy(&buttons, pm + Pm_Buttons, sizeof(buttons));
    aae_move_sprint_omni(pm, pml, omni, orig_PM_UpdateSprint);
    say_decision(pm, forward, right, buttons, omni);
}

// "mulss xmm0, [rbp - 0x2c]" in PM_WalkMove, right behind its call of BG_GetSprintStrafeSpeedScale (only while
// sprinting): that function's result, 0.667, is in xmm0, about to be multiplied by the side stick (at rbp - 0x2c as
// a float); the move in rbx. The product, cut to a whole number, becomes the move's side stick.
detour_with_registers(PM_WalkMove_StrafeScaled) {
    float game_scale, scale;
    memcpy(&game_scale, registers->xmm[0], sizeof(game_scale));
    scale = game_scale;
    if (at_work()) {
        // the interface's value has T7Overcharged's rule in it already; once more changes nothing, and keeps this
        // place T7Overcharged's whatever comes from there
        scale = aae_move_sprint_strafe_scale(aae_movement_strafe_scale());
        memcpy(registers->xmm[0], &scale, sizeof(scale));
    }
    if (debug) {
        const uint8_t *pm = (const uint8_t *)registers->rbx;
        int8_t right = (int8_t)pm[Pm_Right];
        uint32_t bits;
        memcpy(&bits, &scale, sizeof(bits));
        if (news(Said_Strafe, (uint64_t)(uint8_t)right | (uint64_t)bits << 8))
            printf("movement: strafe while sprinting (%s): side stick %d becomes %d, scale %.3f (%s), the game's own "
                   "%.3f makes it %d\n", side_of(player_of(pm)), right, (int8_t)(int32_t)(scale * (float)right), scale,
                   at_work() ? "T7Overcharged's" : "the game's own", game_scale,
                   (int8_t)(int32_t)(game_scale * (float)right));
    }
}

// "cmp eax, 349" in PM_CmdScaleImpl (where the move speed's scale is inlined), a "jg" behind it: eax is how long the
// player has been sprinting, ms; past 349 the speed isn't ramped up any more. The move in r15.
detour_with_registers(PM_CmdScale_SprintLerpCmp) {
    int32_t elapsed = (int32_t)registers->rax;
    bool fast = patch_on(Move_FastSprintLerpIn);
    int32_t value = aae_move_sprint_lerp_register(elapsed, fast);
    if (value != elapsed)
        registers->rax = (uint32_t)value;
    if (debug) {
        bool done = aae_move_sprint_lerp_done(elapsed, fast);
        if (news(Said_Speed, done))
            printf("movement: sprint speed (%s): %d ms into the sprint %s (%s: full from %d ms)\n",
                   side_of(player_of((const uint8_t *)registers->r15)), elapsed,
                   done ? "at full speed" : "still ramping up", fast ? "fast_sprintlerpin" : "the game's own",
                   fast ? Fast_Lerp_Ms : Game_Lerp_Ms);
    }
}

// "cmp dword [rbx + 0x8c], 3 ; je <no sprint>" in PM_UpdateSprint, where the game's test whether a sprint may start is
// inlined: the player state in rbx; in the slide state no sprint starts. Yes: the game goes on behind the je.
detour_choice(PM_UpdateSprint_SlideStateCmp, "mov %rbx, %rdi\n") {
    (void)second;
    bool fast = patch_on(Move_CanSlideThenSprint);
    if (debug) {
        const uint8_t *ps = (const uint8_t *)first;
        int32_t state = read_int(ps + Ps_MoveState);
        if (news(Said_Slide, (uint64_t)(state == Slide_State) | (uint64_t)fast << 1) && state == Slide_State)
            printf("movement: slide state (%s): %s\n", side_of(ps),
                   aae_move_sprint_slide_state_blocks(state, fast) ? "keeps a sprint from starting (the game's own)"
                                                                   : "a sprint may start in it (canslidethensprint)");
    }
    return fast;
}

// Jump_Start's two tests of how long the sprint was, each a "cmp <register>, 1500" with a "jg" behind it: past
// 1500 ms the leap is at full strength, otherwise at elapsed / 1500 of it. The first one, "cmp ecx, 1500", is for the
// jump's height, the second, "cmp eax, 1500", for its speed along the ground. The move in r14, the player state in r15.
static bool fast_leap(void) {
    return patch_on(Move_FastSprintLeap);
}
detour_with_registers(Jump_Start_SprintLeapCmp1) {
    int32_t elapsed = (int32_t)registers->rcx, value = aae_move_sprint_leap_register(elapsed, fast_leap());
    if (value != elapsed)
        registers->rcx = (uint32_t)value;
}
detour_with_registers(Jump_Start_SprintLeapCmp2) {
    bool fast = fast_leap();
    int32_t elapsed = (int32_t)registers->rax, value = aae_move_sprint_leap_register(elapsed, fast);
    if (value != elapsed)
        registers->rax = (uint32_t)value;
    if (debug) {
        const uint8_t *pm = (const uint8_t *)registers->r14, *ps = (const uint8_t *)registers->r15;
        float velocity[3];
        memcpy(velocity, ps + Ps_Velocity, sizeof(velocity));
        // the game's own share of the full strength, as it works it out: (float)elapsed / 1500
        float own = aae_move_sprint_leap_full(elapsed, false) ? 1.0f : (float)elapsed / (float)Game_Leap_Full_Ms;
        float strength = aae_move_sprint_leap_full(elapsed, fast) ? 1.0f : (float)elapsed / (float)Game_Leap_Full_Ms;
        if (news(Said_Leap, (uint32_t)read_int(pm + Pm_ServerTime)))
            printf("movement: sprint leap (%s): a jump %d ms into the sprint at strength %.3f (%s), the game's own "
                   "%.3f; speed up %.1f, along %.1f %.1f\n", side_of(ps), elapsed, strength,
                   fast ? "fast_sprintleap: full after 500 ms" : "the game's own: full after 1500 ms", own, velocity[2],
                   velocity[0], velocity[1]);
    }
}

void aae_move_sprint_init(uint64_t game_base_address) {
    const char *env = getenv("BO3MACFIX_MOVEDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    // With the setting off nothing is put in, except for a debug run, which then shows the game's own numbers next to
    // T7Overcharged's: every handler does exactly what the game does while the component isn't at work.
    if (!aae_movement_enabled() && !debug)
        return;
    if (!prepatch_has_hook(ADDR_PM_UpdateSprint) || !prepatch_has_hook(ADDR_PM_WalkMove_StrafeScaled) ||
        !prepatch_has_hook(ADDR_PM_CmdScale_SprintLerpCmp) || !prepatch_has_hook(ADDR_PM_UpdateSprint_SlideStateCmp) ||
        !prepatch_has_hook(ADDR_Jump_Start_SprintLeapCmp1) || !prepatch_has_hook(ADDR_Jump_Start_SprintLeapCmp2)) {
        printf("movement: not running from the pre-patched executable: T7Overcharged's changes to sprinting are left "
               "out\n");
        return;
    }
    cg_clients = (uint8_t *const *)(game_base_address + ADDR_cgArray);
    install_hook_PM_UpdateSprint((void *)(game_base_address + ADDR_PM_UpdateSprint));
    install_detour_PM_WalkMove_StrafeScaled((void *)(game_base_address + ADDR_PM_WalkMove_StrafeScaled));
    install_detour_PM_CmdScale_SprintLerpCmp((void *)(game_base_address + ADDR_PM_CmdScale_SprintLerpCmp));
    install_detour_PM_UpdateSprint_SlideStateCmp((void *)(game_base_address + ADDR_PM_UpdateSprint_SlideStateCmp),
                                                 (void *)(game_base_address + ADDR_PM_UpdateSprint_SlideStateCmp_Past));
    install_detour_Jump_Start_SprintLeapCmp1((void *)(game_base_address + ADDR_Jump_Start_SprintLeapCmp1));
    install_detour_Jump_Start_SprintLeapCmp2((void *)(game_base_address + ADDR_Jump_Start_SprintLeapCmp2));
    if (debug)
        printf("movement: sprint: the hooks are in%s\n",
               aae_movement_enabled() ? "" : " (AAEMovement is off: they only show the game's own numbers)");
}
