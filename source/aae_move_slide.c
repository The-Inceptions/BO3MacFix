// T7Overcharged's slide changes (AAEMovement): what All-around Enhancement's build of T7Overcharged does to slides
// on Windows, as the mod's commands movement_patch and movement_patch_all switch them (aae_movement.h).
//
// slide_preserve_momentum. While a player slides, the game pushes them along after each move's acceleration
// (PM_DoSlideAdjustments): on a slide surface towards the wish speed of a dvar (450) with an acceleration of 2, in its
// other case (the movement flag 51 and the move's int at +0x64) by a mix of the move's direction and the velocity with
// a wish speed of 1000 and an acceleration of 0.25; then it caps the speed by another dvar, which it compares with the
// speed squared, so that with its default of 500 the cap never acts. T7Overcharged replaces the whole function: on a
// slide surface a wish speed of 300; the other push in steps of double precision, with 0.333 and 0.0066666668 as
// doubles where the game has floats; no cap, but after the other push the velocity from before it is put back if the
// speed has gone above 90000 (it never does). Both call the game's PM_Accelerate at its address. Here the function is
// hooked, and with the patch on T7Overcharged's function runs instead of the game's, with every conversion between
// float and double where T7Overcharged has one. Compared bit for bit with T7Overcharged's own code under Wine
// (wip/agents/movec/wine, not in the repository): what that gave is in test_aae_move_slide.c as a checksum.
//
// slideslowdown. After a slide, while the player's movement state is 3, the game ramps the speed scale up from almost
// nothing over a time the weapon gives (PM_CmdScaleImpl: ((1 - f) * 0.01 + f) squared, f the part of that time gone
// by), and on a slide surface PM_Friction adds friction of 1.5 times the speed. T7Overcharged makes the jump that skips
// the ramp unconditional and makes the 1.5 read 0.0.
//
// instant_slide. A slide the player asks for in the air starts on landing only at a horizontal speed of 150 or more
// (the game's test, which the landing also asks to shorten itself). T7Overcharged makes the jump past that comparison
// unconditional. A slide started on the ground (crouch while sprinting) never asks this test: it needs a sprint and a
// speed of 100, which T7Overcharged leaves as they are.
//
// slideangles. When a slide starts with the view looking down more than 10.05 degrees, the game pulls it to 10 degrees
// over half a second; while sliding it clamps the view's pitch (PM_ClampViewAngles with the limit 20). T7Overcharged
// makes the jump past the pull unconditional and leaves out every clamp with the limit 20, which only the sliding one
// has.
//
// Each change acts only while T7Overcharged's movement has started and the change's patch is on; otherwise every place
// gives the game's own result. All of it only from the pre-patched executable (detour.h).
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "aae_move_slide.h"
#include "aae_movement.h"
#include "detour.h"
#include "dobby.h"
#include "offsets.h"
#include "prepatch.h"

// the sums below are T7Overcharged's, in its order: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

// T7Overcharged's constants. The two doubles are not the game's floats widened: (float) of each is the game's.
#define Surface_Wish_Speed 300.0f
#define Surface_Acceleration 2.0f
#define Flagged_Wish_Speed 1000.0f
#define Flagged_Acceleration 0.25f
#define Flagged_Third 0x1.54fdf3b645a1dp-2         // 0.333
#define Flagged_Velocity_Share 0x1.b4e81be11bb42p-8 // 0.0066666668
#define Undo_Above 90000.0f

#define Flag_Sliding 0x80ull
#define Flag_Sprinting 0x20ull
#define Flag_16 (1ull << 16)          // refuses a slide like the next one (not followed)
#define Flag_18 (1ull << 18)          // a sprint younger than 200 ms may slide with it
#define Flag_No_Slide (1ull << 40)    // what a script's allowslide(false) sets (the builtin at 0x100dad972)
#define Flag_Slide_Asked (1ull << 46) // a slide was asked for in the air: on landing the speed test decides
#define Flag_Other_Push (1ull << 51)
#define No_Ground 0x3ff
#define Sprint_Least_Ms 200
#define Slide_Least_Speed 100.0f

