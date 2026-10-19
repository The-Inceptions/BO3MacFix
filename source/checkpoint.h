// A checkpoint to come back to a zombies game later, with All-around Enhancement (checkpoint.c). Zombies has no save
// system, but AAE's custom mutations can start a game at a chosen round with chosen points (tfoption_starting_round,
// tfoption_starting_points). The settings page saves the round and points, with what else the UI shows (map, perks,
// weapon, character, scoreboard) for reference, in BO3MacFix/checkpoint.ini. "Resume" then sets those two options for
// the next game, and once that game is under way the player's own values for them go back.
// Plain C, so tests/test_checkpoint.c tests it with made-up folders.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define Checkpoint_File "../../../BO3MacFix/checkpoint.ini"
#define Checkpoint_Players_Folder "../../../players/"
#define Checkpoint_Max_Round 255     // AAE's Starting Round slider goes from 1 to 255
#define Checkpoint_Max_Points 9999990 // the most a zombies score shows
#define Checkpoint_Round_Dvar "tfoption_starting_round"
#define Checkpoint_Points_Dvar "tfoption_starting_points"

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

// saves a checkpoint from "Key=value" lines (Map, Round, Points, Perks, Weapon, Character, Stats; Round and Points are
// needed), with the time it's saved at, replacing the last one. A resume that's set up stays set up. False if a line is
// malformed, Round or Points is missing or not a number, or the file can't be written.
bool checkpoint_save(const checkpoint_paths_t *paths, const char *lines, long now);

// the checkpoint as "Key=value" lines into out (empty if there's none)
void checkpoint_read(const checkpoint_paths_t *paths, char *out, size_t out_size);

// sets up the next game to start at the checkpoint's round (1 to 255) with its points: in the AAE preset's custom
// mutations file (preset: what follows AAECustomMutations in its name, like "_Default") and, through the console
// commands put in commands (one per line, for Engine.Exec), in the game's options now. previous_round and
// previous_points are those options' values before (empty if unset), which checkpoint_clear puts back. Doing it again
// keeps the values from the first time. False, changing nothing, if there's no checkpoint, an argument is malformed or
// a file can't be written.
bool checkpoint_apply(const checkpoint_paths_t *paths, const char *preset, const char *previous_round,
                      const char *previous_points, char *commands, size_t commands_size);

// whether a resume is set up, and whether a game has started with it since (checkpoint_mark_in_use)
typedef enum { Checkpoint_No_Resume, Checkpoint_Resume_Set_Up, Checkpoint_Resume_In_Use } checkpoint_resume_t;
checkpoint_resume_t checkpoint_resume_state(const checkpoint_paths_t *paths);

// notes that a game has started with the resume set up, so it's undone back in the menus; false if none is set up
bool checkpoint_mark_in_use(const checkpoint_paths_t *paths);

// undoes checkpoint_apply: puts the previous values back in the mutations file, unless they were changed since, and
// puts the console commands that set them again in commands, for the options whose current values (current_round,
// current_points, NULL when there's no game to ask yet) are still the checkpoint's. The checkpoint itself stays. False
// if no resume was set up or a file can't be written.
bool checkpoint_clear(const checkpoint_paths_t *paths, const char *current_round, const char *current_points,
                      char *commands, size_t commands_size);
