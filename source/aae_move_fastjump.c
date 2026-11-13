// T7Overcharged's movement patch "fastjump" (AAEMovement): the next jump at once, more control in the air at low
// speed, a little faster acceleration on the ground and the speed kept through a landing.
//
// All-around Enhancement loads its own build of T7Overcharged (T7O below), and in every ordinary game it switches
// this patch on ("movement_patch fastjump 1", T7O 0x1800f25f0). On Windows that changes four things:
// - PM_CheckJump refuses a jump within 500 ms of the last one. T7O writes 1 over the 500 of its "cmp eax, 500"
//   (Windows 0x1425fce51). Here the same compare (0x1014dd451, "cmp ecx, 500 ; jl <no jump>", ecx the command's
//   time minus the player state's jump time) is detoured: a time of 1 to 499 ms is shown to it as 500, so that only
//   one below 1 ms is refused, as by T7O's "cmp eax, 1".
//   (A jump also needs a fresh press of the button, on both games: one held since the last command never jumps.)
// - PM_Accelerate(ps, pml, wishdir, wishspeed, accel): T7O wraps it and looks at who calls. From PM_AirMove's first
//   call, while the horizontal speed is below 150 and the player isn't sliding (bit 7 of the movement flags), the
//   acceleration is ×4 and the wish speed +170; from PM_WalkMove's first call the acceleration is ×1.1111112 (the
//   usual 9 becomes 10); any other caller gets the game's own. Here the function is hooked and the caller is the
//   hook's return address: the pre-patched executable's stub only jumps, so a handler returns to where the game
//   called. BO3MacFix's own slide code (aae_move_slide.c) calls PM_Accelerate at its address as well: it is neither
//   caller and gets nothing changed, as T7O's own slide code on Windows. T7O hands the game a copy of wishdir; so
//   does this while the patch is on.
// - PM_Friction(pml, pm): T7O wraps it to note, whenever the game's PM_IsInAir says the player is in the air, the
//   time (GetTickCount64) by the player state's address, before the game's friction runs. And it replaces
//   BG_GetFriction (the game's constant 5.5) at PM_Friction's main call with 0 while the player isn't sliding, the
//   horizontal speed is 100 or more (its square compared with 10000) and the last time in the air is at most 67 ms
//   ago: the speed is kept through a landing. Here PM_Friction is hooked for the first, and the place right behind
//   that call (0x10113c500: the friction in xmm0, the player state in rbx) is detoured for the second.
// - T7O's fifth hook, on PM_AirMove, only keeps the pmove for its BG_GetFriction (its "g_pm"); here the friction's
//   place has the player state in a register.
//
// What is different here, on purpose:
// - The clock. T7O's GetTickCount64 moves in steps of the system's timer, usually 15.6 ms, so the 67 ms after the
//   last move in the air end with the fifth step after it: 62.5 to 78 ms later (at 60 frames a second that leaves 3
//   or 4 moves on the ground without friction). Here it is a clock in milliseconds: 67 to 68 ms (4 moves at 60
//   frames). Like T7O this takes the difference of the two times unsigned: a time ahead of the clock counts as long
//   ago.
// - The server's and the client's movement run on two threads at once. T7O's g_pm and its map of times are shared by
//   both without a lock, so a friction could be decided with the other thread's player state. Here it is always this
//   call's own player state, and the times are kept under a lock.
// - T7O keeps a time for every player state it ever saw. Here they are 64, and the one seen longest ago makes room
//   for a new one. A time more than 67 ms ago does exactly what no time does, so this only differs when more than
//   64 player states were in the air within 67 ms; the game moves a handful.
//
// Every handler asks each time it runs whether the patch is on (aae_movement.h) and does what the game does when it
// isn't: T7O enables and disables its hooks instead. Only from the pre-patched executable, like every detour inside
// a function.
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aae_move_fastjump.h"
#include "aae_movement.h"
#include "detour.h"
#include "dobby.h"
#include "offsets.h"
#include "prepatch.h"

// the sums below are T7Overcharged's, in its order: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Ps_Movement_Flags 0x10 // the player state's movement flags (64 bits); bit 7 is set while sliding
#define Ps_Velocity 0x3c       // its velocity, three floats
#define Ps_Jump_Time 0x110     // the server time of its last jump (int), which Jump_Start sets
#define Pm_Server_Time 0x8     // the pmove's command: its server time (int)

