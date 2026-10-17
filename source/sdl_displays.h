// Following display changes: SDL 2.0.7's display list and the game window, see sdl_displays.c.
#pragma once
#include <stddef.h>
#include <stdint.h>
void sdl_displays_init(void);                            // the ASL_BuildModeTable hook and the constrainFrameRect fix
int sdl_displays_refresh(uint32_t target, int rehome);   // main thread: rebuild SDL's list with target first; rehome moves the game window; 1 done, 0 busy (retry), -1 failed
int sdl_displays_rebuild_mode_table(void);               // engine thread (tier 2): ASL's mode table for the current display 0; 1 done
int sdl_displays_count(void);
uint32_t sdl_displays_id(int index);                     // index 0 = the game's display
int sdl_game_fullscreen_mode(void);                      // 0 windowed, 1 fullscreen, 2 fullscreen window
uint32_t sdl_game_window_display(void);                  // the display the game window is on now, 0 if there's no window
void sdl_displays_describe(char *buf, size_t n);         // "window=WxH fullscreen=M frame=x,y,w,h screen=x,y,w,h"

// the AppKit half, in sdl_displays_cocoa.m (main thread), only for sdl_displays.c
void sdl_cocoa_init(void);                                         // the constrainFrameRect fix
double sdl_cocoa_screen(uint32_t display, char *name, size_t n);   // the display's backing scale and name
void sdl_cocoa_move_to_screen(void *nswindow, uint32_t display);   // puts the window on the display's screen if it isn't
uint32_t sdl_cocoa_window_display(void *nswindow);                 // the display of the window's screen, 0 if none
void sdl_cocoa_describe(void *nswindow, char *buf, size_t n);      // "frame=x,y,w,h screen=x,y,w,h"
