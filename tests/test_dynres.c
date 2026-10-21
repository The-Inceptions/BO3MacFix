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

// keeping the sets of DynamicResolution's sizes past the engine's 11 frames
static void test_keep(void) {
    dynres_keep_t k = {{0}};
    uint32_t s95 = dynres_size(2496, 1616), s90 = dynres_size(2368, 1528), s85 = dynres_size(2232, 1448),
             s80 = dynres_size(2104, 1360), cam = dynres_size(512, 512);
    dynres_keep_add(&k, 0);
    CHECK(k.size[0] == 0);
    dynres_keep_add(&k, s95);
    dynres_keep_add(&k, s90);
    dynres_keep_add(&k, s95); // again: moves first, no duplicate
    CHECK(k.size[0] == s95 && k.size[1] == s90 && k.size[2] == 0);
    dynres_keep_add(&k, s85);
    dynres_keep_add(&k, s80); // a 4th: the oldest (s90) goes
    CHECK(k.size[0] == s80 && k.size[1] == s85 && k.size[2] == s95);
    uint32_t sizes[Dynres_Sets] = {dynres_size(2624, 1696), s95, cam, s85, 0, s90, s80, s95};
    uint64_t used[Dynres_Sets] = {100, 99, 100, 95, 100, 100, 94, 101};
    unsigned mask = dynres_keep_mask(&k, sizes, used, 100);
    CHECK(mask & (1u << 1));    // kept size, used a frame ago
    CHECK(!(mask & (1u << 0))); // set 0 is the engine's own
    CHECK(!(mask & (1u << 2))); // another view's set: the engine's to free
    CHECK(mask & (1u << 3));    // 5 frames ago: still fresh
    CHECK(!(mask & (1u << 4))); // free
    CHECK(!(mask & (1u << 5))); // a size no longer kept
    CHECK(!(mask & (1u << 6))); // 6 frames ago: left to the engine
    CHECK(!(mask & (1u << 7))); // last use after the frame read: left alone
    dynres_keep_t none = {{0}};
    CHECK(dynres_keep_mask(&none, sizes, used, 100) == 0);
}

// warming up at a match start: which sizes, in which order, one frame each, and back to the user's value
static void test_prewarm(void) {
    int out[Dynres_Keep_Max];
    CHECK(dynres_prewarm_sizes(100, 85, out) == 3 && out[0] == 95 && out[1] == 90 && out[2] == 85);
    CHECK(dynres_prewarm_sizes(100, 75, out) == 3 && out[2] == 85); // the 3 nearest: as many as stay kept
    CHECK(dynres_prewarm_sizes(100, 95, out) == 1 && out[0] == 95);
    CHECK(dynres_prewarm_sizes(87, 70, out) == 3 && out[0] == 82 && out[1] == 77 && out[2] == 72);
    CHECK(dynres_prewarm_sizes(80, 72, out) == 2 && out[0] == 75 && out[1] == 72); // the floor, not below
    CHECK(dynres_prewarm_sizes(60, 60, out) == 0);

    dynres_prewarm_t p;
    dynres_prewarm_init(&p);
    CHECK(!dynres_prewarm_running(&p));
    CHECK(dynres_prewarm_frame(&p, 100, 85, false, false) == 0); // the load, the menus: waits
    CHECK(dynres_prewarm_frame(&p, 100, 85, false, true) == 0);
    CHECK(dynres_prewarm_frame(&p, 100, 85, true, false) == 95); // the match's first 3D frame
    CHECK(dynres_prewarm_running(&p));
    CHECK(dynres_prewarm_frame(&p, 100, 85, true, false) == 0); // 95's set not there yet
    CHECK(dynres_prewarm_frame(&p, 100, 85, true, true) == 90); // it appeared: the next size
    CHECK(dynres_prewarm_frame(&p, 100, 85, true, true) == 85);
    CHECK(dynres_prewarm_frame(&p, 100, 85, true, true) == 100); // back to the user's value
    CHECK(!dynres_prewarm_running(&p));
    CHECK(dynres_prewarm_frame(&p, 100, 85, true, true) == 0); // once per arming
    for (int i = 0; i < 100; i++)
        CHECK(dynres_prewarm_frame(&p, 100, 85, true, false) == 0);

    // a size whose set doesn't appear (already there): goes on after Dynres_Prewarm_Wait frames
    p.armed = true; // the next map
    CHECK(dynres_prewarm_frame(&p, 100, 85, true, false) == 95);
    int frames = 0, value = 0;
    while (value == 0 && frames < 100) {
        value = dynres_prewarm_frame(&p, 100, 85, true, false);
        frames++;
    }
    CHECK(value == 90 && frames == Dynres_Prewarm_Wait);
    // ready no longer matters once it runs (the 3D view at a lower size doesn't use set 0)
    CHECK(dynres_prewarm_frame(&p, 100, 85, false, true) == 85);
    CHECK(dynres_prewarm_frame(&p, 100, 85, false, true) == 100);
    CHECK(!dynres_prewarm_running(&p) && !p.armed);

    // the user's value changed between the start and the end: goes back to the one it started from
    p.armed = true;
    CHECK(dynres_prewarm_frame(&p, 100, 95, true, false) == 95);
    CHECK(dynres_prewarm_frame(&p, 90, 85, true, true) == 100);
    // nothing to visit (the floor is the user's value): nothing written, disarmed
    p.armed = true;
    CHECK(dynres_prewarm_frame(&p, 60, 60, true, false) == 0);
    CHECK(!dynres_prewarm_running(&p) && !p.armed);
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
    test_keep();
    test_prewarm();
    if (failures == 0)
        printf("test_dynres: all passed\n");
    return failures != 0;
}