#define Air_Control_Below 150.0f          // the horizontal speed below which the air control is raised
#define Air_Acceleration_Factor 4.0f
#define Air_Wish_Speed_Added 170.0f
#define Ground_Acceleration_Factor 1.1111112f // T7O's float 0x3f8e38e4
#define No_Friction_From 10000.0f         // the square of the horizontal speed from which a landing keeps it
#define No_Friction_For_Ms 67
#define Jump_Delay_Ms 500                 // the game's: no jump sooner after the last one

// ---- T7Overcharged's rules

static bool sliding(uint8_t movement_flags) {
    return (int8_t)movement_flags < 0;
}

void aae_move_fastjump_accelerate(aae_move_fastjump_call call, uint8_t movement_flags, float velocity_x,
                                  float velocity_y, float *wish_speed, float *acceleration) {
    if (call == Fastjump_Air_Call) {
        // T7O's C library's sqrtf is the processor's square root for every value but one that isn't a number, and
        // that gives no change either way
        float speed = sqrtf(velocity_y * velocity_y + velocity_x * velocity_x);
        if (Air_Control_Below > speed && !sliding(movement_flags)) {
            *acceleration = *acceleration * Air_Acceleration_Factor;
            *wish_speed = *wish_speed + Air_Wish_Speed_Added;
        }
    } else if (call == Fastjump_Ground_Call) {
        *acceleration = *acceleration * Ground_Acceleration_Factor;
    }
}

bool aae_move_fastjump_fast_enough(uint8_t movement_flags, float velocity_x, float velocity_y) {
    return !sliding(movement_flags) && !(No_Friction_From > velocity_y * velocity_y + velocity_x * velocity_x);
}

bool aae_move_fastjump_just_landed(uint64_t now, uint64_t last_in_air) {
    return now - last_in_air <= No_Friction_For_Ms;
}

int32_t aae_move_fastjump_delay(int32_t since_last_jump) {
    return since_last_jump >= 1 && since_last_jump < Jump_Delay_Ms ? Jump_Delay_Ms : since_last_jump;
}

bool aae_move_fastjump_air_note(aae_move_fastjump_air_t *air, const void *player_state, uint64_t now) {
    unsigned place = air->count;
    for (unsigned i = 0; i < air->count; i++) {
        if (air->player_state[i] == player_state) {
            air->at[i] = now;
            return true;
        }
    }
    bool kept = true;
    if (air->count < Fastjump_Air_Places) {
        air->count++;
    } else {
        place = 0;
        for (unsigned i = 1; i < Fastjump_Air_Places; i++) {
            if (now - air->at[i] > now - air->at[place])
                place = i;
        }
        kept = !aae_move_fastjump_just_landed(now, air->at[place]);
    }
    air->player_state[place] = player_state;
    air->at[place] = now;
    return kept;
}

bool aae_move_fastjump_air_last(const aae_move_fastjump_air_t *air, const void *player_state, uint64_t *at) {
    for (unsigned i = 0; i < air->count; i++) {
        if (air->player_state[i] == player_state) {
            *at = air->at[i];
            return true;
        }
    }
    return false;
}

// ---- the game

static uint64_t uptime_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}
static uint64_t (*now_ms)(void) = uptime_ms;

// the game's
static bool (*PM_IsInAir)(uint8_t *pm, uint8_t *pml);
static const void *air_call_returns, *ground_call_returns; // where PM_AirMove's and PM_WalkMove's first calls of
                                                            // PM_Accelerate return to

static bool debug;
static void watch_move(const uint8_t *pm, const uint8_t *ps, bool in_air, bool on);
static void watch_acceleration(const uint8_t *ps, aae_move_fastjump_call call, float given, float acceleration);
static void watch_no_friction(const uint8_t *ps);

static bool fastjump_on(void) {
    return aae_movement_enabled() && aae_movement_started() && aae_movement_patch_on(Move_FastJump);
}

static float horizontal(const uint8_t *ps, int axis) {
    float value;
    memcpy(&value, ps + Ps_Velocity + 4 * axis, sizeof(value));
    return value;
}

