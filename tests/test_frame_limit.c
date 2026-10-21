// Tests for frame_limit.c: which frame rate limit applies to a frame, and when the pause menu's limit (PauseMenuFPS),
// the menus' (MenuFPS) and the background's (BackgroundFPS) start, stop and are logged.
#include <stdio.h>
#include "../source/frame_limit.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Second 1000000000ULL
#define Frame (Second / 120)

// FramePacingFPS, BatteryFPS and PauseMenuFPS as the render thread puts them together
static int frame_limit_fps(int pacing_fps, int battery_fps, bool on_battery, int pause_menu_fps, bool pause_menu_open) {
    frame_limit_t limits[] = { { pacing_fps, true }, { battery_fps, on_battery }, { pause_menu_fps, pause_menu_open } };
    return frame_limit_lowest(limits, 3);
}

// frame_limit_for_frame with the pause menu's logging as 1 (starts), -1 (stops) or 0, checking that no other limit
// logs anything
static int for_frame(frame_limits_t *limits, uint64_t now, int *log) {
    int started, stopped;
    int fps = frame_limit_for_frame(limits, now, &started, &stopped);
    CHECK(((started | stopped) & ~Limit_Pause_Menu) == 0);
    *log = (started & Limit_Pause_Menu) ? 1 : (stopped & Limit_Pause_Menu) ? -1 : 0;
    return fps;
}

static void test_lowest_limit(void) {
    CHECK(frame_limit_fps(0, 0, false, 0, false) == 0);
    CHECK(frame_limit_fps(60, 0, false, 0, false) == 60);
    // BatteryFPS only on battery
    CHECK(frame_limit_fps(0, 45, false, 0, false) == 0);
    CHECK(frame_limit_fps(0, 45, true, 0, false) == 45);
    CHECK(frame_limit_fps(60, 45, true, 0, false) == 45);
    CHECK(frame_limit_fps(30, 45, true, 0, false) == 30);
    // PauseMenuFPS only with the pause menu open
    CHECK(frame_limit_fps(0, 0, false, 30, false) == 0);
    CHECK(frame_limit_fps(120, 0, false, 30, false) == 120);
    CHECK(frame_limit_fps(0, 0, false, 30, true) == 30);
    CHECK(frame_limit_fps(120, 0, false, 30, true) == 30);
    CHECK(frame_limit_fps(120, 45, true, 30, true) == 30);
    CHECK(frame_limit_fps(120, 45, false, 60, true) == 60);
    // a lower limit already applies
    CHECK(frame_limit_fps(30, 0, false, 60, true) == 30);
    CHECK(frame_limit_fps(0, 30, true, 40, true) == 30);
    // off (0) isn't the lowest
    CHECK(frame_limit_fps(0, 0, true, 60, true) == 60);
    CHECK(frame_limit_fps(90, 0, true, 0, true) == 90);
}

static void test_pause_menu_open_and_close(void) {
    frame_limits_t limits = { 0 };
    atomic_store(&limits.pause_menu_fps, 30);
    int log;
    CHECK(for_frame(&limits, Second, &log) == 0 && log == 0);
    // opens in a match: limited from the next frame, logged once
    frame_limit_set_pause_menu(&limits, true, Second);
    CHECK(for_frame(&limits, Second + Frame, &log) == 30 && log == 1);
    CHECK(for_frame(&limits, Second + 2 * Frame, &log) == 30 && log == 0);
    // told again every quarter second (a little late): stays open for as long as that goes on
    int logs = 0;
    for (uint64_t t = Second; t < 20 * Second; t += 300000000) {
        frame_limit_set_pause_menu(&limits, true, t);
        for (uint64_t frame = t; frame < t + 300000000; frame += Frame) {
            CHECK(for_frame(&limits, frame, &log) == 30);
            logs += log != 0;
        }
    }
    CHECK(logs == 0);
    // closes: unlimited from the next frame, logged once
    frame_limit_set_pause_menu(&limits, false, 20 * Second);
    CHECK(for_frame(&limits, 20 * Second + Frame, &log) == 0 && log == -1);
    CHECK(for_frame(&limits, 20 * Second + 2 * Frame, &log) == 0 && log == 0);
    // opened and closed again between two frames: nothing to log
    frame_limit_set_pause_menu(&limits, true, 21 * Second);
    frame_limit_set_pause_menu(&limits, false, 21 * Second);
    CHECK(for_frame(&limits, 21 * Second + Frame, &log) == 0 && log == 0);
}

