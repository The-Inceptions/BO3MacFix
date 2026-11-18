// Tests for battery_saver.c: what BatterySaver's levels set on battery and plugged in, against the user's own settings.
#include <stdio.h>
#include "../source/battery_saver.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool same(battery_settings_t a, battery_settings_t b) {
    return a.battery_fps == b.battery_fps && a.battery_resolution == b.battery_resolution &&
           a.idle_menu_fps == b.idle_menu_fps && a.background_fps == b.background_fps &&
           a.idle_display_sleep == b.idle_display_sleep;
}

// off, or an unknown level: the user's own settings, on battery and plugged in
static void test_off(void) {
    battery_settings_t user = { 40, 0, 0, 30, 0 };
    CHECK(same(battery_saver_effective(Battery_Saver_Off, true, user), user));
    CHECK(same(battery_saver_effective(Battery_Saver_Off, false, user), user));
    CHECK(same(battery_saver_effective(3, true, user), user));
    CHECK(same(battery_saver_effective(-1, true, user), user));
    CHECK(same(battery_saver_level(Battery_Saver_Off), (battery_settings_t){ 0, 0, 0, 0, 0 }));
}

// the levels on battery, over the defaults (BatteryFPS 0, BatteryResolution 0, IdleMenuFPS 0, BackgroundFPS 30,
// IdleDisplaySleep 0)
static void test_levels_on_battery(void) {
    battery_settings_t defaults = { 0, 0, 0, 30, 0 };
    CHECK(same(battery_saver_effective(Battery_Saver_Saver, true, defaults), (battery_settings_t){ 40, 85, 20, 15, 1 }));
    CHECK(same(battery_saver_effective(Battery_Saver_Longest, true, defaults), (battery_settings_t){ 30, 70, 15, 10, 1 }));
    CHECK(same(battery_saver_level(Battery_Saver_Saver), (battery_settings_t){ 40, 85, 20, 15, 1 }));
    CHECK(same(battery_saver_level(Battery_Saver_Longest), (battery_settings_t){ 30, 70, 15, 10, 1 }));
}

// plugged in: the menus' and the background's limits stay the user's; BatteryFPS and BatteryResolution carry the
// level's (they only apply on battery), and IdleDisplaySleep 1 (on battery only)
static void test_plugged_in(void) {
    battery_settings_t defaults = { 0, 0, 0, 30, 0 };
    CHECK(same(battery_saver_effective(Battery_Saver_Longest, false, defaults), (battery_settings_t){ 30, 70, 0, 30, 1 }));
    battery_settings_t user = { 0, 0, 20, 60, 0 };
    CHECK(same(battery_saver_effective(Battery_Saver_Saver, false, user), (battery_settings_t){ 40, 85, 20, 60, 1 }));
}

// the user's own values that save more stay; the level's apply where they save more; 0 is off, not the lowest
static void test_stronger(void) {
    battery_settings_t user = { 30, 60, 10, 10, 0 }; // all stronger than Saver's
    CHECK(same(battery_saver_effective(Battery_Saver_Saver, true, user), (battery_settings_t){ 30, 60, 10, 10, 1 }));
    battery_settings_t weaker = { 60, 95, 30, 60, 0 }; // all weaker than Longest's
    CHECK(same(battery_saver_effective(Battery_Saver_Longest, true, weaker), (battery_settings_t){ 30, 70, 15, 10, 1 }));
    battery_settings_t mixed = { 30, 0, 25, 0, 0 }; // off counts as weakest
    CHECK(same(battery_saver_effective(Battery_Saver_Saver, true, mixed), (battery_settings_t){ 30, 85, 20, 15, 1 }));
    // the user's IdleDisplaySleep: 2 (always) stays 2, 1 stays 1
    battery_settings_t always = { 0, 0, 0, 30, 2 };
    CHECK(battery_saver_effective(Battery_Saver_Saver, true, always).idle_display_sleep == 2);
    CHECK(battery_saver_effective(Battery_Saver_Saver, false, always).idle_display_sleep == 2);
    battery_settings_t own = { 0, 0, 0, 30, 1 };
    CHECK(battery_saver_effective(Battery_Saver_Longest, true, own).idle_display_sleep == 1);
}

int main(void) {
    test_off();
    test_levels_on_battery();
    test_plugged_in();
    test_stronger();
    if (failures == 0)
        printf("test_battery_saver: all passed\n");
    return failures == 0 ? 0 : 1;
}
