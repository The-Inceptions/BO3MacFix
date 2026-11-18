// BatterySaver: the battery's levers as one setting. Measured on 2026-11-18 (wip/night/perf-battery2.md; on mains, the
// lid closed, the GPU's own energy counter, a still scene, every pair alternated inside one run): at the battery limit
// of 40 fps DEIMOS's GPU drew 10-11.4 W at the full resolution, 7.4 W at 85% and 4.8 W at 70%, 7.0 W at 30 fps; Kino
// 3.2-3.3 W, 2.6 W, 2.1 W and 2.2 W. A paused game behind another app drew 7.4-10.3 W of GPU at BackgroundFPS 30 and
// 2.6 W at 10 (Kino). The levels:
//   Saver:   40 fps, the 3D scene at 85% at most, 20 fps in an untouched menu (IdleMenuFPS), 15 fps behind another
//            app (BackgroundFPS), the display may sleep when idle (IdleDisplaySleep 1)
//   Longest: 30 fps, 70%, 15 fps in an untouched menu, 10 fps behind another app, the same
// (The untouched menu's and the display's parts only act with ControllerBridge, as those settings themselves do. The
// menu's rate stays at 15 at the least: at 10 a controller is read every 100 ms, the review of IdleMenuFPS, M2.)
// Each only ever adds saving to what the user set: where the user's own value saves more (a lower limit), it stays.
// Plugged in, nothing of it applies (BatteryFPS and BatteryResolution only apply on battery anyway; the rest is only
// changed while on battery).
#include "battery_saver.h"

static const battery_settings_t levels[] = {
    { 0, 0, 0, 0, 0 },
    { 40, 85, 20, 15, 1 },
    { 30, 70, 15, 10, 1 },
};

battery_settings_t battery_saver_level(int level) {
    return level >= Battery_Saver_Saver && level <= Battery_Saver_Longest ? levels[level] : levels[0];
}

// the one of two values (0 = off) that saves more: the lower, and any against off
static int stronger(int user, int level) {
    if (level == 0)
        return user;
    if (user == 0)
        return level;
    return user < level ? user : level;
}

battery_settings_t battery_saver_effective(int level, bool on_battery, battery_settings_t user) {
    // off (or a level it doesn't know): all of the level's values are 0, so the user's own stay
    battery_settings_t l = battery_saver_level(level), e = user;
    e.battery_fps = stronger(user.battery_fps, l.battery_fps);
    e.battery_resolution = stronger(user.battery_resolution, l.battery_resolution);
    if (on_battery) {
        e.idle_menu_fps = stronger(user.idle_menu_fps, l.idle_menu_fps);
        e.background_fps = stronger(user.background_fps, l.background_fps);
    }
    // 1 lets the display sleep on battery only, so it changes nothing plugged in; 2 (always) stays
    if (user.idle_display_sleep == 0)
        e.idle_display_sleep = l.idle_display_sleep;
    return e;
}
