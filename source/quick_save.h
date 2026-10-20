// Quick save (QuickSave in settings.ini, quick_save.m): a key, or L3 + R3 held for a second in the pause menu, saves a
// solo zombies game the way the settings page's Save game row does. checkpoint.lua does the saving; see quick_save.m.
#pragma once
#include <stdint.h>
#include "havok.h"

// key: QuickSave, the macOS virtual key code (F5 96, F6 97, F7 98, F8 100, F9 101), 0 for off (the buttons too)
void quick_save_init(int key);
// the settings page changed it
void quick_save_set_key(int key);

// a controller's XInput buttons, from gc_bridge.m's XInputGetState hook (any thread)
void quick_save_pad(uint32_t user, uint16_t buttons);

// CheckpointTakeQuickSave() -> nil, or "key" and the key's name, or "pad": a quick save asked for in the last 3 seconds
// (checkpoint.lua's timer, twice a second)
int quick_save_lua_take(lua_State *s);
