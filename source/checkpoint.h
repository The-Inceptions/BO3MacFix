// Saving a zombies game to resume it later, with All-around Enhancement (checkpoint.c). Zombies has no save system, but
// AAE's custom mutations can start a game at a chosen round with chosen points (tfoption_starting_round,
// tfoption_starting_points) and with every perk (tfoption_perkaholic) or Quick Revive (tfoption_spawn_with_quick_res).
// The settings page saves the round, points, perks and weapons, with what else the UI shows (map, character,
// scoreboard) to look at, in BO3MacFix/checkpoint.ini. "Resume" then sets those options for the next game, and gives
// the saved weapons back once it's under way (zm_inventory.c); after that game the player's own options go back.
// Checkpoints with the full loadout (zm_loadout.h) get each gun back with its camo, attachments and ammo type, and the
// exact perks, so AAE's perk options are left alone for them. Plain C, so tests/test_checkpoint.c tests it with
// made-up folders.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "zm_loadout.h"

#define Checkpoint_File "../../../BO3MacFix/checkpoint.ini"
#define Checkpoint_Players_Folder "../../../players/"
#define Checkpoint_Max_Round 255      // AAE's Starting Round slider goes from 1 to 255
#define Checkpoint_Max_Points 9999990 // the most a zombies score shows
#define Checkpoint_Round_Dvar "tfoption_starting_round"
#define Checkpoint_Points_Dvar "tfoption_starting_points"
#define Checkpoint_Perkaholic_Dvar "tfoption_perkaholic"
#define Checkpoint_Quick_Revive_Dvar "tfoption_spawn_with_quick_res"
// the perk the HUD calls quick_revive (hudItems.perks.quick_revive), the only one AAE can give by itself
#define Checkpoint_Quick_Revive_Perk "quick_revive"
// the "Key = value" entries checkpoint.ini holds: a save's 16 keys and Saved, a resume's InUse, Applied, AppliedPreset
// and 2 per AAE option (28 in all), with room for more. A save or resume that needs one more is refused, with a log
// line, and the file stays as it was.
#define Checkpoint_Max_Entries 64
// checkpoint_read's out: every entry checkpoint.ini can hold (a 31-character key and a 1023-character value each)
#define Checkpoint_Read_Size 69632

// AAE's custom mutations files are "modvar <dvar> <value>" lines, which it runs with exec.

// the value the last line in text that sets dvar gives it (without quotes around it) into out; false if no line sets
// it or the value doesn't fit
bool aae_cfg_value(const char *text, const char *dvar, char *out, size_t out_size);

// text with dvar set to value into out: the first line that sets it gets the value and any later ones go, or a line is
// added at the end if none does. A NULL value takes out every line that sets it. Every other line stays as it was.
// False if out is too small.
bool aae_cfg_set(const char *text, const char *dvar, const char *value, char *out, size_t out_size);

typedef struct {
    const char *checkpoint; // checkpoint.ini
    const char *players;    // the folder AAE keeps AAECustomMutations_<preset>.cfg in, ending in '/'
} checkpoint_paths_t;

// saves a game from "Key=value" lines, with the time it's saved at, replacing the last one. Round and Points are
// needed. Perks (the HUD's perk names, like juggernaut), Weapons (the primary weapons' names, like ray_gun_upgraded)
// and Equipment (the other weapons) are comma-separated names. Map, Held (the weapon in hand, as the HUD names it),
// Character and Stats are text to show. The full loadout is Guns, GunInHand and PerkList (zm_loadout.h's text). A
// resume that's set up stays set up. False if a line is malformed, Round or Points is missing or not a number, a list
// has something other than names in it, the loadout doesn't read back, or the file can't be written.
bool checkpoint_save(const checkpoint_paths_t *paths, const char *lines, long now);

// the checkpoint as "Key=value" lines into out (empty if there's none); out_size is Checkpoint_Read_Size, so every
// line fits
void checkpoint_read(const checkpoint_paths_t *paths, char *out, size_t out_size);

// sets up the next game to resume the checkpoint: its round (1 to 255), its points, and Perkaholic and Quick Revive if
// it had perks, in the AAE preset's custom mutations file (preset: what follows AAECustomMutations in its name, like
// "_Default") and, through the console commands put in commands (one per line, for Engine.Exec), in the game's options
// now. Each option's value in the file before (or none) is kept for checkpoint_clear; an option that already has the
// checkpoint's value is left alone. Doing it again keeps the values from before the first time. False, changing
// nothing, if there's no checkpoint, the preset is malformed or a file can't be written.
bool checkpoint_apply(const checkpoint_paths_t *paths, const char *preset, char *commands, size_t commands_size);

// whether a resume is set up, whether a game has started with it since (checkpoint_mark_in_use), and whether that game
// got the saved weapons back (checkpoint_mark_weapons_given)
typedef enum {
    Checkpoint_No_Resume,
    Checkpoint_Resume_Set_Up,
    Checkpoint_Resume_In_Use,
    Checkpoint_Resume_Weapons_Given,
} checkpoint_resume_t;
checkpoint_resume_t checkpoint_resume_state(const checkpoint_paths_t *paths);

// notes that a game has started with the resume set up, so it's undone back in the menus; false if none is set up
bool checkpoint_mark_in_use(const checkpoint_paths_t *paths);

// notes that the game the resume is in use in got its weapons back, so nothing gives them again; false if no game has
// started with the resume
bool checkpoint_mark_weapons_given(const checkpoint_paths_t *paths);

// undoes checkpoint_apply: puts each option's value from before back in the mutations file, unless it was changed
// since, and puts the console commands that set them again in commands, for the options whose current values in the
// game are still the checkpoint's. current: "dvar=value" lines with those values, NULL when there's no game to ask yet.
// The checkpoint itself stays. False if no resume was set up or a file can't be written.
bool checkpoint_clear(const checkpoint_paths_t *paths, const char *current, char *commands, size_t commands_size);

// the checkpoint's full loadout (Guns, GunInHand, PerkList) into loadout; false if it has none or it's malformed
bool checkpoint_loadout(const checkpoint_paths_t *paths, loadout_t *loadout);

// the console commands that turn the weapons a player holds into the saved ones, one per line into out: "take <name>"
// for each held weapon that isn't saved, then "give <name>" for each saved one that isn't held. saved and held are
// comma-separated weapon names. Returns how many commands there are, 0 if nothing is saved (a game saved without its
// weapons keeps the ones it starts with), or -1 if a list has something other than names in it or out is too small.
int checkpoint_weapon_commands(const char *saved, const char *held, char *out, size_t out_size);
