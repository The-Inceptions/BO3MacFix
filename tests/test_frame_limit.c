// Tests for frame_limit.c: which frame rate limit applies to a frame, and when the pause menu's limit (PauseMenuFPS)
// starts, stops and is logged.
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
    CHECK(frame_limit_for_frame(&limits, Second, &log) == 0 && log == 0);
    // opens in a match: limited from the next frame, logged once
    frame_limit_set_pause_menu(&limits, true, Second);
    CHECK(frame_limit_for_frame(&limits, Second + Frame, &log) == 30 && log == 1);
    CHECK(frame_limit_for_frame(&limits, Second + 2 * Frame, &log) == 30 && log == 0);
    // told again every quarter second (a little late): stays open for as long as that goes on
    int logs = 0;
    for (uint64_t t = Second; t < 20 * Second; t += 300000000) {
        frame_limit_set_pause_menu(&limits, true, t);
        for (uint64_t frame = t; frame < t + 300000000; frame += Frame) {
            CHECK(frame_limit_for_frame(&limits, frame, &log) == 30);
            logs += log != 0;
        }
    }
    CHECK(logs == 0);
    // closes: unlimited from the next frame, logged once
    frame_limit_set_pause_menu(&limits, false, 20 * Second);
    CHECK(frame_limit_for_frame(&limits, 20 * Second + Frame, &log) == 0 && log == -1);
    CHECK(frame_limit_for_frame(&limits, 20 * Second + 2 * Frame, &log) == 0 && log == 0);
    // opened and closed again between two frames: nothing to log
    frame_limit_set_pause_menu(&limits, true, 21 * Second);
    frame_limit_set_pause_menu(&limits, false, 21 * Second);
    CHECK(frame_limit_for_frame(&limits, 21 * Second + Frame, &log) == 0 && log == 0);
}

// the UI goes away with the menu open, without saying it closed (a map loads, the game quits to the main menu)
static void test_pause_menu_timeout(void) {
    frame_limits_t limits = { 0 };
    atomic_store(&limits.pause_menu_fps, 30);
    int log;
    frame_limit_set_pause_menu(&limits, true, 5 * Second);
    CHECK(frame_limit_for_frame(&limits, 5 * Second, &log) == 30 && log == 1);
    CHECK(frame_limit_for_frame(&limits, 5 * Second + Pause_Menu_Timeout_NS - 1, &log) == 30 && log == 0);
    CHECK(frame_limit_for_frame(&limits, 5 * Second + Pause_Menu_Timeout_NS, &log) == 0 && log == -1);
    CHECK(frame_limit_for_frame(&limits, 60 * Second, &log) == 0 && log == 0);
    // told it's open again after that: limited and logged again
    frame_limit_set_pause_menu(&limits, true, 61 * Second);
    CHECK(frame_limit_for_frame(&limits, 61 * Second, &log) == 30 && log == 1);
    // a new UI Lua state closes it right away
    frame_limit_set_pause_menu(&limits, false, 61 * Second + Frame);
    CHECK(frame_limit_for_frame(&limits, 61 * Second + 2 * Frame, &log) == 0 && log == -1);
}

// PauseMenuFPS changed from the settings page (F10 in the pause menu), and the other limits
static void test_pause_menu_settings(void) {
    frame_limits_t limits = { 0 };
    int log;
    // off: the menu being open changes nothing and says nothing
    frame_limit_set_pause_menu(&limits, true, Second);
    CHECK(frame_limit_for_frame(&limits, Second, &log) == 0 && log == 0);
    // turned on with it open: limited and logged
    atomic_store(&limits.pause_menu_fps, 30);
    CHECK(frame_limit_for_frame(&limits, Second + Frame, &log) == 30 && log == 1);
    // changed: the new rate, nothing logged
    atomic_store(&limits.pause_menu_fps, 40);
    CHECK(frame_limit_for_frame(&limits, Second + 2 * Frame, &log) == 40 && log == 0);
    // turned off with it open: unlimited, but it isn't the menu closing
    atomic_store(&limits.pause_menu_fps, 0);
    CHECK(frame_limit_for_frame(&limits, Second + 3 * Frame, &log) == 0 && log == 0);
    frame_limit_set_pause_menu(&limits, false, Second + 3 * Frame);
    CHECK(frame_limit_for_frame(&limits, Second + 4 * Frame, &log) == 0 && log == 0);

    // with FramePacingFPS: the pause menu's 30 while it's open, then back to pacing at 60
    atomic_store(&limits.pause_menu_fps, 30);
    atomic_store(&limits.pacing_fps, 60);
    CHECK(frame_limit_for_frame(&limits, 2 * Second, &log) == 60 && log == 0);
    frame_limit_set_pause_menu(&limits, true, 2 * Second);
    CHECK(frame_limit_for_frame(&limits, 2 * Second + Frame, &log) == 30 && log == 1);
    frame_limit_set_pause_menu(&limits, false, 2 * Second + Frame);
    CHECK(frame_limit_for_frame(&limits, 2 * Second + 2 * Frame, &log) == 60 && log == -1);
    // a lower BatteryFPS on battery wins over it, though it still says the menu's limit is on
    atomic_store(&limits.pacing_fps, 0);
    atomic_store(&limits.pause_menu_fps, 60);
    atomic_store(&limits.battery_fps, 45);
    atomic_store(&limits.on_battery, true);
    frame_limit_set_pause_menu(&limits, true, 3 * Second);
    CHECK(frame_limit_for_frame(&limits, 3 * Second, &log) == 45 && log == 1);
    atomic_store(&limits.on_battery, false);
    CHECK(frame_limit_for_frame(&limits, 3 * Second + Frame, &log) == 60 && log == 0);
}

int main(void) {
    test_lowest_limit();
    test_pause_menu_open_and_close();
    test_pause_menu_timeout();
    test_pause_menu_settings();
    if (failures == 0)
        printf("test_frame_limit: all passed\n");
    return failures == 0 ? 0 : 1;
}
