// The frame rate limits that pace presents (gpu_tweaks.c) as plain C: which one applies to a frame, and when the pause
// menu counts as open in a match for PauseMenuFPS and the game as in the menus for MenuFPS. No game or macOS calls, so
// tests/test_frame_limit.c tests it as is.
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// how long the pause menu counts as open, and the game as in the menus, after notices.lua last said so. It says so
// four times a second while it is (LUI's timers run about 20% late in a match), so a UI that goes away without saying
// it closed (a map loading, quitting) doesn't leave the limit on.
#define Pause_Menu_Timeout_NS 2000000000ULL

// the limits that apply only in some situations, as bits for frame_limit_for_frame's logging
enum {
    Limit_Pause_Menu = 1, // PauseMenuFPS: the pause menu is open in a match
    Limit_Menus = 2,      // MenuFPS: in the menus, outside a match (the main menu, the lobby, the mods menu)
    Limit_Background = 4, // BackgroundFPS: the game isn't the active app, or is hidden or minimized, with the menus
                          // or a match up (not while a map loads)
};

// one frame rate limit: its rate (0 = off) and whether it applies right now
typedef struct {
    int fps;
    bool applies;
} frame_limit_t;

// the frame rate to pace at: the lowest of the limits that apply and are on, 0 when none is
int frame_limit_lowest(const frame_limit_t *limits, int count);

typedef struct {
    // FramePacingFPS, BatteryFPS, PauseMenuFPS, MenuFPS and BackgroundFPS, 0 = off
    atomic_int pacing_fps, battery_fps, pause_menu_fps, menu_fps, background_fps;
    atomic_bool on_battery, in_background;
    // when the pause menu stops counting as open, the game as in the menus, and the UI as up (nanoseconds of a clock
    // that only goes forward), 0 while it isn't. The UI is up while notices.lua polls: in the menus and in a match,
    // not while the game starts or a map loads.
    _Atomic uint64_t pause_menu_until, menus_until, ui_until;
    int logged; // the render thread's: the Limit_* bits the log last said are on
} frame_limits_t;

// from the engine thread: notices.lua says the pause menu is open in a match (four times a second while it is), or
// that it closed. A new UI Lua state closes it too.
void frame_limit_set_pause_menu(frame_limits_t *limits, bool open, uint64_t now);

// from the engine thread, four times a second while a UI Lua state polls (notices.lua): whether the game is in the
// menus, outside a match. That the UI polls at all is what BackgroundFPS waits for.
void frame_limit_set_menus(frame_limits_t *limits, bool in_menus, uint64_t now);

// from the engine thread: a new UI Lua state is set up (a map starts loading, from the menus or a match). The menus
// and the UI end until it polls.
void frame_limit_ui_gone(frame_limits_t *limits);

// whether a match runs at now: the UI polls (notices.lua) and doesn't say the game is in the menus. Not while a map loads.
bool frame_limit_in_match(frame_limits_t *limits, uint64_t now);

// from the render thread, for each frame: the frame rate to pace it at, 0 for none. *started gets the Limit_* bits of
// the limits that start applying with this frame (their situation began, or their setting was turned on in it),
// *stopped those that stop because their situation ended. A limit turned off while its situation lasts is in neither
// (the settings page logs that).
int frame_limit_for_frame(frame_limits_t *limits, uint64_t now, int *started, int *stopped);