// the UI goes away with the menu open, without saying it closed (a map loads, the game quits to the main menu)
static void test_pause_menu_timeout(void) {
    frame_limits_t limits = { 0 };
    atomic_store(&limits.pause_menu_fps, 30);
    int log;
    frame_limit_set_pause_menu(&limits, true, 5 * Second);
    CHECK(for_frame(&limits, 5 * Second, &log) == 30 && log == 1);
    CHECK(for_frame(&limits, 5 * Second + Pause_Menu_Timeout_NS - 1, &log) == 30 && log == 0);
    CHECK(for_frame(&limits, 5 * Second + Pause_Menu_Timeout_NS, &log) == 0 && log == -1);
    CHECK(for_frame(&limits, 60 * Second, &log) == 0 && log == 0);
    // told it's open again after that: limited and logged again
    frame_limit_set_pause_menu(&limits, true, 61 * Second);
    CHECK(for_frame(&limits, 61 * Second, &log) == 30 && log == 1);
    // a new UI Lua state closes it right away
    frame_limit_set_pause_menu(&limits, false, 61 * Second + Frame);
    CHECK(for_frame(&limits, 61 * Second + 2 * Frame, &log) == 0 && log == -1);
}

// PauseMenuFPS changed from the settings page (F10 in the pause menu), and the other limits
static void test_pause_menu_settings(void) {
    frame_limits_t limits = { 0 };
    int log;
    // off: the menu being open changes nothing and says nothing
    frame_limit_set_pause_menu(&limits, true, Second);
    CHECK(for_frame(&limits, Second, &log) == 0 && log == 0);
    // turned on with it open: limited and logged
    atomic_store(&limits.pause_menu_fps, 30);
    CHECK(for_frame(&limits, Second + Frame, &log) == 30 && log == 1);
    // changed: the new rate, nothing logged
    atomic_store(&limits.pause_menu_fps, 40);
    CHECK(for_frame(&limits, Second + 2 * Frame, &log) == 40 && log == 0);
    // turned off with it open: unlimited, but it isn't the menu closing
    atomic_store(&limits.pause_menu_fps, 0);
    CHECK(for_frame(&limits, Second + 3 * Frame, &log) == 0 && log == 0);
    frame_limit_set_pause_menu(&limits, false, Second + 3 * Frame);
    CHECK(for_frame(&limits, Second + 4 * Frame, &log) == 0 && log == 0);

    // with FramePacingFPS: the pause menu's 30 while it's open, then back to pacing at 60
    atomic_store(&limits.pause_menu_fps, 30);
    atomic_store(&limits.pacing_fps, 60);
    CHECK(for_frame(&limits, 2 * Second, &log) == 60 && log == 0);
    frame_limit_set_pause_menu(&limits, true, 2 * Second);
    CHECK(for_frame(&limits, 2 * Second + Frame, &log) == 30 && log == 1);
    frame_limit_set_pause_menu(&limits, false, 2 * Second + Frame);
    CHECK(for_frame(&limits, 2 * Second + 2 * Frame, &log) == 60 && log == -1);
    // a lower BatteryFPS on battery wins over it, though it still says the menu's limit is on
    atomic_store(&limits.pacing_fps, 0);
    atomic_store(&limits.pause_menu_fps, 60);
    atomic_store(&limits.battery_fps, 45);
    atomic_store(&limits.on_battery, true);
    frame_limit_set_pause_menu(&limits, true, 3 * Second);
    CHECK(for_frame(&limits, 3 * Second, &log) == 45 && log == 1);
    atomic_store(&limits.on_battery, false);
    CHECK(for_frame(&limits, 3 * Second + Frame, &log) == 60 && log == 0);
}

static void test_lowest(void) {
    frame_limit_t none[] = { { 0, true }, { 60, false } };
    CHECK(frame_limit_lowest(none, 2) == 0);
    CHECK(frame_limit_lowest(none, 0) == 0);
    frame_limit_t some[] = { { 0, true }, { 120, true }, { 15, false }, { 60, true }, { 30, false } };
    CHECK(frame_limit_lowest(some, 5) == 60);
    some[4].applies = true;
    CHECK(frame_limit_lowest(some, 5) == 30);
}

