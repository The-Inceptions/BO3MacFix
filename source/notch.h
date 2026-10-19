// AvoidNotch: keeps the game's picture below the built-in display's camera housing in fullscreen, see notch.m.
#pragma once
#include <stdbool.h>

void notch_init(int enabled);           // dylib init: hooks ASL's letterbox and Metal client rect when enabled
bool notch_live_possible(void);         // whether notch_set_enabled can apply now (hooked already, or slots in the copy)
bool notch_set_enabled(int enabled);    // the settings page, any thread; false = needs a restart
bool notch_take_refresh(void);          // engine thread, once a frame: true = redo ASL's letterbox and client rect now
// metalfx.m on the render thread, during present: the drawable the client rect forced while the picture is placed
// below the housing (its blit rect isn't centred then), false otherwise
bool notch_forced_drawable(int *width, int *height);
