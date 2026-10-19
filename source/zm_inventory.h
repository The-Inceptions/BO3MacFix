// The local player's weapons in a game this Mac hosts (a solo zombies game), read from the game's own player state,
// and the game's cheat check (CheatsOk), which its give and take console commands make: BO3MacFix's own commands get
// through it for a moment, so a resumed game gets its saved weapons back without sv_cheats (which All-around
// Enhancement watches, turning its bank and weapon locker off). The addresses come from the give and take commands in
// the game's code (offsets.h).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// call once with the game's base address
void zm_inventory_init(uint64_t game_base);

// the local player's weapons as "name:type" separated by commas, the one in hand first (type: the weapon's inventory
// type, 0 for a primary weapon, 1 grenades and the like, 4 melee). Reads that would fault are refused, so a wrong
// address gives false, not a crash. False, with out empty, if there's no game hosted here or no player in it.
bool zm_inventory_read(char *out, size_t out_size);

// lets the local player's next count cheat-protected commands (give, take) through the cheat check, for 10 seconds.
// The first call hooks CheatsOk, from the main thread before any such command runs.
void zm_inventory_allow_cheats(int count);
