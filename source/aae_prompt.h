// All-around Enhancement 4's prompts in the world, kept by distance as its Windows plugin makes the game keep them
// (AAEPrompts). See aae_prompt.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void aae_prompt_init(uint64_t game_base_address, int enabled);

// whether a prompt that has this objective gets its "far away" mark cleared at the place where the game leaves it
// alone
bool aae_prompt_clears_far_away(bool mod_loaded, int32_t objective);
