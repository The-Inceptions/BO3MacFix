// Tests for display_sleep.c: when IdleDisplaySleep lets the game's display sleep assertion go and takes it again, and
// when a controller's input is declared as user activity.
#include <stdio.h>
#include "../source/display_sleep.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Second 1000000000ULL

// off: never let go, whatever the situation, and nothing is declared
static void test_off(void) {
    display_sleep_t s = { 0 };
    for (uint64_t t = 1; t < 100; t++)
        CHECK(!display_sleep_decide(&s, Display_Sleep_Off, true, true, true, t * Second));
    CHECK(!display_sleep_activity_due(&s, 100 * Second));
}

// away from play: let go once it has lasted the delay, not before; back in play: held at once
static void test_delay_and_return(void) {
    display_sleep_t s = { 0 };
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, false, false, true, 1 * Second)); // playing
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 10 * Second)); // the pause menu opens
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 10 * Second + Display_Sleep_Delay_NS - 1));
    CHECK(display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 10 * Second + Display_Sleep_Delay_NS));
    CHECK(display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 100 * Second)); // stays let go
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, false, false, true, 101 * Second)); // play goes on: at once
    // a short return to play starts the delay again
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 102 * Second));
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, false, false, true, 103 * Second));
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 104 * Second));
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 104 * Second + Display_Sleep_Delay_NS - 1));
    CHECK(display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 104 * Second + Display_Sleep_Delay_NS));
}

// 1 applies on battery only: plugged in, held at once; on battery again, the away time already counted counts
static void test_on_battery(void) {
    display_sleep_t s = { 0 };
    CHECK(!display_sleep_decide(&s, Display_Sleep_On_Battery, false, true, true, 1 * Second));
    CHECK(!display_sleep_decide(&s, Display_Sleep_On_Battery, false, true, true, 30 * Second)); // plugged in: never
    CHECK(!display_sleep_decide(&s, Display_Sleep_On_Battery, true, true, true, 31 * Second)); // unplugged: the delay first
    CHECK(display_sleep_decide(&s, Display_Sleep_On_Battery, true, true, true, 31 * Second + Display_Sleep_Delay_NS));
    CHECK(!display_sleep_decide(&s, Display_Sleep_On_Battery, false, true, true, 40 * Second)); // plugged in: held at once
    // 2 doesn't care about the power source
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 41 * Second));
    CHECK(display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 41 * Second + Display_Sleep_Delay_NS));
    // the setting turned off while let go: held at once
    CHECK(!display_sleep_decide(&s, Display_Sleep_Off, false, true, true, 50 * Second));
}

// a controller's input is declared while let go (and a while after, below), at most every Display_Sleep_Activity_NS
static void test_activity(void) {
    display_sleep_t s = { 0 };
    CHECK(!display_sleep_activity_due(&s, 1 * Second)); // held: macOS doesn't need to hear about it
    display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 2 * Second);
    display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 2 * Second + Display_Sleep_Delay_NS);
    uint64_t t = 10 * Second;
    CHECK(display_sleep_activity_due(&s, t));
    CHECK(!display_sleep_activity_due(&s, t + 1));
    CHECK(!display_sleep_activity_due(&s, t + Display_Sleep_Activity_NS - 1));
    CHECK(display_sleep_activity_due(&s, t + Display_Sleep_Activity_NS));
    CHECK(!display_sleep_activity_due(&s, t + Display_Sleep_Activity_NS + Second));
    // back in play: still declared for Display_Sleep_Wake_NS (a display that went off isn't lit by the assertion),
    // then nothing; once let go again the first input is declared at once if the last one is long enough ago
    display_sleep_decide(&s, Display_Sleep_Always, false, false, true, 30 * Second);
    CHECK(display_sleep_activity_due(&s, 31 * Second));
    CHECK(!display_sleep_activity_due(&s, 30 * Second + Display_Sleep_Wake_NS));
    display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 70 * Second);
    display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 70 * Second + Display_Sleep_Delay_NS);
    CHECK(display_sleep_activity_due(&s, 76 * Second));
}

