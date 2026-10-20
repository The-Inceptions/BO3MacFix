// Whether the game is in the background, for BackgroundFPS (gpu_tweaks.c), see app_focus.m.
#pragma once
#include <stdbool.h>

// calls changed(background) on the main thread now and whenever that changes: the game isn't the active app, it's
// hidden (Cmd+H), or none of its windows can be seen (minimized, on another Space, covered). Before the game's window
// is up that's true too; BackgroundFPS waits for the menus or a match anyway (frame_limit.c). Safe to call more than
// once; the first callback is the one used.
void app_focus_watch(void (*changed)(bool background));
