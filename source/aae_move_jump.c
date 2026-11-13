// T7Overcharged's jumpslowdown (AAEMovement): no penalty for jumping again and again.
//
// The game makes a player pay for jumping. A landing multiplies the velocity by 0.67 (by 0.5 to 0.2 after a fall
// that does damage, which also starts a time of slow movement), the first frame on the ground after a jump
// multiplies it by 0.8 and starts a second of "slowdown" (pm_time 1000, counted down, with movement flag 18
// pending), during which the ground's friction is up to twice as strong, a jump's upward speed is lowered (its
// square divided by up to 2: half the height right after a landing) and its horizontal speed capped; and the
// camera dips on landing.
//
// The mod's T7Overcharged takes all of that away with its patch "jumpslowdown", six hooks of the Windows game that
// its console command movement_patch switches on and off (the mod switches it on; its "snail" option and a
// developer loop switch it off):
// - BG_GetViewDip, the camera's dip on landing, always 0. The landing only adds its landing event (10 to 12) when
//   there is a dip, so that goes too.
// - Jump_SlowDown, the first frame on the ground: does nothing.
// - Jump_ReduceFriction, the friction multiplier while the slowdown is pending: always 1 (forgetting the slowdown
//   once its time is over, as the game's own does).
// - PM_CrashLand, the landing: the game's, then the horizontal velocity as it was before and no vertical one.
// - Jump_Start with Jump_ClampHorizontalVelocity: the cap only for jumps started while sliding (movement flag 7).
// And the first time it switches the patch at all, which it does when it starts (all its patches off), it puts two
// nops over a conditional jump in Jump_Start: the one the game takes, at a jump while the slowdown is pending, when
// the player doesn't have gadget 13 (which spares the penalty), to work out the divisor of the jump's upward speed.
// From then on every such jump goes the gadget's way, divisor 1, whatever the switch says later: nothing ever puts
// the jump back. That part is on here as soon as T7Overcharged's movement has started.
//
// On the Mac five of the places are functions or a test of the same shape, but the landing is part of
// PM_GroundTrace and the cap part of Jump_Start, so those are detours at their begin and end (detour.h); see the
// handlers. When the switch is off every handler does exactly what the game does. The movement code runs on two
// threads at once, the server's and the client's prediction: what the landing's begin keeps is the thread's own.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aae_move_jump.h"
#include "aae_movement.h"
#include "detour.h"
#include "dobby.h"
#include "offsets.h"
#include "prepatch.h"

// T7Overcharged's sums, in its order: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Slowdown_Ms 1000 // the slowdown's time after a jump's landing

// ---- the rules

bool aae_move_jump_on(bool started, bool switched_on) {
    return started && switched_on;
}

bool aae_move_jump_full_height(bool started) {
    return started;
}

float aae_move_jump_reduce_friction(uint8_t *ps) {
    int32_t time;
    memcpy(&time, ps + Move_Ps_Time, sizeof(time));
    if (time > Slowdown_Ms) {
        uint64_t flags;
        memcpy(&flags, ps + Move_Ps_Flags, sizeof(flags));
        flags &= ~Move_Flag_SlowdownPending;
        memcpy(ps + Move_Ps_Flags, &flags, sizeof(flags));
        memset(ps + Move_Ps_JumpOriginZ, 0, sizeof(float));
    }
    return 1.0f;
}

void aae_move_jump_landing_begin(aae_move_jump_landing_t *landing, bool on, uint8_t *ps) {
    landing->ps = on ? ps : NULL;
    if (on)
        memcpy(landing->horizontal, ps + Move_Ps_Velocity, sizeof(landing->horizontal));
}

bool aae_move_jump_landing_end(aae_move_jump_landing_t *landing, uint8_t *ps) {
    uint8_t *kept_for = landing->ps;
    landing->ps = NULL;
    if (kept_for == NULL || kept_for != ps)
        return false;
    memcpy(ps + Move_Ps_Velocity, landing->horizontal, sizeof(landing->horizontal));
    memset(ps + Move_Ps_Velocity + 2 * sizeof(float), 0, sizeof(float));
    return true;
}

float aae_move_jump_speed_cap(bool on, uint64_t flags, float cap) {
    if (on && (flags & Move_Flag_Sliding) == 0)
        return INFINITY;
    return cap;
}

// ---- the game's side

#define Pm_PlayerState 0x0    // pmove_t: the player state it moves
#define Pm_CommandTime 0x8    // and its command's time (an int)
#define Pml_FallSpeed 0xa8    // pml_t: the upward speed before this move (a float)
#define Divisor_Full_Ms 901   // the game's lowered jump: from here on its upward speed's square is halved
#define Divisor_Per_Ms 0.0011098779f