// MenuFPS: notices.lua says the game is in the menus four times a second while it is, and once when it isn't
static void test_menus(void) {
    frame_limits_t limits = { 0 };
    atomic_store(&limits.menu_fps, 60);
    int started, stopped;
    CHECK(frame_limit_for_frame(&limits, Second, &started, &stopped) == 0 && started == 0 && stopped == 0);
    frame_limit_set_menus(&limits, true, Second);
    CHECK(frame_limit_for_frame(&limits, Second + Frame, &started, &stopped) == 60 && started == Limit_Menus &&
          stopped == 0);
    int logs = 0;
    for (uint64_t t = Second; t < 30 * Second; t += 250000000) {
        frame_limit_set_menus(&limits, true, t);
        for (uint64_t frame = t; frame < t + 250000000; frame += Frame) {
            CHECK(frame_limit_for_frame(&limits, frame, &started, &stopped) == 60);
            logs += started != 0 || stopped != 0;
        }
    }
    CHECK(logs == 0);
    // a match starts: the menus' UI state goes away (a new one closes it), then the limit stops
    frame_limit_set_menus(&limits, false, 30 * Second);
    CHECK(frame_limit_for_frame(&limits, 30 * Second + Frame, &started, &stopped) == 0 && started == 0 &&
          stopped == Limit_Menus);
    CHECK(frame_limit_for_frame(&limits, 30 * Second + 2 * Frame, &started, &stopped) == 0 && stopped == 0);
    // the UI stops saying so without saying it ended (a map loading): it ends 2 s after the last time
    frame_limit_set_menus(&limits, true, 40 * Second);
    CHECK(frame_limit_for_frame(&limits, 40 * Second, &started, &stopped) == 60 && started == Limit_Menus);
    CHECK(frame_limit_for_frame(&limits, 40 * Second + Pause_Menu_Timeout_NS - 1, &started, &stopped) == 60);
    CHECK(frame_limit_for_frame(&limits, 40 * Second + Pause_Menu_Timeout_NS, &started, &stopped) == 0 &&
          stopped == Limit_Menus);
    // MenuFPS turned off in the menus: unlimited, nothing logged (the settings page does), and on again: logged
    frame_limit_set_menus(&limits, true, 50 * Second);
    CHECK(frame_limit_for_frame(&limits, 50 * Second, &started, &stopped) == 60 && started == Limit_Menus);
    atomic_store(&limits.menu_fps, 0);
    CHECK(frame_limit_for_frame(&limits, 50 * Second + Frame, &started, &stopped) == 0 && started == 0 &&
          stopped == 0);
    atomic_store(&limits.menu_fps, 30);
    CHECK(frame_limit_for_frame(&limits, 50 * Second + 2 * Frame, &started, &stopped) == 30 &&
          started == Limit_Menus && stopped == 0);
    // with FramePacingFPS 120 the menus' 30 wins, and a lower FramePacingFPS wins over a higher MenuFPS
    atomic_store(&limits.pacing_fps, 120);
    CHECK(frame_limit_for_frame(&limits, 50 * Second + 3 * Frame, &started, &stopped) == 30);
    atomic_store(&limits.pacing_fps, 30);
    atomic_store(&limits.menu_fps, 60);
    CHECK(frame_limit_for_frame(&limits, 50 * Second + 4 * Frame, &started, &stopped) == 30);
    // never in a match: the pause menu's limit and the menus' are separate
    atomic_store(&limits.pacing_fps, 0);
    frame_limit_set_menus(&limits, false, 51 * Second);
    CHECK(frame_limit_for_frame(&limits, 51 * Second, &started, &stopped) == 0 && stopped == Limit_Menus);
    atomic_store(&limits.pause_menu_fps, 40);
    frame_limit_set_pause_menu(&limits, true, 52 * Second);
    CHECK(frame_limit_for_frame(&limits, 52 * Second, &started, &stopped) == 40 && started == Limit_Pause_Menu);
}

