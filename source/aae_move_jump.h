// T7Overcharged's jumpslowdown, which All-around Enhancement switches on for every player (AAEMovement): no
// penalty for jumping again and again. See aae_move_jump.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// installs the hooks, once, when the setting AAEMovement is on (after aae_movement_init)
void aae_move_jump_init(uint64_t game_base_address);

// ---- the rules, apart from the game (tests/test_aae_move_jump.c)

// What the rules read and change of the player state (playerState_s, the same offsets in both games)
#define Move_Ps_Flags 0x10        // 64 bits of movement flags
#define Move_Ps_Time 0x24         // pm_time, an int: what the slowdown after a jump counts with (ms)
#define Move_Ps_Origin 0x30       // three floats
#define Move_Ps_Velocity 0x3c     // three floats
#define Move_Ps_Gravity 0xc4      // an int
#define Move_Ps_JumpTime 0x110    // the command time of the last jump, an int
#define Move_Ps_JumpOriginZ 0x114 // the height the last jump started from, a float
#define Move_Flag_Sliding (1ull << 7)
#define Move_Flag_SlowdownPending (1ull << 18) // a jump's slowdown is still to come or going on

// Whether jumpslowdown acts: T7Overcharged's movement has started and the mod's switch for it is on.
bool aae_move_jump_on(bool started, bool switched_on);

// The part T7Overcharged makes for good when its movement starts: a jump while the slowdown of the last one is
// pending is as high as any (the game's divisor of its upward speed becomes 1). Whether the game's test for that
// counts as passed.
bool aae_move_jump_full_height(bool started);

// T7Overcharged's Jump_ReduceFriction, which the game asks for the friction multiplier while a jump's slowdown is
// pending: always 1, and once the slowdown's time is over (pm_time above 1000) the pending slowdown and the jump's
// start height are forgotten, as in the game's own.
float aae_move_jump_reduce_friction(uint8_t *ps);

// T7Overcharged's wrapper of PM_CrashLand, the game's landing: whatever the landing did, the horizontal velocity is
// the one from before it and the vertical one is 0. On the Mac the landing is part of PM_GroundTrace, so the wrapper
// is two halves, at its begin and at its end, and what the begin keeps belongs to that one landing on that thread.
typedef struct {
    uint8_t *ps;           // the player state of the landing going on, NULL when its end is to do nothing
    uint8_t horizontal[8]; // its horizontal velocity before the landing (two floats, kept as bits)
} aae_move_jump_landing_t;
void aae_move_jump_landing_begin(aae_move_jump_landing_t *landing, bool on, uint8_t *ps);
// whether it changed the velocity
bool aae_move_jump_landing_end(aae_move_jump_landing_t *landing, uint8_t *ps);

// The cap of a jump's horizontal speed (the game's Jump_ClampHorizontalVelocity): T7Overcharged leaves it to jumps
// started while sliding (movement flag 7). The cap to give the game's own test: its own one, or +infinity for none.
float aae_move_jump_speed_cap(bool on, uint64_t flags, float cap);
