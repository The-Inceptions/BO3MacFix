// T7Overcharged's breath component, as All-around Enhancement 4's build of it has it (AAEHoldBreath). See
// aae_breath.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void aae_breath_init(uint64_t game_base_address, int enabled);
// T7Overcharged has been started by the mod (its init, t7overcharged.c): registers its two dvars, once
void aae_breath_t7o_started(void);

// the statuses and the names T7Overcharged gives them; 5 has none, and nothing gives it
enum {
    Aae_Breath_Ads_Off = 0,            // not aiming all the way, or nothing to say about the weapon
    Aae_Breath_Ads_On = 1,             // aiming all the way
    Aae_Breath_Can_Breath = 2,         // aiming all the way with a weapon whose sway holding the breath steadies
    Aae_Breath_Holding = 3,            // holding the breath, while it is taken in
    Aae_Breath_Holding_Zoomed = 4,     // aiming all the way through a scope with zoom levels
    Aae_Breath_Starts = 6,             // the breath is held from this frame on
    Aae_Breath_Breathes_Out = 7,       // it was let go
    Aae_Breath_Gasps = 8,              // it was held too long
    Aae_Breath_Held = 9,               // holding the breath, after it was taken in
};

// What T7Overcharged reads of the player, from the client's predicted player state and its cg_t.
typedef struct {
    bool holding;           // the breath is held: the player state's weapFlags & 4
    bool eflag_0x4000;      // the player state's eFlags & 0x4000
    float sights;           // how far the sights are up (fWeaponPosFrac): 1 all the way
    int32_t hold_time;      // the game's time the breath has been held (cg_t), -1 while it isn't
    int32_t breath_in_time; // how long of that it is taken in for: 1000, or 0 when that was cut short
    int32_t delay;          // the time before the game's next breath sound
    int32_t frame_time;     // the last frame's ms
} aae_breath_player_t;

// The held weapon's definition, as far as T7Overcharged reads it.
typedef struct {
    int32_t weapon_class;   // weapClass, 10 for an item
    bool steadies;          // bHoldBreathToSteady: holding the breath steadies its sway
    bool airburst;          // bAirburstWeapon
} aae_breath_weapon_t;

// The game, asked in T7Overcharged's order and only as far as it gets.
typedef struct {
    void *context;
    // the held weapon (the player state's view model weapon) and its definition
    void (*weapon)(void *context, aae_breath_weapon_t *weapon);
    // whether the weapon has a scope overlay (T7Overcharged's OFF_BG_IsWeaponZoomed), then whether its zoom levels
    // differ (its OFF_BG_IsWeaponScoped)
    bool (*overlay)(void *context);
    bool (*zoom_levels)(void *context);
} aae_breath_game_t;

// T7Overcharged's aim status for a player: first from the weapon while it is aimed all the way (1, 2 or 4, else 0),
// then from the game's hold breath timers, the way the game's next look at them will go (3, 6, 7, 8 or 9, or as it
// was).
int aae_breath_status(const aae_breath_player_t *player, const aae_breath_game_t *game);
// "ADS_OFF", "ADS_ON." (with the point), "CAN_BREATH", "HOLDING_BREATH", "HOLDING_BREATH_ZOOMED", else "Unknown"
const char *aae_breath_status_name(int status);
// Whether a status is to be handed to the mod: it isn't the last one handed over (*last starts at -1, one for all
// local clients), which it then becomes.
bool aae_breath_publish(int *last, int status);
