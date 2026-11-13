// T7Overcharged's movement patch "fastjump" (AAEMovement): see aae_move_fastjump.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// installs the hooks, once, when the setting AAEMovement is on (aae_movement.h); they act while the patch is on
void aae_move_fastjump_init(uint64_t game_base_address);

// ---- T7Overcharged's rules, without the game

// which of the game's calls of PM_Accelerate it is
typedef enum {
    Fastjump_Other_Call,  // any other (also BO3MacFix's own)
    Fastjump_Air_Call,    // PM_AirMove's first
    Fastjump_Ground_Call, // PM_WalkMove's first
} aae_move_fastjump_call;

// The wish speed and the acceleration that PM_Accelerate is to get from this call: from the air move, while the
// horizontal speed is below 150 and the player isn't sliding, the acceleration ×4 and the wish speed +170; from the
// ground move the acceleration ×1.1111112; from any other call both as they are.
void aae_move_fastjump_accelerate(aae_move_fastjump_call call, uint8_t movement_flags, float velocity_x,
                                  float velocity_y, float *wish_speed, float *acceleration);

// The two halves of the rule for no ground friction: the player isn't sliding and moves at 100 or more (the square
// of the horizontal speed against 10000; what isn't a number passes, as with T7Overcharged's compare), and the last
// time the player was seen in the air is at most 67 ms before now (the difference taken unsigned).
bool aae_move_fastjump_fast_enough(uint8_t movement_flags, float velocity_x, float velocity_y);
bool aae_move_fastjump_just_landed(uint64_t now, uint64_t last_in_air);

// What PM_CheckJump's "cmp ecx, 500; jl <no jump>" is to see for this time since the last jump, so that it refuses
// a jump only below 1 ms, as T7Overcharged's "cmp eax, 1" does: 1 to 499 become 500, everything else stays.
int32_t aae_move_fastjump_delay(int32_t since_last_jump);

// When each player state was last seen in the air, by its address, for this many of them (T7Overcharged keeps them
// all, see aae_move_fastjump.c)
#define Fastjump_Air_Places 64
typedef struct {
    const void *player_state[Fastjump_Air_Places];
    uint64_t at[Fastjump_Air_Places];
    unsigned count;
} aae_move_fastjump_air_t;

// Notes the time for this player state. When all places are taken, the one seen longest ago (unsigned, as the rule
// counts) makes room: false when that one was seen within the 67 ms, the one case in which this differs from
// T7Overcharged's map.
bool aae_move_fastjump_air_note(aae_move_fastjump_air_t *air, const void *player_state, uint64_t now);
// the time noted for it: false for none
bool aae_move_fastjump_air_last(const aae_move_fastjump_air_t *air, const void *player_state, uint64_t *at);
