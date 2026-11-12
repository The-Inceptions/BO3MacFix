#pragma once
#include <stddef.h>
#include <stdint.h>

// LuaHeapGrowth: lets the UI's Lua VM have up to 24 MB more when its 60 MB are full, as All-around Enhancement 4's
// Windows plugin does (see lua_heap.c)
void lua_heap_init(uint64_t game_base_address, int enabled);
// the bytes the UI's Lua VM holds and may hold right now (its 60 MB and what it got on top); 0 before it exists
size_t lua_heap_used(size_t *capacity);
