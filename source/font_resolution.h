#pragma once
#include <stdint.h>

// SharpFonts: menu and HUD text from glyphs drawn twice as detailed into the game's font atlas, as All-around
// Enhancement 4's Windows plugin draws it (see font_resolution.c)
void font_resolution_init(uint64_t game_base_address, int enabled);