// BackgroundFPS: while the game isn't the active app, or is hidden or minimized, with a UI polling (the menus or a match)
static void test_background(void) {
    frame_limits_t limits = { 0 };
    int started, stopped;
    // off: the background changes nothing and says nothing
    atomic_store(&limits.in_background, true);
    frame_limit_set_menus(&limits, false, Second); // a match's UI polls
    CHECK(frame_limit_for_frame(&limits, Second, &started, &stopped) == 0 && started == 0 && stopped == 0);
    // turned on in the background: limited and logged
    atomic_store(&limits.background_fps, 15);
    CHECK(frame_limit_for_frame(&limits, Second + Frame, &started, &stopped) == 15 && started == Limit_Background);
    CHECK(frame_limit_for_frame(&limits, Second + 2 * Frame, &started, &stopped) == 15 && started == 0);
    // back in front: unlimited, logged
    atomic_store(&limits.in_background, false);
    CHECK(frame_limit_for_frame(&limits, Second + 3 * Frame, &started, &stopped) == 0 && stopped == Limit_Background);
    // in the menus, then in the background: the lower background limit while it lasts, then the menus' again
    atomic_store(&limits.menu_fps, 60);
    frame_limit_set_menus(&limits, true, 2 * Second);
    CHECK(frame_limit_for_frame(&limits, 2 * Second, &started, &stopped) == 60 && started == Limit_Menus);
    atomic_store(&limits.in_background, true);
    CHECK(frame_limit_for_frame(&limits, 2 * Second + Frame, &started, &stopped) == 15 &&
          started == Limit_Background && stopped == 0);
    atomic_store(&limits.in_background, false);
    CHECK(frame_limit_for_frame(&limits, 2 * Second + 2 * Frame, &started, &stopped) == 60 && started == 0 &&
          stopped == Limit_Background);
    // with the pause menu open in a match and BatteryFPS 45 on battery: the lowest of them all
    frame_limit_set_menus(&limits, false, 3 * Second);
    atomic_store(&limits.pause_menu_fps, 30);
    atomic_store(&limits.battery_fps, 45);
    atomic_store(&limits.on_battery, true);
    frame_limit_set_pause_menu(&limits, true, 3 * Second);
    CHECK(frame_limit_for_frame(&limits, 3 * Second, &started, &stopped) == 30 && started == Limit_Pause_Menu &&
          stopped == Limit_Menus);
    atomic_store(&limits.in_background, true);
    CHECK(frame_limit_for_frame(&limits, 3 * Second + Frame, &started, &stopped) == 15 &&
          started == Limit_Background);
    atomic_store(&limits.background_fps, 60);
    CHECK(frame_limit_for_frame(&limits, 3 * Second + 2 * Frame, &started, &stopped) == 30 && started == 0);
    // in a match with no situational limit: only what the user set
    frame_limit_set_pause_menu(&limits, false, 4 * Second);
    atomic_store(&limits.in_background, false);
    atomic_store(&limits.on_battery, false);
    CHECK(frame_limit_for_frame(&limits, 4 * Second, &started, &stopped) == 0 &&
          stopped == (Limit_Pause_Menu | Limit_Background));
}

// BackgroundFPS waits for a UI to poll: not while the game starts, nor while a map loads
static void test_background_not_loading(void) {
    frame_limits_t limits = { 0 };
    int started, stopped;
    atomic_store(&limits.background_fps, 15);
    atomic_store(&limits.menu_fps, 30);
    atomic_store(&limits.in_background, true);
    // the game starting behind other apps: no UI yet
    CHECK(frame_limit_for_frame(&limits, Second, &started, &stopped) == 0 && started == 0);
    // the menus are up: the background's 15
    frame_limit_set_menus(&limits, true, 2 * Second);
    CHECK(frame_limit_for_frame(&limits, 2 * Second, &started, &stopped) == 15 &&
          started == (Limit_Menus | Limit_Background));
    // a map starts loading from the menus (a new UI state): neither limit while it loads
    frame_limit_ui_gone(&limits);
    CHECK(frame_limit_for_frame(&limits, 2 * Second + Frame, &started, &stopped) == 0 &&
          stopped == (Limit_Menus | Limit_Background));
    CHECK(frame_limit_for_frame(&limits, 30 * Second, &started, &stopped) == 0 && started == 0);
    // the match's UI polls: the background's 15 again, not the menus'
    frame_limit_set_menus(&limits, false, 31 * Second);
    CHECK(frame_limit_for_frame(&limits, 31 * Second, &started, &stopped) == 15 && started == Limit_Background);
    // the polls stop without a new UI state (a hitch, or a load that keeps the old state): 2 s after the last one
    CHECK(frame_limit_for_frame(&limits, 31 * Second + Pause_Menu_Timeout_NS - 1, &started, &stopped) == 15);
    CHECK(frame_limit_for_frame(&limits, 31 * Second + Pause_Menu_Timeout_NS, &started, &stopped) == 0 &&
          stopped == Limit_Background);
}

// whether a match runs (DynamicResolution warms up there): the UI polls and doesn't say the game is in the menus
static void test_in_match(void) {
    frame_limits_t limits = { 0 };
    CHECK(!frame_limit_in_match(&limits, Second)); // the game starting: no UI yet
    frame_limit_set_menus(&limits, true, 2 * Second);
    CHECK(!frame_limit_in_match(&limits, 2 * Second)); // the menus
    frame_limit_ui_gone(&limits);
    CHECK(!frame_limit_in_match(&limits, 3 * Second)); // a map loading
    frame_limit_set_menus(&limits, false, 20 * Second);
    CHECK(frame_limit_in_match(&limits, 20 * Second)); // the match's UI polls
    CHECK(!frame_limit_in_match(&limits, 20 * Second + Pause_Menu_Timeout_NS)); // its polls stopped
}

int main(void) {
    test_lowest();
    test_lowest_limit();
    test_pause_menu_open_and_close();
    test_pause_menu_timeout();
    test_pause_menu_settings();
    test_menus();
    test_background();
    test_background_not_loading();
    test_in_match();
    if (failures == 0)
        printf("test_frame_limit: all passed\n");
    return failures == 0 ? 0 : 1;
}
