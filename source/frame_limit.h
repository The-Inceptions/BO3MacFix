// The frame rate limits that pace presents (gpu_tweaks.c) as plain C: which one applies to a frame, and when the pause
// menu counts as open in a match for PauseMenuFPS. No game or macOS calls, so tests/test_frame_limit.c tests it as is.
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// how long the pause menu counts as open after notices.lua last said it is. It says so four times a second while it
// is (LUI's timers run about 20% late in a match), so a UI that goes away with the menu open, without saying it
// closed, doesn't leave the limit on.
#define Pause_Menu_Timeout_NS 2000000000ULL

// the frame rate to pace at: the lowest of FramePacingFPS, BatteryFPS while on battery and PauseMenuFPS while the
// pause menu is open in a match, leaving out the ones that are 0 (off). 0 when none applies.
int frame_limit_fps(int pacing_fps, int battery_fps, bool on_battery, int pause_menu_fps, bool pause_menu_open);

typedef struct {
    atomic_int pacing_fps, battery_fps, pause_menu_fps; // FramePacingFPS, BatteryFPS and PauseMenuFPS, 0 = off
    atomic_bool on_battery;
    _Atomic uint64_t pause_menu_until; // when the pause menu stops counting as open (nanoseconds of a clock that only
                                       // goes forward), 0 while it's closed
    bool pause_menu_logged;            // the render thread's: the log last said the pause menu's limit is on
} frame_limits_t;

// from the engine thread: notices.lua says the pause menu is open in a match (four times a second while it is), or
// that it closed. A new UI Lua state closes it too.
void frame_limit_set_pause_menu(frame_limits_t *limits, bool open, uint64_t now);

// from the render thread, for each frame: the frame rate to pace it at, 0 for none. *log is 1 when the pause menu's
// limit starts applying (it opened in a match, or PauseMenuFPS was turned on with it open), -1 when it stops because
// the menu closed or stopped counting as open, 0 otherwise (also when PauseMenuFPS is turned off with the menu open,
// which the settings page logs).
int frame_limit_for_frame(frame_limits_t *limits, uint64_t now, int *log);
