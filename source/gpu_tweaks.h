#ifndef _GPU_TWEAKS_H
#define _GPU_TWEAKS_H

#include <stdbool.h>
#include <stdint.h>

// checked_code_patch from bo3macfix.c
typedef void (*code_patch_fn)(uint32_t address, const uint8_t *expected, uint8_t *patch, uint32_t size);

// frames_in_flight: 1 to 3 (2 leaves Aspyr's default), pacing_fps: 30 to 240 (0 leaves it off)
void gpu_tweaks_init(int frames_in_flight, int pacing_fps, code_patch_fn checked_code_patch);

// metalfx_upscale: 1 upscales to the screen's native size with MetalFX when the game runs at a lower resolution, 2 the
// same but to the panel's resolution when the display is in a scaled mode, 0 = off
void gpu_tweaks_metalfx_init(int metalfx_upscale, code_patch_fn checked_code_patch);

// seconds: how often to log the frame rate, 1% low and worst frame time, 0 = off
void gpu_tweaks_frame_stats_init(int seconds, code_patch_fn checked_code_patch);

// fps: the frame rate limit while on battery, 30 to 240 (0 = off)
void gpu_tweaks_battery_fps_init(int fps, code_patch_fn checked_code_patch);
// whether the Mac runs on battery, as BatteryFPS sees it (checked every 5 s from the first call; BO3MACFIX_ONBATTERY=1
// pretends it does), for AAEEngineDefaults' battery choice
bool gpu_tweaks_on_battery(void);
// fps: the frame rate limit while macOS's Low Power Mode is on, 30 to 240 (0 = off)
void gpu_tweaks_low_power_fps_init(int fps, code_patch_fn checked_code_patch);

// setting: BatteryResolution, the most the 3D scene's resolution may be on battery as a share of ResolutionPercent, 50 to
// 95 (0 = off), through DynamicResolution's controller as its ceiling (dynres.h)
void gpu_tweaks_battery_resolution_init(int setting, code_patch_fn checked_code_patch);

// fps: the frame rate limit while the pause menu is open in a match, 30 to 240 (0 = off)
void gpu_tweaks_pause_menu_fps_init(int fps, code_patch_fn checked_code_patch);

// from the engine thread (notices.m): whether the pause menu is open in a match. notices.lua says it is four times a
// second while it is, and when it closes; it also counts as closed 2 s after the last time it was said to be open.
void gpu_tweaks_pause_menu_open(bool open);

// fps: the frame rate limit in the menus outside a match (the main menu, the lobby, the mods menu), 30 to 240 (0 = off)
void gpu_tweaks_menu_fps_init(int fps, code_patch_fn checked_code_patch);

// fps: IdleMenuFPS, the frame rate limit while the pause menu or the menus have been left untouched for
// Idle_Menu_Seconds (no key, mouse, trackpad or controller input), 5 to 60 (0 = off)
void gpu_tweaks_idle_menu_fps_init(int fps, code_patch_fn checked_code_patch);

// from the controller code (gc_bridge.m) for each state the game reads: a player's input there ends IdleMenuFPS's idle
void gpu_tweaks_controller_input(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, int16_t left_x,
                                 int16_t left_y, int16_t right_x, int16_t right_y);

// from the engine thread (notices.m): whether the game is in the menus, outside a match, four times a second while a
// UI Lua state polls (notices.lua). It ends 2 s after the last poll that said so; BackgroundFPS waits for the polls too,
// so it doesn't hold back the game starting or a map loading.
void gpu_tweaks_menus_open(bool in_menus);

// from the engine thread (notices.m): a new UI Lua state is set up, which a map loading makes: the menus' limit and
// BackgroundFPS stop until it polls
void gpu_tweaks_ui_gone(void);

// fps: the frame rate limit while the game isn't the active app, or is hidden or minimized, with the menus or a match
// up, 10 to 240 (0 = off)
void gpu_tweaks_background_fps_init(int fps, code_patch_fn checked_code_patch);

// BatterySaver (battery_saver.h): 0 off, 1 Saver, 2 Longest: while on battery, the battery settings at the level's
// values where those save more than the user's own; plugged in, nothing of it. After the settings it sets are read.
void gpu_tweaks_battery_saver_init(int level, code_patch_fn checked_code_patch);

// IdleDisplaySleep (display_sleep.h): 0 the game keeps the display on as it always did, 1 the display may turn off
// while the game is paused, in the menus or in the background on battery, 2 the same always
void gpu_tweaks_idle_display_sleep_init(int mode);

// setting: DynamicResolution, the lowest share of ResolutionPercent to go down to while the GPU can't keep up with the
// display's refresh rate, 50 to 95 (0 = off), see dynres.h
void gpu_tweaks_dynres_init(int setting, code_patch_fn checked_code_patch);

// the user's ResolutionPercent while DynamicResolution holds the dvar at another value, else 0: what config.ini
// gets (engine_display.c's SyncHardwareProfileWithDvars hook)
int gpu_tweaks_dynres_held_base(void);

// turns on macOS's Metal performance HUD (FPS and GPU time) when enabled
void gpu_tweaks_metal_hud_init(int enabled);

// changes from the settings menu while the game runs: FramePacingFPS, BatteryFPS, LowPowerFPS, PauseMenuFPS, MenuFPS,
// BackgroundFPS and FrameStats. Each
// returns false when it needs a restart (turning it on needs present hooked, which only the pre-patched executable can
// do late).
bool gpu_tweaks_live_possible(void);
bool gpu_tweaks_set_pacing(int fps);
bool gpu_tweaks_set_battery_fps(int fps);
bool gpu_tweaks_set_low_power_fps(int fps);
bool gpu_tweaks_set_battery_resolution(int setting);
bool gpu_tweaks_set_pause_menu_fps(int fps);
bool gpu_tweaks_set_menu_fps(int fps);
bool gpu_tweaks_set_idle_menu_fps(int fps);
bool gpu_tweaks_set_background_fps(int fps);
bool gpu_tweaks_set_frame_stats(int seconds);
bool gpu_tweaks_set_dynamic_resolution(int setting);
bool gpu_tweaks_set_idle_display_sleep(int mode);
bool gpu_tweaks_set_battery_saver(int level);

#endif