// Which NaN comes out of an addition or multiplication of two floats that both aren't numbers: on x86 the first
// operand's, made quiet (and a single one, made quiet). T7Overcharged's instructions have their operands in a fixed
// order, but a C compiler may swap them, so where both can be NaNs the rule is applied here.
static bool not_a_number(float value) {
    return value != value;
}
static float quiet(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    bits |= 0x400000;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static float add(float first, float second) {
    return not_a_number(first) ? quiet(first) : not_a_number(second) ? quiet(second) : first + second;
}
static float multiply(float first, float second) {
    return not_a_number(first) ? quiet(first) : not_a_number(second) ? quiet(second) : first * second;
}

// T7Overcharged's length of the move's two floats (1 for none) and one over it. A length that isn't a number fails
// the comparison and stays.
static float inverse_length(float x, float y) {
    float length = sqrtf(add(y * y, x * x) + 0.0f);
    if (-length >= 0.0f)
        length = 1.0f;
    return (float)(1.0 / (double)length);
}

aae_move_slide_push_kind aae_move_slide_push(bool slide_surface, uint64_t flags, int32_t flagged_move,
                                             const float along[2], const float velocity[3],
                                             aae_move_slide_push_t *push) {
    float x = along[0], y = along[1];
    if (slide_surface) {
        float inverse = inverse_length(x, y);
        push->direction[0] = multiply(inverse, x);
        push->direction[1] = multiply(inverse, y);
        push->direction[2] = (float)((double)inverse * 0.0);
        push->wish_speed = Surface_Wish_Speed;
        push->acceleration = Surface_Acceleration;
        return Slide_Push_Surface;
    }
    if ((flags & Flag_Other_Push) == 0 || flagged_move == 0)
        return Slide_Push_None;
    float inverse = inverse_length(x, y);
    // the up part: what the game has as (0 * 2 + 0) * 0.333, every step rounded to a float
    float up = (float)((double)inverse * 0.0);
    up = (float)((double)up + (double)up);
    up = (float)((double)up + 0.0);
    push->direction[2] = (float)((double)up * Flagged_Third);
    float along_x = multiply(inverse, x), along_y = multiply(inverse, y);
    float twice_x = (float)((double)along_x + (double)along_x), twice_y = (float)((double)along_y + (double)along_y);
    float sum_x = add(twice_x, (float)((double)velocity[0] * Flagged_Velocity_Share));
    float sum_y = add(twice_y, (float)((double)velocity[1] * Flagged_Velocity_Share));
    push->direction[0] = (float)((double)sum_x * Flagged_Third);
    push->direction[1] = (float)((double)sum_y * Flagged_Third);
    push->wish_speed = Flagged_Wish_Speed;
    push->acceleration = Flagged_Acceleration;
    return Slide_Push_Flagged;
}

bool aae_move_slide_push_undone(const float velocity[3]) {
    float speed = sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
    return speed > Undo_Above;
}

uint32_t aae_move_slide_ramp_length(bool on, uint32_t length) {
    // the game's "time >= length" (a signed compare) holds for every time when the length is the least int
    return on ? 0x80000000u : length;
}

float aae_move_slide_surface_friction(bool on, float factor) {
    return on ? 0.0f : factor;
}

float aae_move_slide_least_speed(bool on, float least) {
    // "least > speed" fails for every speed, also for one that isn't a number
    return on ? -INFINITY : least;
}

float aae_move_slide_start_pitch(bool on, float pitch) {
    // "pitch > 10.05" fails
    return on ? -INFINITY : pitch;
}

bool aae_move_slide_clamp_left_out(bool on) {
    return on;
}

aae_move_slide_answer aae_move_slide_answer_of(const aae_move_slide_asked_t *asked) {
    uint64_t flags = asked->flags;
    if ((flags & Flag_Sliding) != 0)
        return Slide_Answer_Sliding;
    if ((flags & (Flag_16 | Flag_No_Slide)) != 0)
        return Slide_Answer_Not_Allowed;
    if (asked->slide_time > asked->server_time)
        return Slide_Answer_Too_Soon;
    if (asked->ground_entity == No_Ground)
        return Slide_Answer_In_Air;
    if (asked->ground_plane == 0)
        return Slide_Answer_No_Ground;
    if ((flags & Flag_Slide_Asked) != 0)
        return Slide_Answer_Landing;
    if (!asked->slide_button)
        return Slide_Answer_No_Button;
    if ((flags & Flag_Sprinting) == 0)
        return Slide_Answer_Not_Sprinting;
    int32_t into_sprint = (int32_t)((uint32_t)asked->server_time - (uint32_t)asked->sprint_start);
    if (into_sprint < Sprint_Least_Ms && (flags & Flag_18) == 0)
        return Slide_Answer_Sprint_Too_New;
    // the game's order of sums; a speed that isn't a number passes, as the game's comparison lets it
    const float *v = asked->velocity;
    float speed = sqrtf(v[2] * v[2] + (v[1] * v[1] + v[0] * v[0]));
    if (Slide_Least_Speed > speed)
        return Slide_Answer_Too_Slow;
    return Slide_Answer_Later;
}

// ---- the game's side

// the player state and the move (pml_t), at the same places in both games
#define Ps_Flags 0x10        // uint64_t movement flags
#define Ps_Velocity 0x3c     // three floats
#define Ps_View_Pitch 0x318  // the view's pitch, then its yaw
#define Ps_Slide_Time 0x844  // int: when the last slide started, and then when it ended
#define Pml_Along 0x40       // two floats the slide push goes along
#define Pml_Flags 0x54       // 2: on a slide surface
#define Pml_Flagged 0x64     // int: the other push needs it non-zero
#define Pull_From_Pitch 10.05f
#define Clamp_Limit 20.0f
#define Ramp_Floor 0.01f     // the ramp's factor at its start is this squared
#define Cg_PlayerState 0x11a8d0 // cg_t, the local client's: its predicted player state
#define Ps_Ground_Entity 0x100 // int: what the player stands on
#define Ps_Weapon_State 0x8c   // int: most likely the weapon's state; the speed ramp after a slide needs 3
#define Ps_Sprint_Start 0x750  // int: when the sprint began
#define Ps_Slide_Count 0x84c   // int: slides in a row; the game starts no 15th
#define Pm_Server_Time 0x8     // the move's command: its time,
#define Pm_Crouch_Byte 0xe     // its crouch button (this byte's 0x40, button word 0's 0x400000),
#define Pm_Slide_Byte 0x13     // and its slide button (this byte's 1, button word 1's 0x1000000)
#define Pml_Ground_Plane 0x30

static void (*PM_Accelerate)(uint8_t *ps, uint8_t *pml, const float *direction, float wish_speed, float acceleration);

static bool on(move_patch patch) {
    return aae_movement_started() && aae_movement_patch_on(patch);
}

static float horizontal_speed(const uint8_t *ps) {
    float velocity[2];
    memcpy(velocity, ps + Ps_Velocity, sizeof(velocity));
    return sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
}

// BO3MACFIX_MOVEDEBUG=1: a line when a change acts or the game's own rule decides at one of the places, once per slide
// and thread (the server's movement and the client's prediction run on two threads and both come by), and what the
// first local client's player does, from a thread of its own (watch, further down)
static int debug;
static int (*say)(const char *format, ...) __attribute__((format(printf, 1, 2))) = printf;
static uint64_t now_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}
// whether this place has had its line for this slide on this thread
static bool new_slide(int place, const uint8_t *ps) {
    static _Thread_local struct {
        const uint8_t *ps;
        int32_t time;
    } said[6];
    int32_t time;
    memcpy(&time, ps + Ps_Slide_Time, sizeof(time));
    if (said[place].ps == ps && said[place].time == time)
        return false;
    said[place].ps = ps, said[place].time = time;
    return true;
}

