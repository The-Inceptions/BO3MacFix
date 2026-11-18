// StartWithMod's part in the game (start_mod_game.c; the rules and files are start_mod.c's): what the launcher did
// for this start, and the loaded mod noted at each first menu for the launcher's next start.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// enabled: StartWithMod (only said in the log: the mod is noted either way, so that turning it on starts with the mod
// of the real last quit). folder: BO3MACFIX_STARTWITHMOD_DIR, NULL for BO3MacFix's own folder. launch_note:
// BO3MACFIX_STARTWITHMOD_LAUNCH, what the launcher did, NULL when the game was started without a launcher of this
// version. A start the launcher didn't give the mod for a reason the player has to know gets a lasting notice.
void start_mod_game_init(uint64_t game_base_address, bool enabled, const char *folder, const char *launch_note,
                         bool debug);

// the first menu of a UI state is up (notices.m, the moment of "notices: ready on UIRootFull"): the start got as far
// as the menus, and lastmod.txt gets the mod loaded now. Main thread, like the game's own mod loading.
void start_mod_game_menu_ready(void);
