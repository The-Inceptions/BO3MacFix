// The local player's weapons in a game this Mac hosts (a solo zombies game), read from the game's own player state,
// and the game's cheat check (CheatsOk), which its give and take console commands make: BO3MacFix's own commands get
// through it for a moment, so a resumed game gets its saved weapons back without sv_cheats (which All-around
// Enhancement watches, turning its bank and weapon locker off). The addresses come from the give and take commands in
// the game's code (offsets.h).
//
// The full loadout (zm_loadout.h) comes from there too: each gun with its attachments, weapon options (the camo) and
// alternate ammo type, and the perks, the last two from the server script VM's variables. It's restored by the give
// command "give bo3macfix_restore": its cheat check runs on the server thread, inside the game's client command
// handling, where the game runs script callbacks itself, and there the restore calls what the Pack-a-Punch machine
// does: the GiveWeapon builtin's code with the saved options, aat::acquire(weapon, name), and zm_perks::give_perk.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "zm_loadout.h"

// call once with the game's base address
void zm_inventory_init(uint64_t game_base);

// hooks the game's CheatsOk; call at start-up, with the other hooks
void zm_inventory_install_hook(void);

// the local player's weapons as "name:type" separated by commas, the one in hand first (type: the weapon's inventory
// type, 0 for a primary weapon, 1 grenades and the like, 4 melee). Reads that would fault are refused, so a wrong
// address gives false, not a crash. False, with out empty, if there's no game hosted here or no player in it.
bool zm_inventory_read(char *out, size_t out_size);

// lets the "give name" and "take name" lines of commands (BO3MacFix's own, as the game will run them) through the cheat
// check, each once, for the local player, whenever the game runs it: it holds commands while it's paused. Replaces the
// ones allowed before (none for an empty text, which checkpoint.lua sends as a match starts: that also drops a shield's
// health still waiting, see zm_inventory_new_map); returns how many. The other cheat commands (noclip, god and the like)
// and other names still need sv_cheats.
int zm_inventory_allow_commands(const char *commands);
// how many of those the game hasn't run yet
int zm_inventory_allowed_left(void);

// the local player's loadout: its primary weapons, with names that have their attachments, options, cosmetic variants
// and ammo types, the one in hand, and its perks. note gets what couldn't be read from the script VM, if anything.
// From any thread: every read that would fault is refused. False if there's no game hosted here or no player in it.
bool zm_inventory_read_loadout(loadout_t *out, char *note, size_t note_size);

// sets up plan for the next "give bo3macfix_restore", for 30 seconds; returns that command
const char *zm_inventory_set_restore(const loadout_t *plan);
// for testing: the same with plan's guns given the Pack-a-Punch machine's way (zm_weapons::give_build_kit_weapon, the
// game's camo and build kit), then their ammo types and the perks; the guns' options are left out
const char *zm_inventory_set_pap_test(const loadout_t *plan);

// once the restore has run: what it did, as one line (loadout_result_text), into out; false until then
bool zm_inventory_restore_result(char *out, size_t out_size);

// after a restore made while the player had no control (a map's intro: Ascension's lander ride), called by Lua at each
// poll: NULL once there's nothing more to look at, "" meanwhile, and "give bo3macfix_switch" when the player has had
// control for a moment with a gun in hand other than the one the restore put there (the game's own, as the intro
// ended): that command switches back to it, on the server thread (loadout_switch_poll). From the UI thread.
const char *zm_inventory_switch_poll(void);

// at a map change (HKAI_InitMapData, a restart too): a shield's health the last restore set up and still waits to
// set, for the shield it gave, is dropped, so a shield held in the next game doesn't get it
void zm_inventory_new_map(void);