// PM_DoSlideAdjustments(playerState_s *, pml_t *)
install_hook_name(PM_DoSlideAdjustments, void, uint8_t *ps, uint8_t *pml) {
    if (!on(Move_SlidePreserveMomentum)) {
        orig_PM_DoSlideAdjustments(ps, pml);
        return;
    }
    uint64_t flags;
    int32_t flagged;
    float along[2], before[3], after[3];
    aae_move_slide_push_t push;
    memcpy(&flags, ps + Ps_Flags, sizeof(flags));
    memcpy(&flagged, pml + Pml_Flagged, sizeof(flagged));
    memcpy(along, pml + Pml_Along, sizeof(along));
    memcpy(before, ps + Ps_Velocity, sizeof(before));
    aae_move_slide_push_kind kind =
        aae_move_slide_push((pml[Pml_Flags] & 2) != 0, flags, flagged, along, before, &push);
    if (kind == Slide_Push_None)
        return;
    PM_Accelerate(ps, pml, push.direction, push.wish_speed, push.acceleration);
    memcpy(after, ps + Ps_Velocity, sizeof(after));
    bool undone = kind == Slide_Push_Flagged && aae_move_slide_push_undone(after);
    if (undone)
        memcpy(ps + Ps_Velocity, before, sizeof(before));
    if (debug && new_slide(0, ps))
        say("movement: slide_preserve_momentum: T7Overcharged's push %s, wish speed %.0f, acceleration %.2f: "
            "velocity %.1f %.1f %.1f before, %.1f %.1f %.1f after%s\n",
            kind == Slide_Push_Surface ? "on a slide surface" : "of flag 51", push.wish_speed, push.acceleration,
            before[0], before[1], before[2], after[0], after[1], after[2], undone ? ", put back" : "");
}