// what counts as a player's input: buttons, triggers past 30, sticks past 8000 either way; a resting controller doesn't
static void test_is_input(void) {
    CHECK(!display_sleep_is_input(0, 0, 0, 0, 0, 0, 0));
    CHECK(!display_sleep_is_input(0, 30, 30, 8000, -8000, 8000, -8000)); // at the thresholds: still resting
    CHECK(!display_sleep_is_input(0, 12, 3, 400, -350, -600, 250));      // a worn stick's drift
    CHECK(display_sleep_is_input(0x1000, 0, 0, 0, 0, 0, 0));             // A
    CHECK(display_sleep_is_input(0x0001, 0, 0, 0, 0, 0, 0));             // the D-pad
    CHECK(display_sleep_is_input(0, 31, 0, 0, 0, 0, 0));
    CHECK(display_sleep_is_input(0, 0, 31, 0, 0, 0, 0));
    CHECK(display_sleep_is_input(0, 0, 0, 8001, 0, 0, 0));
    CHECK(display_sleep_is_input(0, 0, 0, -8001, 0, 0, 0));
    CHECK(display_sleep_is_input(0, 0, 0, 0, 8001, 0, 0));
    CHECK(display_sleep_is_input(0, 0, 0, 0, -8001, 0, 0));
    CHECK(display_sleep_is_input(0, 0, 0, 0, 0, 8001, 0));
    CHECK(display_sleep_is_input(0, 0, 0, 0, 0, -8001, 0));
    CHECK(display_sleep_is_input(0, 0, 0, 0, 0, 0, 8001));
    CHECK(display_sleep_is_input(0, 0, 0, 0, 0, 0, -32768));
}

// without ControllerBridge a controller's input can't be seen: nothing is let go, whatever the mode (the review of
// 2026-11-18, F1: otherwise the display went dark under a controller player's hands, and no button woke it)
static void test_no_bridge(void) {
    display_sleep_t s = { 0 };
    for (uint64_t t = 1; t < 100; t++)
        CHECK(!display_sleep_decide(&s, Display_Sleep_Always, true, true, false, t * Second));
    CHECK(!display_sleep_activity_due(&s, 100 * Second));
    // the bridge seen again (it is read each time): the usual delay first
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, true, true, true, 101 * Second));
    CHECK(display_sleep_decide(&s, Display_Sleep_Always, true, true, true, 101 * Second + Display_Sleep_Delay_NS));
    // the bridge gone while let go: taken back at once
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, true, true, false, 120 * Second) && s.retaken_at == 120 * Second);
}

// after the assertion is taken back, a controller's input is still declared for Display_Sleep_Wake_NS (the review, F2:
// a display that went off isn't lit by the assertion, and play can go on without this player's input), rate-limited
static void test_wake_window(void) {
    display_sleep_t s = { 0 };
    display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 1 * Second);
    CHECK(display_sleep_decide(&s, Display_Sleep_Always, false, true, true, 1 * Second + Display_Sleep_Delay_NS));
    CHECK(s.retaken_at == 0);
    CHECK(!display_sleep_decide(&s, Display_Sleep_Always, false, false, true, 60 * Second)); // play goes on
    CHECK(s.retaken_at == 60 * Second);
    CHECK(display_sleep_activity_due(&s, 61 * Second));                                  // still declared
    CHECK(!display_sleep_activity_due(&s, 62 * Second));                                 // the rate holds
    CHECK(display_sleep_activity_due(&s, 61 * Second + Display_Sleep_Activity_NS));
    CHECK(!display_sleep_activity_due(&s, 60 * Second + Display_Sleep_Wake_NS));          // the window is over
    CHECK(!display_sleep_activity_due(&s, 200 * Second));
    // held all along (never let go): no window, nothing declared
    display_sleep_t t = { 0 };
    CHECK(!display_sleep_decide(&t, Display_Sleep_Always, false, false, true, 5 * Second) && t.retaken_at == 0);
    CHECK(!display_sleep_activity_due(&t, 6 * Second));
}

int main(void) {
    test_off();
    test_delay_and_return();
    test_on_battery();
    test_activity();
    test_is_input();
    test_no_bridge();
    test_wake_window();
    if (failures == 0)
        printf("test_display_sleep: all passed\n");
    return failures == 0 ? 0 : 1;
}
