#pragma once
#include <stdbool.h>
#include <stdint.h>

// LuaUnrefFix: keeps the game from releasing Lua reference 0, which writes 64 GB behind the table it's released from,
// as All-around Enhancement 4's Windows plugin does for one such call (see lua_unref.c)
void lua_unref_init(uint64_t game_base_address, int enabled);
// whether the n-th release of reference 0 gets a line in the log: the first 16, then every power of two
bool lua_unref_logged(uint32_t n);
