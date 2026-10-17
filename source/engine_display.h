// Following display changes: the engine's side, see engine_display.c.
#pragma once
#include <stddef.h>
#include "display_follow.h"

void engine_display_init(void);                          // the R_CheckVidRestart and R_SetWndParms hooks
void engine_display_request(const DisplayDecision *d);   // any thread; applied on the engine's next frame (tier 2)
void engine_display_describe(char *buf, size_t n);      // "game=WxH refresh=N maxfps=N"
// FullScreenMode as the engine has it, from any thread (plain reads): 0 windowed, 1 Fullscreen, 2 Fullscreen Window, -1
// before the dvar exists. While alt-tab has left fullscreen until the game is active again, the mode it was in.
int engine_display_fullscreen_mode(void);
