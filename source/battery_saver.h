// BatterySaver: one setting for the battery's levers, set together while the Mac runs on battery. See battery_saver.c.
#pragma once
#include <stdbool.h>

enum { Battery_Saver_Off = 0, Battery_Saver_Saver = 1, Battery_Saver_Longest = 2 };

// the settings a level sets, each 0 for off (the same meaning as in settings.ini)
typedef struct {
    int battery_fps;        // BatteryFPS: the frame rate limit on battery
    int battery_resolution; // BatteryResolution: the 3D scene's most on battery, % of ResolutionPercent
    int idle_menu_fps;      // IdleMenuFPS: a pause menu or menu left untouched for 10 s
    int background_fps;     // BackgroundFPS: the game behind another app
    int idle_display_sleep; // IdleDisplaySleep: 1 the display may sleep when idle on battery, 2 always
} battery_settings_t;

// what applies, from the user's own settings and the level: on battery each of the level's values where it saves more
// than the user's (a lower frame rate or resolution, or one where the user's is off), and the display may sleep when
// idle; plugged in, the user's own, except BatteryFPS and BatteryResolution, which only ever apply on battery anyway
// and so may carry the level's values all the time. Never a value that saves less than the user's.
battery_settings_t battery_saver_effective(int level, bool on_battery, battery_settings_t user);

// the level's own values (Battery_Saver_Off: all 0)
battery_settings_t battery_saver_level(int level);
