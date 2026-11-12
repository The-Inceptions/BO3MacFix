#pragma once
#include <stdbool.h>
#include <stdint.h>

// AAEMantling: mantling as All-around Enhancement 4 sets it up through its Windows plugin's dvars (see aae_mantle.c)
void aae_mantle_init(uint64_t game_base_address, int enabled);

// ---- the rules, apart from the game (tests/test_aae_mantle.c)

// the move input the game's traversal code is shown, so that its own test "more than 50" comes out as the mod's
// "more than threshold" (and "any input at all" for 0). In place; only their size changes, not their direction.
void aae_mantle_scan_input(int threshold, signed char *forward, signed char *right);
// the same for the game's test before it looks for a ledge at all, which the mod drops for a threshold of 0 or less
void aae_mantle_start_input(int threshold, signed char *forward, signed char *right);
// what the scan's first trace hit: whether to go on looking for a ledge there, and the "over" flag for the choice of
// animation. Returns the contents to show the game so that its own test agrees.
uint32_t aae_mantle_first_hit(bool ignore_checks, uint32_t contents, uint32_t surface_flags, uint32_t *traversal_flags);
// the kind of traversal for a ledge this far above the player's feet, or -1 for the game's own choice
int aae_mantle_high_type(float height, float offset, uint32_t traversal_flags);
// the height to show the game's own choice when it's below the mod's first step but not below the game's
float aae_mantle_low_height(float height);
