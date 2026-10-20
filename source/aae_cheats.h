// All-around Enhancement's bank and weapon locker at a map start (AAEBankFix, on by default).
//
// The game's map command turns sv_cheats on whenever a mod is loaded, just before it spawns the server (and for devmap
// alike). AAE turns it off again itself, unless its own cheat switch elmg_cheats is set, but only once its players
// connect: its banks script checks sv_cheats before that, during the map load, and leaves out the bank, the weapon
// locker and the loader of each player's bank. So for a map (not devmap) started with AAE loaded and elmg_cheats not
// set, the map command sets sv_cheats to 0 instead, as AAE itself would. devmap, other mods and no mod stay as they were.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define Aae_Mod_Name "2631943123" // AAE's folder name, its workshop id

// whether a map started by command ("map" or "devmap", the console command's argv[0]) with mod loaded (its folder
// name, "" without one) should leave sv_cheats off: elmg_cheats is that dvar's value (0 if it doesn't exist). NULL for
// what couldn't be read keeps the game's behaviour
bool aae_cheats_keep_off(const char *command, const char *mod, int elmg_cheats);

// at start-up, before the game's threads run: hooks Dvar_SetBool when enabled (in the pre-patched copy that only
// stores its slot's pointer)
void aae_cheats_install(uint64_t base_address, bool enabled);
