// Tests for dynres.c, DynamicResolution's controller: its floor, when it steps down and up, and that it settles
// without going back and forth, against a model of a GPU-bound game at 120 Hz.
#include <stdio.h>
#include "../source/dynres.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// the GPU's time per frame at a resolution: a fixed part (shadows, the HUD, MetalFX) and a part that grows with the
// pixels. Kino at 100% needed 8.1-8.4 ms calm and 9.0-9.8 ms with the zombies around (perf-dip).
typedef struct {
    double fixed_ms, pixels_ms;
} gpu_model_t;

static double gpu_ms(gpu_model_t m, int percent) {
    double scale = percent / 100.0;
    return m.fixed_ms + m.pixels_ms * scale * scale;
}

// one window of the game at 120 Hz: the frame rate (the GPU's when it can't keep up, as measured) and how busy the GPU was
static int window(dynres_t *d, gpu_model_t m) {
    double ms = gpu_ms(m, d->percent);
    double fps = ms <= 1000.0 / 120 ? 120 : 1000.0 / ms;
    double busy = ms * fps / 1000;
    return dynres_window(d, fps, busy > 0.99 ? 0.99 : busy, 120);
}

// runs windows until nothing changed for 100 in a row (or 2000 windows), returns how many changes it made
static int settle(dynres_t *d, gpu_model_t m) {
    int changes = 0, quiet = 0;
    for (int i = 0; i < 2000 && quiet < 100; i++) {
        int before = d->percent;
        window(d, m);
        if (d->percent != before) {
            changes++;
            quiet = 0;
        } else {
            quiet++;
        }
    }
    return changes;
}

static void test_lowest_for(void) {
    CHECK(dynres_lowest_for(100, 75) == 75);
    CHECK(dynres_lowest_for(100, 70) == 70);
    CHECK(dynres_lowest_for(100, 85) == 85);
    CHECK(dynres_lowest_for(80, 70) == 55);  // 56, down to a whole step
    CHECK(dynres_lowest_for(60, 75) == 50);  // 45 is below the engine's 50
    CHECK(dynres_lowest_for(150, 70) == 105);
    CHECK(dynres_lowest_for(50, 75) == 50);  // can't go lower than the base either
}

static void test_reset(void) {
    dynres_t d;
    dynres_reset(&d, 100, 75);
    CHECK(d.percent == 100 && d.base == 100 && d.lowest == 75);
    dynres_reset(&d, 60, 75); // a floor above the base is the base
    CHECK(d.percent == 60 && d.lowest == 60);
}

static void test_steps_down_after_hold(void) {
    dynres_t d;
    dynres_reset(&d, 100, 75);
    // behind (110 of 120 fps) with the GPU busy: waits Dynres_Hold_Down windows after the start, then one step
    for (int i = 0; i < Dynres_Hold_Down; i++)
        CHECK(dynres_window(&d, 110, 0.98, 120) == 100);
    CHECK(dynres_window(&d, 110, 0.98, 120) == 95);
    // and again only after the hold
    for (int i = 0; i < Dynres_Hold_Down; i++)
        CHECK(dynres_window(&d, 110, 0.98, 120) == 95);
    CHECK(dynres_window(&d, 110, 0.98, 120) == 90);
}

static void test_far_behind_two_steps(void) {
    dynres_t d;
    dynres_reset(&d, 100, 75);
    for (int i = 0; i < Dynres_Hold_Down; i++)
        dynres_window(&d, 95, 0.99, 120);
    CHECK(dynres_window(&d, 95, 0.99, 120) == 90); // 95 fps is under 85% of 120
}

static void test_floor(void) {
    dynres_t d;
    dynres_reset(&d, 100, 75);
    for (int i = 0; i < 100; i++)
        dynres_window(&d, 60, 0.99, 120);
    CHECK(d.percent == 75);
}

static void test_not_gpu_bound(void) {
    dynres_t d;
    dynres_reset(&d, 100, 75);
    // behind because of the CPU: the GPU is busy only 60% of the time, a lower resolution wouldn't help
    for (int i = 0; i < 50; i++)
        dynres_window(&d, 90, 0.60, 120);
    CHECK(d.percent == 100);
}

static void test_steps_up_after_headroom(void) {
    dynres_t d;
    dynres_reset(&d, 100, 70);
    for (int i = 0; i < 50; i++)
        dynres_window(&d, 80, 0.99, 120);
    CHECK(d.percent == 70);
    // at 120 fps with the GPU busy 60%: a step up (75%, x1.15 the pixels) still fits under 88%
    for (int i = 0; i < Dynres_Hold_Up - 1; i++)
        CHECK(dynres_window(&d, 120, 0.60, 120) == 70);
    CHECK(dynres_window(&d, 120, 0.60, 120) == 75);
    // a window without headroom starts the count again
    for (int i = 0; i < Dynres_Hold_Up - 1; i++)
        dynres_window(&d, 120, 0.60, 120);
    dynres_window(&d, 120, 0.85, 120); // 0.85 x (80/75)^2 = 0.97: no room
    for (int i = 0; i < Dynres_Hold_Up - 1; i++)
        CHECK(dynres_window(&d, 120, 0.60, 120) == 75);
    CHECK(dynres_window(&d, 120, 0.60, 120) == 80);
    // never above the base
    for (int i = 0; i < 200; i++)
        dynres_window(&d, 120, 0.30, 120);
    CHECK(d.percent == 100);
}

static void test_no_targets(void) {
    dynres_t d;
    dynres_reset(&d, 100, 75);
    for (int i = 0; i < 20; i++) {
        dynres_window(&d, 0, 0.99, 120);
        dynres_window(&d, 60, 0.99, 0);
    }
    CHECK(d.percent == 100);
}

// the model: settles at a resolution that holds 120 fps, and then stays there
static void test_settles(void) {
    gpu_model_t zombies = { 2.0, 7.8 }, calm = { 2.0, 6.4 }, empty = { 2.0, 4.0 };
    dynres_t d;
    dynres_reset(&d, 100, 75);
    CHECK(settle(&d, zombies) <= 4);
    CHECK(d.percent < 100 && d.percent >= 75);
    CHECK(gpu_ms(zombies, d.percent) <= 1000.0 / 120);
    int held = d.percent;
    for (int i = 0; i < 500; i++)
        window(&d, zombies);
    CHECK(d.percent == held); // no going back and forth
    // calmer: up a little, not all the way if 100% doesn't fit
    settle(&d, calm);
    CHECK(d.percent >= held && gpu_ms(calm, d.percent) <= 1000.0 / 120);
    CHECK(gpu_ms(calm, 100) > 1000.0 / 120 ? d.percent < 100 : 1);
    // an easy scene: back to the base
    settle(&d, empty);
    CHECK(d.percent == 100);
    // at the base with room to spare, nothing changes at all
    CHECK(settle(&d, empty) == 0);
}

int main(void) {
    test_lowest_for();
    test_reset();
    test_steps_down_after_hold();
    test_far_behind_two_steps();
    test_floor();
    test_not_gpu_bound();
    test_steps_up_after_headroom();
    test_no_targets();
    test_settles();
    if (failures == 0)
        printf("test_dynres: all passed\n");
    return failures != 0;
}