// The times in the air: the server's movement and the client's prediction note and read them on two threads.
static pthread_mutex_t air_lock = PTHREAD_MUTEX_INITIALIZER;
static aae_move_fastjump_air_t air_times;
static unsigned dropped; // times dropped while still within the 67 ms (never expected)

static void note_in_air(const void *player_state, uint64_t now) {
    pthread_mutex_lock(&air_lock);
    bool kept = aae_move_fastjump_air_note(&air_times, player_state, now);
    unsigned count = kept ? dropped : ++dropped;
    pthread_mutex_unlock(&air_lock);
    if (!kept && debug && (count == 1 || count % 1000 == 0))
        printf("movement: fastjump: more than %d player states in the air within %d ms: a time dropped (%u so far)\n",
               Fastjump_Air_Places, No_Friction_For_Ms, count);
}

static bool last_in_air(const void *player_state, uint64_t *at) {
    pthread_mutex_lock(&air_lock);
    bool found = aae_move_fastjump_air_last(&air_times, player_state, at);
    pthread_mutex_unlock(&air_lock);
    return found;
}

// PM_Accelerate, the hook below; returns_to is where the call returns to, which says who called
static void (*orig_PM_Accelerate)(uint8_t *ps, uint8_t *pml, const float *wishdir, float wish_speed,
                                  float acceleration);
static void accelerate(uint8_t *ps, uint8_t *pml, const float *wishdir, float wish_speed, float acceleration,
                       const void *returns_to) {
    aae_move_fastjump_call call = returns_to == air_call_returns      ? Fastjump_Air_Call
                                  : returns_to == ground_call_returns ? Fastjump_Ground_Call
                                                                      : Fastjump_Other_Call;
    if (!fastjump_on()) {
        if (debug)
            watch_acceleration(ps, call, acceleration, acceleration);
        orig_PM_Accelerate(ps, pml, wishdir, wish_speed, acceleration);
        return;
    }
    float given = acceleration, direction[3];
    aae_move_fastjump_accelerate(call, ps[Ps_Movement_Flags], horizontal(ps, 0), horizontal(ps, 1), &wish_speed,
                                 &acceleration);
    if (debug)
        watch_acceleration(ps, call, given, acceleration);
    memcpy(direction, wishdir, sizeof(direction));
    orig_PM_Accelerate(ps, pml, direction, wish_speed, acceleration);
}
// the game jumps here from the function's first instruction (through the pre-patched executable's stub, which only
// jumps), so this function's return address is the game's call
__attribute__((noinline)) static void fake_PM_Accelerate(uint8_t *ps, uint8_t *pml, const float *wishdir,
                                                        float wish_speed, float acceleration) {
    accelerate(ps, pml, wishdir, wish_speed, acceleration, __builtin_return_address(0));
}

// PM_Friction: the time is noted before the game's friction runs, as T7O does
static void (*orig_PM_Friction)(uint8_t *pml, uint8_t *pm);
static void fake_PM_Friction(uint8_t *pml, uint8_t *pm) {
    bool on = fastjump_on();
    if (on || debug) {
        uint8_t *ps;
        memcpy(&ps, pm, sizeof(ps));
        bool in_air = PM_IsInAir(pm, pml);
        if (on && in_air)
            note_in_air(ps, now_ms());
        if (debug)
            watch_move(pm, ps, in_air, on);
    }
    orig_PM_Friction(pml, pm);
}

// "mulss xmm0, [rbp - 0x1c]" right behind PM_Friction's main call of BG_GetFriction: the friction (5.5) in xmm0, the
// player state in rbx
detour_with_registers(PM_Friction_GotFriction) {
    if (!fastjump_on())
        return;
    const uint8_t *ps = (const uint8_t *)registers->rbx;
    uint64_t last;
    if (!aae_move_fastjump_fast_enough(ps[Ps_Movement_Flags], horizontal(ps, 0), horizontal(ps, 1)) ||
        !last_in_air(ps, &last) || !aae_move_fastjump_just_landed(now_ms(), last))
        return;
    const float none = 0.0f;
    memcpy(registers->xmm[0], &none, sizeof(none));
    if (debug)
        watch_no_friction(ps);
}