// PM_CmdScaleImpl, "sub ecx, [rdx + 0x844] ; cmp ecx, eax ; jge": in the movement state after a slide, the time since
// the slide ended (serverTime in ecx, less the end in the player state rdx) against the ramp's length in eax; when the
// ramp isn't over, the speed scale is multiplied by its factor. eax is written again right behind the jump.
detour_with_registers(Slide_RampCompare) {
    uint32_t length = (uint32_t)registers->rax;
    bool slowdown = on(Move_SlideSlowdown);
    uint32_t seen = aae_move_slide_ramp_length(slowdown, length);
    if (seen != length)
        registers->rax = seen;
    const uint8_t *ps = (const uint8_t *)registers->rdx;
    int32_t end, since;
    memcpy(&end, ps + Ps_Slide_Time, sizeof(end));
    since = (int32_t)((uint32_t)registers->rcx - (uint32_t)end);
    if (debug && since < (int32_t)length && new_slide(1, ps)) {
        float part = (float)since / (float)(int32_t)length, factor = (1.0f - part) * Ramp_Floor + part;
        say("movement: %s speed ramp after a slide, %d of %d ms gone, factor %.4f%s\n",
            slowdown ? "slideslowdown: no" : "the game's", since, (int32_t)length, factor * factor,
            slowdown ? " (1 now)" : "");
    }
}

// PM_Friction, "mulss xmm0, xmm2 ; mulss xmm0, [r14 + 0x24]": on a slide surface the extra friction, its factor (1.5)
// just loaded into xmm0, times the speed in xmm2 and the frame's time, is about to be added to the drop. rbx is the
// player state.
detour_with_registers(Slide_SurfaceFriction) {
    float factor, speed;
    memcpy(&factor, registers->xmm[0], sizeof(factor));
    memcpy(&speed, registers->xmm[2], sizeof(speed));
    bool slowdown = on(Move_SlideSlowdown);
    float seen = aae_move_slide_surface_friction(slowdown, factor);
    memcpy(registers->xmm[0], &seen, sizeof(seen));
    if (debug && new_slide(2, (const uint8_t *)registers->rbx))
        say("movement: %s extra friction on a slide surface, %.2f times the speed %.1f\n",
            slowdown ? "slideslowdown: no" : "the game's", factor, speed);
}

