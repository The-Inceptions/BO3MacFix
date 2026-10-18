// BO3MacFix's settings: one table describing every settings.ini option, a writer for settings.ini, restarting the
// game, and the BO3MacFix Lua table an in-game menu uses (see settings.c).
#pragma once
#include <stdbool.h>
#include "havok.h"

// creates the global Lua table BO3MacFix with GetSettings, SetSetting, RestartGame and RestartPending
void settings_lua_install(lua_State *s);

// sets Name = value in settings.ini, keeping every other line, written atomically. Returns false on failure.
bool settings_write(const char *name, const char *value);

// what the settings page's restart does: starts the game again through Steam once this process has gone, and quits
void settings_restart_game(void);