// "cmp ecx, 500" in PM_CheckJump, before its "jl <no jump>": ecx is the time since the last jump (the "sub" that
// made it left rcx's upper half 0, and so does this); the replaced compare runs again behind this and sets the flags
detour_with_registers(PM_CheckJump_DelayCmp) {
    if (!fastjump_on())
        return;
    registers->rcx = (uint32_t)aae_move_fastjump_delay((int32_t)(uint32_t)registers->rcx);
}

// ---- BO3MACFIX_MOVEDEBUG=1: a line for each jump of a player state with the time since its last one, one for each
// landing with the horizontal speed in the air, and one 100 ms after each landing with the speed then. Speeds are in
// units a second. Movement runs for the server and for the client's prediction, which goes over the same commands
// again and again: a move is counted once, the first time its command's time is ahead of the last one counted.

#define Watched 8
#define After_Landing_Ms 100
typedef struct {
    const void *player_state;
    bool counting;         // the move going on now is counted
    bool in_air, landing;  // in the air; or landed, and the line after the landing still to come
    int32_t last_time;     // the command time of the last move counted
    int32_t jump_time;     // the player state's jump time then
    int32_t since;         // when it went into the air, or landed
    float jump_speed, highest, landing_speed;
    unsigned air_moves, raised;          // moves in the air, and those whose air control the patch raised
    unsigned ground_moves, frictionless; // moves after the landing, and those the patch kept without friction
} watch_t;
static struct {
    watch_t watch[Watched];
    unsigned count, next;
} watched; // under air_lock
static atomic_int said_on = -1;  // whether the patch was on at the last line about it
static atomic_bool said_ground;  // the ground acceleration's line is out

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say_printf(const char *line) {
    printf("movement: fastjump: %s\n", line);
}
static void (*say_line)(const char *line) = say_printf;
static void say(const char *format, ...) {
    char line[300];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    say_line(line);
}

static watch_t *watch_of(const void *player_state, bool add) {
    for (unsigned i = 0; i < watched.count; i++) {
        if (watched.watch[i].player_state == player_state)
            return &watched.watch[i];
    }
    if (!add)
        return NULL;
    unsigned place = watched.count < Watched ? watched.count++ : watched.next++ % Watched;
    memset(&watched.watch[place], 0, sizeof(watched.watch[place]));
    watched.watch[place].player_state = player_state;
    return &watched.watch[place];
}

// the time from one command time to another (they are ints that may go round)
static int32_t elapsed(int32_t from, int32_t to) {
    return (int32_t)((uint32_t)to - (uint32_t)from);
}

static bool same_bits(float a, float b) {
    return memcmp(&a, &b, sizeof(a)) == 0;
}

static float speed_of(const uint8_t *ps) {
    float x = horizontal(ps, 0), y = horizontal(ps, 1);
    return sqrtf(x * x + y * y);
}

static void after_landing(const watch_t *w, unsigned which, int32_t time, float speed) {
    say("player state %u, %d ms after landing at %.0f: %.0f, without friction in %u of %u ground moves", which,
        elapsed(w->since, time), (double)w->landing_speed, (double)speed, w->frictionless, w->ground_moves);
}

static void watch_move(const uint8_t *pm, const uint8_t *ps, bool in_air, bool on) {
    if (atomic_exchange(&said_on, on) != (int)on)
        say("the patch is %s", on ? "on" : "off");
    int32_t time, jump_time;
    memcpy(&time, pm + Pm_Server_Time, sizeof(time));
    memcpy(&jump_time, ps + Ps_Jump_Time, sizeof(jump_time));
    float speed = speed_of(ps);
    pthread_mutex_lock(&air_lock);
    bool new_one = watch_of(ps, false) == NULL;
    watch_t *w = watch_of(ps, true);
    unsigned which = (unsigned)(w - watched.watch);
    if (!new_one && elapsed(w->last_time, time) <= 0) {
        w->counting = false;
        pthread_mutex_unlock(&air_lock);
        return;
    }
    w->counting = true;
    w->last_time = time;
    // a jump moves the jump time on; the client's prediction may also set it back, to the server's, and on again to
    // the same jump, so only a time past the latest one is a jump
    bool jumped = elapsed(w->jump_time, jump_time) > 0;
    if (!new_one && jumped) {
        if (w->jump_time != 0)
            say("player state %u jumped %d ms after its last jump", which, elapsed(w->jump_time, jump_time));
        else
            say("player state %u jumped", which);
    }
    if (new_one || jumped)
        w->jump_time = jump_time;
    if (in_air && (new_one || !w->in_air)) {
        if (w->landing)
            after_landing(w, which, time, speed);
        w->landing = false;
        w->since = time;
        w->jump_speed = w->highest = speed;
        w->air_moves = w->raised = 0;
    } else if (in_air) {
        w->highest = speed > w->highest ? speed : w->highest;
    } else if (!new_one && w->in_air) {
        say("player state %u landed after %d ms in the air: %.0f at the jump, %.0f the highest, %.0f at the landing; "
            "the air control raised in %u of %u air moves", which, elapsed(w->since, time), (double)w->jump_speed,
            (double)w->highest, (double)speed, w->raised, w->air_moves);
        w->landing = true;
        w->since = time;
        w->landing_speed = speed;
        w->ground_moves = w->frictionless = 0;
    }
    if (!in_air && w->landing) {
        if (elapsed(w->since, time) >= After_Landing_Ms) {
            after_landing(w, which, time, speed);
            w->landing = false;
        } else {
            w->ground_moves++;
        }
    }
    w->in_air = in_air;
    pthread_mutex_unlock(&air_lock);
}