// the test whether a slide may start (0x100e8148b), "ucomiss xmm0, xmm2 ; jbe": the horizontal speed in xmm2 below
// the least speed in xmm0 (150) is too slow. rbx is the player state.
detour_with_registers(Slide_SpeedTest) {
    float least, speed;
    memcpy(&least, registers->xmm[0], sizeof(least));
    memcpy(&speed, registers->xmm[2], sizeof(speed));
    bool instant = on(Move_InstantSlide);
    float seen = aae_move_slide_least_speed(instant, least);
    memcpy(registers->xmm[0], &seen, sizeof(seen));
    if (debug && least > speed) {
        // the test can come every frame while a slide is asked for: a line at most every half second
        static _Thread_local uint64_t said;
        uint64_t now = now_ms();
        if (now - said >= 500) {
            said = now;
            say("movement: %s a slide at a horizontal speed of %.1f (the game needs %.0f)\n",
                instant ? "instant_slide: lets" : "the game refuses", speed, least);
        }
    }
}

// a slide's start (0x100e82086), "movss xmm0, [rax + 0x318]": the view's pitch, which it compares with 10.05 next to
// pull the view to 10 degrees when it is more. rax is the player state, and dead behind the comparison.
detour_with_registers(Slide_StartPitch) {
    // the pitch the comparison is to see instead, where the instruction reads it
    static _Thread_local float compared;
    const uint8_t *ps = (const uint8_t *)registers->rax;
    float pitch;
    memcpy(&pitch, ps + Ps_View_Pitch, sizeof(pitch));
    bool angles = on(Move_SlideAngles);
    float seen = aae_move_slide_start_pitch(angles, pitch);
    if (memcmp(&seen, &pitch, sizeof(seen)) != 0) {
        compared = seen;
        registers->rax = (uint64_t)(uintptr_t)&compared - Ps_View_Pitch;
    }
    if (debug) {
        uint64_t flags;
        memcpy(&flags, ps + Ps_Flags, sizeof(flags));
        say("movement: a slide starts (flag 7 %s), horizontal speed %.1f, view pitch %.1f%s\n",
            (flags & Flag_Sliding) != 0 ? "set" : "clear", horizontal_speed(ps), pitch,
            !(pitch > Pull_From_Pitch) ? ""
            : angles                   ? ": slideangles leaves it (the game would pull it to 10)"
                                       : ": the game pulls it to 10");
    }
}

// PM_UpdateViewAngles, "mov edx, 20" before PM_ClampViewAngles(ps, cmd, 20), the clamp of the view's pitch while
// sliding. r15 is the player state, r14 the command; behind the call (the other place) rax is loaded anew.
detour_choice(Slide_PitchClamp, "mov %r15, %rdi\n mov %r14, %rsi\n") {
    const uint8_t *ps = (const uint8_t *)first;
    (void)second;
    bool left_out = aae_move_slide_clamp_left_out(on(Move_SlideAngles));
    if (debug) {
        float pitch;
        memcpy(&pitch, ps + Ps_View_Pitch, sizeof(pitch));
        // once per slide, and once more when the pitch is past the game's limit
        static _Thread_local bool past;
        bool fresh = new_slide(3, ps);
        if (fresh)
            past = false;
        bool now_past = fabsf(pitch) > Clamp_Limit;
        if (fresh || (now_past && !past))
            say("movement: %s while sliding, view pitch %.1f\n",
                left_out ? "slideangles: no 20 degree pitch clamp" : "the game clamps the view pitch to 20",
                pitch);
        past = past || now_past;
    }
    return left_out;
}

// ---- the game's slide test, looked at (BO3MACFIX_MOVEDEBUG=1 only)
//
// The slide function, (pmove_t *, pml_t *), once a move on each thread: hooked only for the debug line, and always
// called as it is. When the command's slide button or crouch button goes down for a player, a line says what the game's
// slide test saw then and what it answered, and whether a slide started.

static uint8_t *const *cg_clients; // the game's pointer to the local clients' cg_t

