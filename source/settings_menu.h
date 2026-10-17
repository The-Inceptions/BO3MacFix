#pragma once
#include "havok.h"
// sets up the in-game settings page in a UI Lua state that doesn't have it yet (called from
// Engine.GetBuildStringField and Engine.Exec)
void settings_menu_run(lua_State *s);