static void watch_acceleration(const uint8_t *ps, aae_move_fastjump_call call, float given, float acceleration) {
    if (call == Fastjump_Ground_Call && !same_bits(given, acceleration) && !atomic_exchange(&said_ground, true))
        say("the ground acceleration %.4g became %.4g", (double)given, (double)acceleration);
    if (call != Fastjump_Air_Call)
        return;
    pthread_mutex_lock(&air_lock);
    watch_t *w = watch_of(ps, false);
    if (w != NULL && w->counting && w->in_air) {
        w->air_moves++;
        w->raised += !same_bits(given, acceleration);
    }
    pthread_mutex_unlock(&air_lock);
}

static void watch_no_friction(const uint8_t *ps) {
    pthread_mutex_lock(&air_lock);
    watch_t *w = watch_of(ps, false);
    if (w != NULL && w->counting && w->landing)
        w->frictionless++;
    pthread_mutex_unlock(&air_lock);
}

void aae_move_fastjump_init(uint64_t game_base_address) {
    const char *env = getenv("BO3MACFIX_MOVEDEBUG");
    debug = env != NULL && atoi(env) != 0;
    // with the setting off the hooks are set only for the debug lines, to see the game's own numbers: they then do
    // nothing but watch
    if (!aae_movement_enabled() && !debug)
        return;
    if (!prepatch_has_hook(ADDR_PM_Accelerate) || !prepatch_has_hook(ADDR_PM_Friction) ||
        !prepatch_has_hook(ADDR_PM_Friction_GotFriction) || !prepatch_has_hook(ADDR_PM_CheckJump_DelayCmp)) {
        printf("movement: fastjump: not running from the pre-patched executable: jumps and landings stay as the "
               "game has them\n");
        return;
    }
    PM_IsInAir = (void *)(game_base_address + ADDR_PM_IsInAir);
    air_call_returns = (const void *)(game_base_address + ADDR_PM_AirMove_AccelerateReturn);
    ground_call_returns = (const void *)(game_base_address + ADDR_PM_WalkMove_AccelerateReturn);
    int failed = DobbyHook((void *)(game_base_address + ADDR_PM_Accelerate), (void *)fake_PM_Accelerate,
                           (void **)&orig_PM_Accelerate) != 0;
    failed += DobbyHook((void *)(game_base_address + ADDR_PM_Friction), (void *)fake_PM_Friction,
                        (void **)&orig_PM_Friction) != 0;
    install_detour_PM_Friction_GotFriction((void *)(game_base_address + ADDR_PM_Friction_GotFriction));
    install_detour_PM_CheckJump_DelayCmp((void *)(game_base_address + ADDR_PM_CheckJump_DelayCmp));
    if (failed)
        printf("movement: fastjump: %d of its two hooks couldn't be set\n", failed);
    if (debug)
        printf("movement: fastjump: hooks on PM_Accelerate and PM_Friction, detours at the jump delay and the "
               "friction%s\n", aae_movement_enabled() ? "" : " (AAEMovement is off: they only watch)");
}
