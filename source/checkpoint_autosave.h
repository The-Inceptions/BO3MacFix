// AutoSave: saving a solo zombies game by itself at the start of each new round, and deleting the saved game.
// checkpoint.lua sees the new round and reads the game; the writer here saves it to the same checkpoint.ini as the
// settings page's Save game (checkpoint.c), on a background queue and at most once per interval, so a round change
// never waits for the disk. checkpoint.c isn't thread-safe, so every call into it holds checkpoint_lock(), from the
// UI's Lua (settings_menu.c) or from the writer. Plain C with libdispatch, so tests/test_checkpoint_autosave.c tests
// it with made-up folders.
#pragma once
#include <stdbool.h>
#include "checkpoint.h"

// the lock every call into checkpoint.c holds. It's recursive, so a function here that takes it can be called holding it.
void checkpoint_lock(void);
bool checkpoint_trylock(void); // false, without waiting, while another thread has it
void checkpoint_unlock(void);

// whether a save at round may replace the saved game, given as checkpoint_read's "Key=value" lines (empty for none):
// yes when there's none, it's at round or lower, or this game resumed it (resumed). A deeper game this one didn't
// resume stays.
bool checkpoint_may_replace(const char *saved, long round, bool resumed);

// AutoSave on or off (settings.ini's AutoSave, on by default)
void checkpoint_autosave_set_enabled(bool on);
bool checkpoint_autosave_enabled(void);

// what became of a save the writer was given
typedef enum {
    Autosave_Saved,       // it's in checkpoint.ini
    Autosave_Kept_Deeper, // the saved game is at a higher round and this game didn't resume it, so that one stays
    Autosave_Resuming,    // this game resumed the saved game and hasn't had its weapons back yet, which they're for
    Autosave_Failed,      // no round in it, malformed, or checkpoint.ini couldn't be written
} autosave_outcome_t;

// told on the writer's queue after each save it was given: what became of it, its round, the saved game's round
// (0 for none) and the note it came with
typedef void (*autosave_report_t)(autosave_outcome_t outcome, long round, long kept, const char *note);

// where the writer saves, the least time between two of its saves, and what it tells. Call before posting; calling it
// again changes them for the saves after.
void checkpoint_autosave_init(const checkpoint_paths_t *paths, int min_interval_ms, autosave_report_t report);

// gives the writer a save ("Key=value" lines, as checkpoint_save takes them) with a note for the report, in place of
// one it hasn't written yet, and returns at once; the writer then applies checkpoint_may_replace. False if AutoSave is
// off, the writer isn't set up, or there's no memory.
bool checkpoint_autosave_post(const char *lines, const char *note);

// drops a save the writer hasn't written yet: a manual save or a delete is newer than it
void checkpoint_autosave_cancel(void);

// waits until the writer has dealt with every save it was given; never call it holding the lock
void checkpoint_autosave_flush(void);

// deletes the saved game (checkpoint.ini), dropping a save the writer hasn't written yet. False, deleting nothing,
// while a resume is set up (checkpoint_clear first, which puts the player's own AAE options back) or if the file
// can't be removed; true if there was none.
bool checkpoint_delete(const checkpoint_paths_t *paths);
