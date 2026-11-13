// T7Overcharged's movement changes (AAEMovement): the switches.
//
// THIS IS A STAND-IN, so that the pieces can be built side by side: the real file reads the mod's dvars and has its
// commands movement_patch and movement_patch_all. Until it is here, nothing is on, unless the environment has
// BO3MACFIX_MOVEFORCE=1: then everything is as All-around Enhancement leaves it in an ordinary game (all patches but
// fastswap, omni-movement, a strafe scale of 0.9, a fast ADS multiplier of 1.4), for trying a piece in the game.
#include <stdlib.h>
#include "aae_movement.h"

static bool enabled, forced;

void aae_movement_init(uint64_t game_base_address, int setting) {
    (void)game_base_address;
    const char *force = getenv("BO3MACFIX_MOVEFORCE");
    enabled = setting != 0;
    forced = enabled && force != NULL && atoi(force) != 0;
}

bool aae_movement_enabled(void) {
    return enabled;
}
bool aae_movement_started(void) {
    return forced;
}
bool aae_movement_patch_on(move_patch patch) {
    return forced && patch != Move_FastSwap;
}
bool aae_movement_omni(void) {
    return forced;
}
float aae_movement_strafe_scale(void) {
    return forced ? 0.9f : 0.667f;
}
float aae_movement_fastads_multiplier(void) {
    return forced ? 1.4f : 2.15f;
}