static const char *answer_text(aae_move_slide_answer answer) {
    switch (answer) {
    case Slide_Answer_Sliding: return "already sliding";
    case Slide_Answer_Not_Allowed: return "no: not allowed (flag 16 or 40; 40 is a script's allowslide(false))";
    case Slide_Answer_Too_Soon: return "no: the last slide's time is still ahead";
    case Slide_Answer_In_Air: return "in the air: with the slide button down the game asks for a slide on landing";
    case Slide_Answer_No_Ground: return "no: not on a ground plane";
    case Slide_Answer_Landing: return "a slide asked for in the air: the landing's test of 150 units/s decides";
    case Slide_Answer_No_Button: return "no: the slide button isn't down";
    case Slide_Answer_Not_Sprinting: return "no: not sprinting";
    case Slide_Answer_Sprint_Too_New: return "no: under 200 ms into the sprint";
    case Slide_Answer_Too_Slow: return "no: under 100 units/s";
    case Slide_Answer_Later: return "button, sprint and speed are right: the game's later tests decide";
    }
    return "?";
}

// whether a button went down for this player on this thread (each thread sees its own players: the server's or the
// client's predicted one)
static bool newly_down(const uint8_t *ps, bool slide, bool crouch, bool *slide_new, bool *crouch_new) {
    static _Thread_local struct {
        const uint8_t *ps;
        bool slide, crouch;
    } seen[4];
    static _Thread_local unsigned next;
    unsigned at = 4;
    for (unsigned i = 0; i < 4; i++)
        if (seen[i].ps == ps)
            at = i;
    if (at == 4) {
        at = next++ % 4;
        seen[at].ps = ps, seen[at].slide = false, seen[at].crouch = false;
    }
    *slide_new = slide && !seen[at].slide;
    *crouch_new = crouch && !seen[at].crouch;
    seen[at].slide = slide, seen[at].crouch = crouch;
    return *slide_new || *crouch_new;
}

// the slide function (pmove_t *, pml_t *): what its test saw, read before it runs; whether it started a slide, after
install_hook_name(PM_UpdateSlide, void, uint8_t *pm, uint8_t *pml) {
    uint8_t *ps;
    memcpy(&ps, pm, sizeof(ps));
    aae_move_slide_asked_t asked;
    memcpy(&asked.flags, ps + Ps_Flags, sizeof(asked.flags));
    memcpy(&asked.server_time, pm + Pm_Server_Time, sizeof(asked.server_time));
    memcpy(&asked.slide_time, ps + Ps_Slide_Time, sizeof(asked.slide_time));
    memcpy(&asked.ground_entity, ps + Ps_Ground_Entity, sizeof(asked.ground_entity));
    memcpy(&asked.ground_plane, pml + Pml_Ground_Plane, sizeof(asked.ground_plane));
    memcpy(&asked.sprint_start, ps + Ps_Sprint_Start, sizeof(asked.sprint_start));
    memcpy(asked.velocity, ps + Ps_Velocity, sizeof(asked.velocity));
    asked.slide_button = (pm[Pm_Slide_Byte] & 1) != 0;
    bool crouch = (pm[Pm_Crouch_Byte] & 0x40) != 0;
    int32_t count;
    memcpy(&count, ps + Ps_Slide_Count, sizeof(count));
    orig_PM_UpdateSlide(pm, pml);
    bool slide_new, crouch_new;
    if (!newly_down(ps, asked.slide_button, crouch, &slide_new, &crouch_new))
        return;
    uint64_t after;
    memcpy(&after, ps + Ps_Flags, sizeof(after));
    const uint8_t *cg = *cg_clients;
    bool client = cg != NULL && ps == cg + Cg_PlayerState;
    const char *pressed = slide_new ? (crouch ? "the slide button down (crouch down too)"
                                              : "the slide button down (crouch up)")
                                    : (asked.slide_button ? "the crouch button down (slide down too)"
                                                          : "the crouch button down (slide up)");
    char sprint[48] = "not sprinting";
    if ((asked.flags & Flag_Sprinting) != 0)
        snprintf(sprint, sizeof(sprint), "sprinting for %d ms",
                 (int32_t)((uint32_t)asked.server_time - (uint32_t)asked.sprint_start));
    const char *outcome = (after & ~asked.flags & Flag_Sliding) != 0       ? "a slide started"
                          : (after & ~asked.flags & Flag_Slide_Asked) != 0 ? "a slide asked for on landing (flag 46)"
                                                                           : "no slide";
    const float *v = asked.velocity;
    say("movement: slide test (%s): %s; %s, horizontal speed %.1f, flags 16 %d 18 %d 40 %d 46 %d, slide count %d: "
        "%s; %s\n", client ? "client" : "server", pressed, sprint, sqrtf(v[0] * v[0] + v[1] * v[1]),
        (asked.flags & Flag_16) != 0, (asked.flags & Flag_18) != 0, (asked.flags & Flag_No_Slide) != 0,
        (asked.flags & Flag_Slide_Asked) != 0, count, answer_text(aae_move_slide_answer_of(&asked)), outcome);
}

