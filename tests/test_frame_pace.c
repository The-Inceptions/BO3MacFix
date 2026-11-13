// Tests for frame_pace.c: how many refreshes each frame stayed on screen, from the times the frames were shown.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include "../source/frame_pace.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Refresh (1.0 / 120)
#define Most 1000

static double times[Most], work[Most];
static frame_pace_t pace;

// count times shown after the frames stayed the given numbers of refreshes in turn (pattern repeats), from t = 100 s,
// each a little late or early (jitter, in refreshes) the way a real display's times are
static int shown_after(const double *pattern, int length, int count, double jitter) {
    double t = 100.0;
    for (int i = 0; i < count; i++) {
        times[i] = t + (i % 2 ? jitter : -jitter) * Refresh;
        t += pattern[i % length] * Refresh;
    }
    return count;
}

static bool near(double a, double b, double within) {
    return fabs(a - b) <= within;
}

static void test_even_120(void) {
    int n = shown_after((const double[]){ 1 }, 1, 121, 0.02);
    frame_pace_summarize(times, n, Refresh, work, &pace);
    CHECK(pace.frames == 120);
    CHECK(pace.refreshes[0] == 120 && pace.refreshes[1] == 0 && pace.refreshes[2] == 0 && pace.refreshes[3] == 0);
    CHECK(pace.changes == 0);
    CHECK(pace.off_grid == 0);
    CHECK(near(pace.fps, 120, 0.5));
    CHECK(near(pace.low_1_fps, 1 / (1.04 * Refresh), 0.5)); // the late ones, 4% of a refresh
    CHECK(near(pace.worst_ms, 1.04 * Refresh * 1000, 0.01));
}

// the stutter: a GPU that needs 1.5 refreshes a frame on a fixed 120 Hz grid shows them 1, 2, 1, 2 refreshes
static void test_alternating(void) {
    int n = shown_after((const double[]){ 1, 2 }, 2, 201, 0);
    frame_pace_summarize(times, n, Refresh, work, &pace);
    CHECK(pace.frames == 200);
    CHECK(pace.refreshes[0] == 100 && pace.refreshes[1] == 100);
    CHECK(pace.changes == 199); // every frame but the first differs from the one before
    CHECK(pace.off_grid == 0);
    CHECK(near(pace.fps, 80, 0.1));
    CHECK(near(pace.low_1_fps, 60, 0.1));
    CHECK(near(pace.worst_ms, 2000.0 / 120, 0.01));
}

// FramePacingFPS 60 on the same: every frame two refreshes
static void test_even_60(void) {
    int n = shown_after((const double[]){ 2 }, 1, 101, 0.1);
    frame_pace_summarize(times, n, Refresh, work, &pace);
    CHECK(pace.frames == 100 && pace.refreshes[1] == 100 && pace.changes == 0 && pace.off_grid == 0);
    CHECK(near(pace.fps, 60, 0.1));
}

// a panel in variable refresh shows each frame when it's done: 1.4 refreshes is no whole number of them
static void test_variable_refresh(void) {
    int n = shown_after((const double[]){ 1.4 }, 1, 101, 0);
    frame_pace_summarize(times, n, Refresh, work, &pace);
    CHECK(pace.frames == 100 && pace.off_grid == 100);
    CHECK(pace.refreshes[0] == 100 && pace.changes == 0); // counted as the nearest whole number, at least 1
    CHECK(near(pace.fps, 120 / 1.4, 0.1));
    // a quarter refresh off is off the grid, less isn't
    n = shown_after((const double[]){ 1.26, 2.2, 0.8, 3.24 }, 4, 5, 0);
    frame_pace_summarize(times, n, Refresh, work, &pace);
    CHECK(pace.frames == 4 && pace.off_grid == 1);
    CHECK(pace.refreshes[0] == 2 && pace.refreshes[1] == 1 && pace.refreshes[2] == 1);
    // two frames within one refresh (a panel faster than it says): one refresh each, off the grid
    n = shown_after((const double[]){ 0.2, 1 }, 2, 3, 0);
    frame_pace_summarize(times, n, Refresh, work, &pace);
    CHECK(pace.frames == 2 && pace.refreshes[0] == 2 && pace.off_grid == 1 && pace.changes == 0);
}

// long frames go into the last bucket, the 1% low is the frame stats line's (the frame at 99% of the sorted ones)
static void test_long_frames(void) {
    double pattern[203];
    for (int i = 0; i < 203; i++)
        pattern[i] = i < 200 ? 1 : 6;
    int n = shown_after(pattern, 203, 204, 0);
    frame_pace_summarize(times, n, Refresh, work, &pace);
    CHECK(pace.frames == 203);
    CHECK(pace.refreshes[0] == 200 && pace.refreshes[3] == 3);
    CHECK(pace.changes == 1);
    CHECK(near(pace.low_1_fps, 20, 0.01));
    CHECK(near(pace.worst_ms, 50, 0.01));
    // two long ones of 202 are fewer than 1%
    n = shown_after(pattern, 203, 203, 0);
    frame_pace_summarize(times, n, Refresh, work, &pace);
    CHECK(pace.frames == 202 && near(pace.low_1_fps, 120, 0.01));
}

// frames never shown (0) are left out, times recorded out of order are sorted, too few times give nothing
static void test_unshown_and_order(void) {
    double some[] = { 100 + 2 * Refresh, 0, 100, 100 + Refresh, -1, 100 + 4 * Refresh };
    frame_pace_summarize(some, 6, Refresh, work, &pace);
    CHECK(pace.frames == 3);
    CHECK(pace.refreshes[0] == 2 && pace.refreshes[1] == 1);
    CHECK(pace.changes == 1 && pace.off_grid == 0);
    CHECK(near(pace.fps, 3 / (4 * Refresh), 0.01));
    double one[] = { 0, 100, 0 };
    pace.frames = 7;
    frame_pace_summarize(one, 3, Refresh, work, &pace);
    CHECK(pace.frames == 0 && pace.fps == 0 && pace.refreshes[0] == 0 && pace.worst_ms == 0);
    frame_pace_summarize(some, 6, 0, work, &pace);
    CHECK(pace.frames == 0);
    frame_pace_summarize(some, 0, Refresh, work, &pace);
    CHECK(pace.frames == 0);
}

int main(void) {
    test_even_120();
    test_alternating();
    test_even_60();
    test_variable_refresh();
    test_long_frames();
    test_unshown_and_order();
    if (failures == 0)
        printf("frame_pace tests passed\n");
    return failures != 0;
}