static bool debug;

static bool jump_slowdown_on(void) {
    return aae_move_jump_on(aae_movement_started(), aae_movement_patch_on(Move_JumpSlowdown));
}

static float horizontal_speed(const uint8_t *ps) {
    float velocity[2];
    memcpy(velocity, ps + Move_Ps_Velocity, sizeof(velocity));
    return sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
}
static float float_at(const uint8_t *place) {
    float value;
    memcpy(&value, place, sizeof(value));
    return value;
}
static int32_t int_at(const uint8_t *place) {
    int32_t value;
    memcpy(&value, place, sizeof(value));
    return value;
}
static uint64_t flags_of(const uint8_t *ps) {
    uint64_t flags;
    memcpy(&flags, ps + Move_Ps_Flags, sizeof(flags));
    return flags;
}

// ---- what BO3MACFIX_MOVEDEBUG=1 prints: a line for each jump, landing, landing slowdown and friction of each
// player state, not one a frame. The client's prediction runs its player through the same commands again and
// again, once a frame until the server has them, so a line is printed once per player state and command.
enum { Seen_Jump, Seen_Landing, Seen_Slowdown, Seen_Friction, Seen_Kinds };
#define Seen_Remembered 8
static bool first_time(int kind, const uint8_t *ps, int32_t time) {
    static _Thread_local struct {
        const uint8_t *ps;
        int32_t time;
    } seen[Seen_Kinds][Seen_Remembered];
    static _Thread_local unsigned next[Seen_Kinds];
    for (int i = 0; i < Seen_Remembered; i++)
        if (seen[kind][i].ps == ps && seen[kind][i].time == time)
            return false;
    unsigned slot = next[kind]++ % Seen_Remembered;
    seen[kind][slot].ps = ps;
    seen[kind][slot].time = time;
    return true;
}
static unsigned thread_number(void) {
    return pthread_mach_thread_np(pthread_self());
}

// what the jump height test did in the jump going on on this thread, for its line
enum { Height_Not_Reached, Height_Full, Height_Game };
static _Thread_local int height_test;

static void say_jump(const uint8_t *pm, const uint8_t *ps, float cap, float given, bool on) {
    int test = height_test;
    height_test = Height_Not_Reached;
    int32_t time = int_at(pm + Pm_CommandTime), pm_time = int_at(ps + Move_Ps_Time);
    if (!first_time(Seen_Jump, ps, time))
        return;
    uint64_t flags = flags_of(ps);
    float up = float_at(ps + Move_Ps_Velocity + 8), from = float_at(ps + Move_Ps_Origin + 8);
    float gravity = (float)int_at(ps + Move_Ps_Gravity);
    char test_text[64], cap_text[64];
    if (test == Height_Full)
        snprintf(test_text, sizeof(test_text), "full height (T7O's permanent part)");
    else if (test == Height_Game)
        snprintf(test_text, sizeof(test_text), "the game's (divisor %.3f)",
                 pm_time < Divisor_Full_Ms ? (double)((float)pm_time * Divisor_Per_Ms + 1.0f) : 2.0);
    else
        snprintf(test_text, sizeof(test_text), "not reached");
    if (isinf(given))
        snprintf(cap_text, sizeof(cap_text), "none (the game's %.1f)", (double)cap);
    else
        snprintf(cap_text, sizeof(cap_text), "%.1f (%s)", (double)cap,
                 (flags & Move_Flag_Sliding) ? "slide jump" : "the game's");
    printf("movement: jump at %d (ps %p, thread %u): slowdown pending %d, pm_time %d, height test %s; up %.1f, peak "
           "%.1f (from %.1f), horizontal %.1f, speed cap %s; jumpslowdown %s\n",
           time, (const void *)ps, thread_number(), (flags & Move_Flag_SlowdownPending) != 0, pm_time, test_text,
           (double)up, gravity > 0.0f ? (double)(from + up * up / (2.0f * gravity)) : (double)from, (double)from,
           (double)horizontal_speed(ps), cap_text, on ? "on" : "off");
}

// what the landing's begin saw, and the camera's dip asked during the landing, for the line at its end
static _Thread_local struct {
    const uint8_t *ps;
    int32_t time;
    float fall_speed, before, fall_height;
    bool in_landing, dip_asked;
    int dip, game_dip;
} landing_seen;

// ---- the hooks