// ---- what the first local client's player does (BO3MACFIX_MOVEDEBUG=1)
//
// Its predicted player state, looked at every 5 ms from a thread of its own, whatever the setting: so a run with
// AAEMovement off shows the game's own numbers by the same lines. It only reads, and through the kernel, which says no
// where a plain read would fault: the game frees the cg_t when it shuts down.

typedef struct {
    bool sliding, asked, after;
    int half_seen, after_seen;
    int32_t state_at_end, state_after;
    uint64_t start, end;
    float speed_before, speed_at_start, pitch_at_start, pitch_half, pitch_low, pitch_high, speed_after[3];
} watched_t;

// one look at the player: a line when a slide is asked for in the air, when one begins and ends, and the horizontal
// speed 0.1, 0.25 and 0.5 s after it ended, with the weapon state that decides whether the game's speed ramp after a
// slide is reached at all
static void watch_step(watched_t *w, uint64_t now, uint64_t flags, const float velocity[3], float pitch,
                       int32_t state) {
    static const uint64_t after_ms[3] = { 100, 250, 500 };
    float speed = sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
    bool sliding = (flags & Flag_Sliding) != 0, asked = (flags & Flag_Slide_Asked) != 0;
    if (asked && !w->asked)
        say("movement: seen: a slide asked for in the air (flag 46), horizontal speed %.1f\n", speed);
    w->asked = asked;
    if (sliding && !w->sliding) {
        w->start = now, w->half_seen = 0, w->after = false;
        w->speed_at_start = speed, w->pitch_at_start = w->pitch_low = w->pitch_high = pitch;
        say("movement: seen: a slide begins, horizontal speed %.1f before it, %.1f at its start, view pitch %.1f\n",
            w->speed_before, speed, pitch);
    } else if (sliding) {
        w->pitch_low = fminf(w->pitch_low, pitch), w->pitch_high = fmaxf(w->pitch_high, pitch);
        if (!w->half_seen && now - w->start >= 500)
            w->pitch_half = pitch, w->half_seen = 1;
    } else if (w->sliding) {
        char half[32] = "none";
        if (w->half_seen)
            snprintf(half, sizeof(half), "%.1f", w->pitch_half);
        say("movement: seen: the slide ended after %llu ms: view pitch %.1f at its start, %s at 0.5 s, %.1f to %.1f "
            "in all; horizontal speed %.1f at its start\n", (unsigned long long)(now - w->start), w->pitch_at_start,
            half, w->pitch_low, w->pitch_high, w->speed_at_start);
        w->end = now, w->after = true, w->after_seen = 0, w->state_at_end = state;
    } else if (w->after) {
        while (w->after_seen < 3 && now - w->end >= after_ms[w->after_seen]) {
            if (w->after_seen == 0)
                w->state_after = state;
            w->speed_after[w->after_seen++] = speed;
        }
        if (w->after_seen == 3) {
            say("movement: seen: horizontal speed %.1f, %.1f and %.1f at 0.1, 0.25 and 0.5 s after the slide; weapon "
                "state %d at its end, %d at 0.1 s (the speed ramp after a slide is the game's in state 3)\n",
                w->speed_after[0], w->speed_after[1], w->speed_after[2], w->state_at_end, w->state_after);
            w->after = false;
        }
    }
    w->sliding = sliding;
    if (!sliding)
        w->speed_before = speed;
}

