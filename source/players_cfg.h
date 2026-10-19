// Which players folder a .cfg comes from (the ASL_ToNativePath hook in bo3macfix.c). The engine's players folder is the
// one in Application Support, but a mod's Lua sees the game folder's (lua_correct_path), where Windows has just the one.
// All-around Enhancement saves its options and custom mutations from Lua as .cfg files there and loads them with exec,
// which only looks in the engine's folder, so they never loaded (and the defaults it then saved replaced them).
// Plain C with no game calls, so tests/test_players_cfg.c tests it with made-up folders.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define Engine_Players_Folder "/Application Support/Call of Duty Black Ops III/players/"

// path: what the engine asked for. game_folder: the game folder, ending in '/'. For a .cfg in the engine's players
// folder that the game folder's players folder has too, puts the game folder's copy in out and returns true, unless the
// engine has its own copy that's newer: whichever was saved last is used. So a mod's file the engine has never seen comes
// from the game folder, and an older copy there of a file the engine keeps itself (its bindings_0.cfg, in a players
// folder brought over from Windows, say) doesn't replace the engine's.
bool players_cfg_in_game_folder(const char *path, const char *game_folder, char *out, size_t out_len);