// BG_GetViewDip(ps, the fall as a height: the square of the landing's speed over twice the gravity) -> how far the
// camera dips on landing (the landing's own call, and the client's view)
install_hook_name(BG_GetViewDip, int, uint8_t *ps, float fall_height) {
    bool on = jump_slowdown_on();
    if (!debug)
        return on ? 0 : orig_BG_GetViewDip(ps, fall_height);
    // with debug lines the game's own answer is asked even while on, for the landing's line (its function only reads)
    int game = orig_BG_GetViewDip(ps, fall_height), given = on ? 0 : game;
    if (landing_seen.in_landing && landing_seen.ps == ps) {
        landing_seen.dip_asked = true;
        landing_seen.dip = given, landing_seen.game_dip = game, landing_seen.fall_height = fall_height;
    }
    return given;
}

// Jump_SlowDown(ps): PM_WalkMove's start, every frame on the ground while the slowdown is pending
install_hook_name(Jump_SlowDown, void, uint8_t *ps) {
    if (jump_slowdown_on()) {
        if (debug && first_time(Seen_Slowdown, ps, int_at(ps + Move_Ps_JumpTime)))
            printf("movement: jump at %d: landing slowdown skipped (ps %p): horizontal %.1f, pm_time %d\n",
                   int_at(ps + Move_Ps_JumpTime), (void *)ps, (double)horizontal_speed(ps), int_at(ps + Move_Ps_Time));
        return;
    }
    if (!debug) {
        orig_Jump_SlowDown(ps);
        return;
    }
    float before = horizontal_speed(ps);
    int32_t time = int_at(ps + Move_Ps_Time), jump = int_at(ps + Move_Ps_JumpTime);
    uint64_t flags = flags_of(ps);
    orig_Jump_SlowDown(ps);
    float after = horizontal_speed(ps);
    if ((after != before || time != int_at(ps + Move_Ps_Time) || flags != flags_of(ps)) &&
        first_time(Seen_Slowdown, ps, jump))
        printf("movement: jump at %d: landing slowdown (the game's, ps %p): horizontal %.1f -> %.1f, pm_time %d -> %d, "
               "pending %d -> %d\n",
               jump, (void *)ps, (double)before, (double)after, time, int_at(ps + Move_Ps_Time),
               (flags & Move_Flag_SlowdownPending) != 0, (flags_of(ps) & Move_Flag_SlowdownPending) != 0);
}

// Jump_ReduceFriction(ps) -> the multiplier of the ground's friction, every frame on the ground while the slowdown is
// pending
install_hook_name(Jump_ReduceFriction, float, uint8_t *ps) {
    bool on = jump_slowdown_on();
    int32_t time = int_at(ps + Move_Ps_Time);
    float multiplier = on ? aae_move_jump_reduce_friction(ps) : orig_Jump_ReduceFriction(ps);
    if (debug && first_time(Seen_Friction, ps, int_at(ps + Move_Ps_JumpTime)))
        printf("movement: jump at %d: friction after the landing x%.3f (%s, ps %p, pm_time %d)\n",
               int_at(ps + Move_Ps_JumpTime), (double)multiplier, on ? "T7O's" : "the game's", (void *)ps, time);
    return multiplier;
}

// ---- the detours (only from the pre-patched executable)

// what the landing's begin keeps, for its end on the same thread
static _Thread_local aae_move_jump_landing_t landing;

// "mov r12, [r12] ; mov rdi, r12" in PM_GroundTrace, where the landing begins: the player was in the air and the
// trace found ground. r12 is the pmove_t (the replaced instructions make it the player state), r13 the pml_t.
// Everything from here to the end below is the landing; it is entered nowhere else and leaves only to the end.
detour_with_registers(Landing_Begin) {
    const uint8_t *pm = (const uint8_t *)registers->r12;
    uint8_t *ps;
    memcpy(&ps, pm + Pm_PlayerState, sizeof(ps));
    aae_move_jump_landing_begin(&landing, jump_slowdown_on(), ps);
    if (debug) {
        landing_seen.ps = ps;
        landing_seen.in_landing = true, landing_seen.dip_asked = false;
        landing_seen.time = int_at(pm + Pm_CommandTime);
        landing_seen.fall_speed = float_at((const uint8_t *)registers->r13 + Pml_FallSpeed);
        landing_seen.before = horizontal_speed(ps);
    }
}