static bool read_game(const uint8_t *from, void *into, size_t size) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)from, size,
                                  (mach_vm_address_t)(uintptr_t)into, &got) == KERN_SUCCESS && got == size;
}

// what starts the watcher's thread (a test gives its own)
static int (*start_thread)(pthread_t *thread, const pthread_attr_t *attributes, void *(*body)(void *),
                           void *argument) = pthread_create;

static void *watch(void *unused) {
    (void)unused;
    watched_t watched = { 0 };
    uint8_t ps[Ps_View_Pitch + 4];
    for (;;) {
        usleep(5000);
        const uint8_t *cg;
        if (!read_game((const uint8_t *)cg_clients, &cg, sizeof(cg)) || cg == NULL ||
            !read_game(cg + Cg_PlayerState, ps, sizeof(ps)))
            continue;
        uint64_t flags;
        float velocity[3], pitch;
        int32_t state;
        memcpy(&flags, ps + Ps_Flags, sizeof(flags));
        memcpy(velocity, ps + Ps_Velocity, sizeof(velocity));
        memcpy(&pitch, ps + Ps_View_Pitch, sizeof(pitch));
        memcpy(&state, ps + Ps_Weapon_State, sizeof(state));
        watch_step(&watched, now_ms(), flags, velocity, pitch, state);
    }
    return NULL;
}

void aae_move_slide_init(uint64_t game_base_address) {
    const char *env = getenv("BO3MACFIX_MOVEDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    cg_clients = (uint8_t *const *)(game_base_address + ADDR_cgArray);
    if (debug) {
        pthread_t thread;
        if (start_thread(&thread, NULL, watch, NULL) == 0)
            pthread_detach(thread);
    }
    if (!aae_movement_enabled()) {
        if (debug)
            say("movement: AAEMovement is 0: nothing of T7Overcharged's slide changes is hooked\n");
        return;
    }
    if (!prepatch_has_hook(ADDR_PM_DoSlideAdjustments) || !prepatch_has_hook(ADDR_PM_CmdScale_SlideRampCmp) ||
        !prepatch_has_hook(ADDR_PM_Friction_SlideFriction) || !prepatch_has_hook(ADDR_CanSlide_SpeedTest) ||
        !prepatch_has_hook(ADDR_StartSlide_PitchLoad) || !prepatch_has_hook(ADDR_PM_UpdateViewAngles_SlideClamp)) {
        say("movement: not running from the pre-patched executable: slides stay as the game has them, without "
            "T7Overcharged's changes\n");
        return;
    }
    PM_Accelerate = (void *)(game_base_address + ADDR_PM_Accelerate);
    install_hook_PM_DoSlideAdjustments((void *)(game_base_address + ADDR_PM_DoSlideAdjustments));
    install_detour_Slide_RampCompare((void *)(game_base_address + ADDR_PM_CmdScale_SlideRampCmp));
    install_detour_Slide_SurfaceFriction((void *)(game_base_address + ADDR_PM_Friction_SlideFriction));
    install_detour_Slide_SpeedTest((void *)(game_base_address + ADDR_CanSlide_SpeedTest));
    install_detour_Slide_StartPitch((void *)(game_base_address + ADDR_StartSlide_PitchLoad));
    install_detour_Slide_PitchClamp((void *)(game_base_address + ADDR_PM_UpdateViewAngles_SlideClamp),
                                    (void *)(game_base_address + ADDR_PM_UpdateViewAngles_SlideClampDone));
    if (!debug)
        return;
    say("movement: T7Overcharged's slide changes are hooked (slide_preserve_momentum, slideslowdown, instant_slide, "
        "slideangles)\n");
    // only for the debug line: the game's own slide test, looked at
    if (prepatch_has_hook(ADDR_PM_UpdateSlide)) {
        install_hook_PM_UpdateSlide((void *)(game_base_address + ADDR_PM_UpdateSlide));
        say("movement: the game's slide test is looked at (a line when the slide or crouch button goes down)\n");
    }
}