// "and byte [r15 + 0x908], 0xf9", where every way through the landing comes out; r15 is the player state
detour_with_registers(Landing_End) {
    uint8_t *ps = (uint8_t *)registers->r15;
    if (!debug) {
        aae_move_jump_landing_end(&landing, ps);
        return;
    }
    float games = horizontal_speed(ps);
    int32_t pm_time = int_at(ps + Move_Ps_Time);
    bool hard = (flags_of(ps) & (1ull << 24)) != 0;
    bool kept = aae_move_jump_landing_end(&landing, ps);
    landing_seen.in_landing = false;
    if (landing_seen.ps != ps || !first_time(Seen_Landing, ps, landing_seen.time))
        return;
    char dip_text[80];
    if (landing_seen.dip_asked)
        snprintf(dip_text, sizeof(dip_text), "dip %d (the game's %d) for a fall of %.1f", landing_seen.dip,
                 landing_seen.game_dip, (double)landing_seen.fall_height);
    else
        snprintf(dip_text, sizeof(dip_text), "no dip asked");
    printf("movement: landing at %d (ps %p, thread %u): falling at %.1f, %s; horizontal %.1f before, %.1f after the "
           "game's landing (x%.2f, pm_time %d, hard %d), %.1f now%s\n",
           landing_seen.time, (void *)ps, thread_number(), (double)landing_seen.fall_speed, dip_text,
           (double)landing_seen.before, (double)games,
           landing_seen.before > 0.0f ? (double)(games / landing_seen.before) : 1.0, pm_time, hard,
           (double)horizontal_speed(ps), kept ? " (T7O's: horizontal kept, vertical 0)" : "");
}

// "movsd xmm1, [r12 + 0x3c]" in Jump_Start, where it caps the jump's horizontal speed (the game's
// Jump_ClampHorizontalVelocity, here part of Jump_Start): the cap is in xmm0, the player state in r12, the pmove_t
// in r14. Next the game compares the speed's square with the cap's: +infinity's square is above any, not a number
// included, so the speed is left as it is.
detour_with_registers(Jump_SpeedCap) {
    uint8_t *ps = (uint8_t *)registers->r12;
    float cap, given;
    memcpy(&cap, registers->xmm[0], sizeof(cap));
    bool on = jump_slowdown_on();
    given = aae_move_jump_speed_cap(on, flags_of(ps), cap);
    memcpy(registers->xmm[0], &given, sizeof(given));
    if (debug)
        say_jump((const uint8_t *)registers->r14, ps, cap, given, on);
}

// "mov esi, 13 ; mov rdi, r15 ; call <active gadget> ; test al, al ; je" in Jump_Start: a jump started while the
// slowdown is pending and its time not over asks whether the player has gadget 13, which spares the penalty. Yes
// goes on where the divisor of the upward speed's square is 1 (the place T7Overcharged's nops send every jump to);
// the skipped test only reads.
detour_before_test(Jump_HeightTest) {
    bool full = aae_move_jump_full_height(aae_movement_started());
    if (debug)
        height_test = full ? Height_Full : Height_Game;
    return full;
}

void aae_move_jump_init(uint64_t game_base_address) {
    if (!aae_movement_enabled())
        return;
    const char *env = getenv("BO3MACFIX_MOVEDEBUG");
    debug = env != NULL && atoi(env) != 0;
    static const uint32_t places[] = { ADDR_BG_GetViewDip, ADDR_Jump_ReduceFriction, ADDR_Jump_SlowDown,
                                       ADDR_Jump_Start_ClampSpeed, ADDR_Jump_Start_SlowdownTest,
                                       ADDR_PM_GroundTrace_CrashLandBegin, ADDR_PM_GroundTrace_CrashLandEnd };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        if (!prepatch_has_hook(places[i])) {
            printf("movement: not running from the pre-patched executable: jumping again and again costs speed and "
                   "height as in the game without the mod\n");
            return;
        }
    }
    uint8_t *base = (uint8_t *)game_base_address;
    install_hook_BG_GetViewDip(base + ADDR_BG_GetViewDip);
    install_hook_Jump_SlowDown(base + ADDR_Jump_SlowDown);
    install_hook_Jump_ReduceFriction(base + ADDR_Jump_ReduceFriction);
    install_detour_Landing_End(base + ADDR_PM_GroundTrace_CrashLandEnd);
    install_detour_Landing_Begin(base + ADDR_PM_GroundTrace_CrashLandBegin);
    install_detour_Jump_SpeedCap(base + ADDR_Jump_Start_ClampSpeed);
    install_detour_Jump_HeightTest(base + ADDR_Jump_Start_SlowdownTest, base + ADDR_Jump_Start_SlowdownTest_Yes);
    if (debug)
        printf("movement: jump: hooks in (view dip, landing slowdown, friction, the landing, the jump's speed cap and "
               "height test)\n");
}
