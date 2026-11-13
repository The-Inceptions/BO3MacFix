// Tests for dynres.c, DynamicResolution's controller: its floor, when it steps down and up, and that it settles
// without going back and forth, against a model of a GPU-bound game at 120 Hz.
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>
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
    // at 120 fps with the GPU busy 60%: a step up (75%, x1.15 the pixels) still fits under 80%
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

// behind because of the CPU: the GPU's share is scaled to the target's frame time first. With room even then, it comes
// back up after the hold (a CPU-bound match mustn't stay at a low resolution for good); without room, it stays.
static void test_up_when_cpu_bound(void) {
    dynres_t d;
    dynres_reset(&d, 100, 85);
    for (int i = 0; i < 50; i++)
        dynres_window(&d, 100, 0.99, 120);
    CHECK(d.percent == 85);
    // 105 fps (under 95% of 120), GPU busy 40%: 0.40 * 120/105 * (90/85)^2 = 0.51, room for 90%
    for (int i = 0; i < Dynres_Hold_Up - 1; i++)
        CHECK(dynres_window(&d, 105, 0.40, 120) == 85);
    CHECK(dynres_window(&d, 105, 0.40, 120) == 90);
    // 105 fps, GPU busy 70%: 0.70 * 120/105 * (95/90)^2 = 0.89, no room for 95%, however long
    for (int i = 0; i < 200; i++)
        CHECK(dynres_window(&d, 105, 0.70, 120) == 90);
}

// after a step down, no step up for Dynres_Hold_Up windows, however light the scene gets
static void test_hold_after_down(void) {
    dynres_t d;
    dynres_reset(&d, 100, 85);
    for (int i = 0; i <= Dynres_Hold_Down; i++)
        dynres_window(&d, 110, 0.98, 120);
    CHECK(d.percent == 95);
    for (int i = 0; i < Dynres_Hold_Up - 1; i++)
        CHECK(dynres_window(&d, 120, 0.40, 120) == 95);
    CHECK(dynres_window(&d, 120, 0.40, 120) == 100);
    CHECK(Dynres_Hold_Up * Dynres_Window_Frames >= 120 * 5); // at least 5 s at 120 fps
}

// play: the scene turns light for a few seconds at a time between the zombies. It comes down for the zombies once and
// stays there instead of going up for each lull and down again; a long calm brings it back up.
static void test_swings(void) {
    gpu_model_t zombies = { 2.0, 7.8 }, lull = { 2.0, 4.0 };
    dynres_t d;
    dynres_reset(&d, 100, 85);
    int changes = 0, before = d.percent;
    int lull_windows = 4 * 120 / Dynres_Window_Frames; // 4 s at 120 fps
    for (int cycle = 0; cycle < 15; cycle++) {          // 15 x (4 s of zombies, 4 s of lull) = 2 minutes
        for (int i = 0; i < 2 * lull_windows; i++) {
            window(&d, i < lull_windows ? zombies : lull);
            changes += d.percent != before;
            before = d.percent;
        }
    }
    CHECK(changes <= 2); // down for the first zombies, then nothing
    CHECK(gpu_ms(zombies, d.percent) <= 1000.0 / 120);
    // a calm of 30 s: back up to the base
    for (int i = 0; i < 30 * 120 / Dynres_Window_Frames; i++)
        window(&d, lull);
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
    // the set of a percent, from set 0's size: the engine's 95/90/85% of 2624x1696
    uint32_t table[Dynres_Sets] = {dynres_size(2624, 1696), cam, s85, 0, s95, s90, 0, 0};
    CHECK(dynres_set_for_percent(table, 100, 95) == 4);
    CHECK(dynres_set_for_percent(table, 100, 90) == 5);
    CHECK(dynres_set_for_percent(table, 100, 85) == 2);
    CHECK(dynres_set_for_percent(table, 100, 80) == -1);
    CHECK(dynres_set_for_percent(table, 100, 100) == -1); // set 0 isn't one to keep
    uint32_t no_set0[Dynres_Sets] = {0, s95};
    CHECK(dynres_set_for_percent(no_set0, 100, 95) == -1);
}

// memory pressure: letting go of the kept sets while macOS reports it, keeping them again once it's back to normal
static void test_pressure(void) {
    dynres_keep_t k = {{0}};
    uint32_t s95 = dynres_size(2496, 1616), s90 = dynres_size(2368, 1528), s85 = dynres_size(2232, 1448);
    dynres_keep_add(&k, s85);
    dynres_keep_add(&k, s90);
    dynres_keep_add(&k, s95);
    uint32_t sizes[Dynres_Sets] = {dynres_size(2624, 1696), s95, s90, s85};
    uint64_t used[Dynres_Sets] = {100, 100, 100, 100};
    CHECK(dynres_keep_mask(&k, sizes, used, 100) == 0xe);
    CHECK(dynres_keep_release(&k));
    CHECK(dynres_keep_mask(&k, sizes, used, 100) == 0); // nothing refreshed: the engine frees them at 11 frames
    CHECK(!dynres_keep_release(&k));                     // nothing left to let go
    dynres_keep_add(&k, s90);                            // learned again after the pressure
    CHECK(dynres_keep_mask(&k, sizes, used, 100) == 0x4);
    // the dispatch source's events
    CHECK(dynres_pressure_after(DISPATCH_MEMORYPRESSURE_WARN, false));
    CHECK(dynres_pressure_after(DISPATCH_MEMORYPRESSURE_CRITICAL, false));
    CHECK(dynres_pressure_after(DISPATCH_MEMORYPRESSURE_CRITICAL, true));
    CHECK(!dynres_pressure_after(DISPATCH_MEMORYPRESSURE_NORMAL, true));
    CHECK(!dynres_pressure_after(DISPATCH_MEMORYPRESSURE_NORMAL, false));
    CHECK(dynres_pressure_after(0, true)); // no level in the event: as before
    CHECK(!dynres_pressure_after(0, false));
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

// the frame rate to reach: the display's, our limit as the display shows it, the game's own com_maxfps
static void test_target(void) {
    CHECK(dynres_target(120, 45, 120) == 40); // BatteryFPS 45 on the 120 Hz panel: 40 fps
    CHECK(dynres_target(120, 90, 0) == 60);
    CHECK(dynres_target(120, 40, 120) == 40 && dynres_target(120, 60, 120) == 60 && dynres_target(120, 30, 0) == 30);
    CHECK(dynres_target(120, 0, 120) == 120 && dynres_target(120, 0, 0) == 120); // no limit: the display
    CHECK(dynres_target(144, 120, 0) == 72 && dynres_target(60, 45, 0) == 30 && dynres_target(240, 45, 0) == 40);
    CHECK(dynres_target(120, 144, 0) == 120); // a limit above the display's rate: the display's
    CHECK(dynres_target(120, 45, 30) == 30);  // com_maxfps lower: it, not rounded (the engine only waits)
    CHECK(dynres_target(120, 0, 50) == 50);
    CHECK(dynres_target(0, 45, 0) == 45 && dynres_target(0, 0, 0) == 0); // no display known
}

// A limit the panel can't show exactly: BatteryFPS 45 on the 120 Hz panel comes out at a steady 40.0 fps, and the GPU's
// busy share reads 92% there (the GPU clocks down to the pace it has: on Kino it stayed at 19-23 ms a frame from 85% down
// to 60%). Aiming at 45 took it from 100% to the floor in 5% steps (gc11old_pwrgpu_stepskinoboom); aimed at what the
// display shows, nothing changes. And a limit it shows exactly, the same.
static void test_uneven_limit(void) {
    dynres_t d;
    dynres_reset(&d, 100, 60);
    int moved = 0;
    for (int i = 0; i < 400; i++)
        moved += dynres_window(&d, 40.0, 0.92, dynres_target(120, 45, 120)) != 100;
    CHECK(moved == 0);
    dynres_reset(&d, 100, 60);
    for (int i = 0; i < 400; i++)
        moved += dynres_window(&d, 60.0, 0.94, dynres_target(120, 90, 120)) != 100;
    CHECK(moved == 0);
    dynres_reset(&d, 100, 60);
    for (int i = 0; i < 400; i++)
        moved += dynres_window(&d, 72.0, 0.95, dynres_target(144, 120, 0)) != 100;
    CHECK(moved == 0);
    // behind what the display can show, with the GPU busy: it still steps down (35 fps at the 40 the panel shows)
    dynres_reset(&d, 100, 60);
    for (int i = 0; i <= Dynres_Hold_Down; i++)
        dynres_window(&d, 35.0, 0.97, dynres_target(120, 45, 120));
    CHECK(d.percent == 95);
}

// The rule without a limit before the way up under a limit was added (main 68a7ef4), for comparing
static int rule_68a7ef4(dynres_t *d, double fps, double gpu_busy, double target_fps) {
    d->windows_since_change++;
    if (target_fps <= 0 || fps <= 0)
        return d->percent;
    bool settled = d->windows_since_change > Dynres_Hold_Down;
    if (fps < target_fps * Dynres_Missed && gpu_busy >= Dynres_GPU_Bound) {
        d->headroom_windows = 0;
        if (!settled || d->percent <= d->lowest)
            return d->percent;
        int steps = fps < target_fps * Dynres_Far_Behind ? 2 : 1;
        int percent = d->percent - steps * Dynres_Step;
        d->percent = percent > d->lowest ? percent : d->lowest;
        d->windows_since_change = d->headroom_windows = 0;
        return d->percent;
    }
    if (d->percent >= d->base)
        return d->percent;
    int up = d->percent + Dynres_Step < d->base ? d->percent + Dynres_Step : d->base;
    double scale = (double)up / d->percent;
    double busy = fps < target_fps ? gpu_busy * target_fps / fps : gpu_busy;
    if (busy * scale * scale >= Dynres_Headroom) {
        d->headroom_windows = 0;
        return d->percent;
    }
    if (++d->headroom_windows < Dynres_Hold_Up || !settled)
        return d->percent;
    d->percent = up;
    d->windows_since_change = d->headroom_windows = 0;
    return d->percent;
}

// The GPU's busy share at a 40 fps limit as the game read it on Kino (gc11_pwrgpu_stepskinoboom, BatteryFPS 45 on the
// 120 Hz panel: 40.0 fps throughout): the windows that stepped (90-94%) and the 5 s frame stats from 95% down to 60%
// (70-93%). The GPU slows its clock to the pace, so the share doesn't fall with the resolution.
static const double busy_at_40[] = { 0.92, 0.71, 0.94, 0.94, 0.93, 0.92, 0.93, 0.93, 0.93, 0.91, 0.84, 0.73,
                                     0.77, 0.76, 0.92, 0.76, 0.71, 0.70, 0.75, 0.76, 0.75, 0.72 };
#define Busy_At_40 (int)(sizeof(busy_at_40) / sizeof(busy_at_40[0]))

// Under the 40 fps limit (BatteryFPS 40 on the 120 Hz panel, the floor at 60), a heavy moment takes the resolution
// down; then every window is at 40.0 fps again with the recorded busy readings. Without a limit's rule (the way up by the
// busy share) it never comes back up: at 90% the next size needs under 72%, at 60% under 68%. With it, back to 100%.
static void test_climb_at_limit(void) {
    dynres_t d, old;
    dynres_reset(&d, 100, 60);
    dynres_reset(&old, 100, 60);
    for (int i = 0; i < 20; i++) { // calm at 100%: 40.0 fps, the GPU 32-35% busy (before the first explosion)
        dynres_window_limited(&d, 40.0, 0.34, 40, true);
        rule_68a7ef4(&old, 40.0, 0.34, 40);
    }
    CHECK(d.percent == 100 && old.percent == 100);
    for (int i = 0; i < 3; i++) { // the explosion: 30 fps, the GPU flat out
        dynres_window_limited(&d, 30.0, 1.0, 40, true);
        rule_68a7ef4(&old, 30.0, 1.0, 40);
    }
    CHECK(d.percent == 90 && old.percent == 90);
    int back_at = -1;
    for (int i = 0; i < 800; i++) { // 10 minutes at 40.0 fps with the clocked-down readings
        double busy = busy_at_40[i % Busy_At_40];
        dynres_window_limited(&d, 40.0, busy, 40, true);
        rule_68a7ef4(&old, 40.0, busy, 40);
        if (d.percent == 100 && back_at < 0)
            back_at = i;
    }
    CHECK(old.percent == 90);                  // the old way up: stuck
    CHECK(d.percent == 100);                   // this one: back
    CHECK(back_at >= 0 && back_at <= 2 * 12);  // two trials, about 9 windows each (15 s at 40 fps)
    // from the floor (BO3MACFIX_DYNRESSTARTLOW): 8 steps back to 100%, about a minute at 40 fps
    dynres_reset(&d, 100, 60);
    CHECK(dynres_start_at_floor(&d) == 60);
    int windows = 0;
    while (d.percent < 100 && windows < 400)
        dynres_window_limited(&d, 40.0, busy_at_40[windows++ % Busy_At_40], 40, true);
    CHECK(d.percent == 100 && windows <= 8 * 10); // 60 s at 40 fps
    // and stays there
    for (int i = 0; i < 400; i++)
        CHECK(dynres_window_limited(&d, 40.0, busy_at_40[i % Busy_At_40], 40, true) == 100);
}

// Under a limit, a trial the GPU can't hold is taken back to the size before, at once after the holds, and the next
// trial waits twice as long each time (up to 16 times): the GPU holds 40 fps up to 95% but only 36 at 100%.
static void test_trial_taken_back(void) {
    dynres_t d;
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    int trials = 0, missed = 0, lowest_after = 100, reached_95 = -1;
    for (int i = 0; i < 800; i++) {
        bool can = d.percent <= 95;
        double fps = can ? 40.0 : 36.0, busy = can ? busy_at_40[i % Busy_At_40] : 1.0;
        int before = d.percent;
        dynres_window_limited(&d, fps, busy, 40, true);
        trials += before == 95 && d.percent == 100;
        missed += !can;
        if (d.percent == 95 && reached_95 < 0)
            reached_95 = i;
        if (reached_95 >= 0 && d.percent < lowest_after)
            lowest_after = d.percent;
    }
    CHECK(reached_95 >= 0 && reached_95 <= 7 * 10);
    CHECK(lowest_after == 95);          // taken back to 95, never lower
    CHECK(d.percent == 95 || d.percent == 100);
    CHECK(trials >= 2 && trials <= 9);  // 8, 16, 32, 64, 128, 128 ... windows apart
    CHECK(missed <= trials * 3);        // each costs the holds and the window that showed it
    CHECK(d.failed_trials == Dynres_Max_Backoff || d.percent == 100);
    // a trial that held clears the doubling: at a 40 limit, 90 -> 95 holds, then 95 -> 100 is tried after the plain hold
    dynres_reset(&d, 100, 60);
    d.percent = 90;
    d.failed_trials = 3;
    for (int i = 0; i < 400 && d.percent < 95; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, true);
    CHECK(d.percent == 95);
    for (int i = 0; i < Dynres_Trial_Windows + 1; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, true);
    CHECK(d.failed_trials == 0);
}

// The trial's rules window by window, under a 40 fps limit
static void test_trial_rules(void) {
    dynres_t d;
    // one trial at a time: the next step only after the trial's windows have passed
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    int k = 0;
    while (d.percent == 60 && k < 100) {
        dynres_window_limited(&d, 40.0, 0.8, 40, true);
        k++;
    }
    CHECK(d.percent == 65 && d.trial_from == 60);
    for (int i = 0; i < Dynres_Trial_Windows; i++)
        CHECK(dynres_window_limited(&d, 40.0, 0.8, 40, true) == 65);
    CHECK(dynres_window_limited(&d, 40.0, 0.8, 40, true) == 70); // confirmed, and its windows counted for the next
    // the windows right after a trial step don't judge it (its render targets are being made): no take-back there
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    for (int i = 0; i < 100 && d.percent == 60; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, true);
    CHECK(dynres_window_limited(&d, 37.0, 0.95, 40, true) == 65);
    CHECK(dynres_window_limited(&d, 37.0, 0.95, 40, true) == 65);
    // behind but not the GPU's doing (busy 50%): not taken back
    CHECK(dynres_window_limited(&d, 35.0, 0.5, 40, true) == 65);
    CHECK(d.trial_from == 60);
    // behind with the GPU busy: back to 60
    CHECK(dynres_window_limited(&d, 35.0, 0.95, 40, true) == 60 && d.failed_trials == 1);
    // the limit not held (misses that aren't the GPU's): no trial however long
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    for (int i = 0; i < 400; i++)
        CHECK(dynres_window_limited(&d, i % 2 ? 40.0 : 35.0, 0.5, 40, true) == 60);
    // at a 10 fps limit (BackgroundFPS): the hold is at least the holds after a change, 3 windows
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    CHECK(dynres_window_limited(&d, 10.0, 0.9, 10, true) == 60);
    CHECK(dynres_window_limited(&d, 10.0, 0.9, 10, true) == 60);
    CHECK(dynres_window_limited(&d, 10.0, 0.9, 10, true) == 65);
    // a restart forgets a trial and the doubling
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    for (int i = 0; i < 100 && d.percent == 60; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, true);
    d.failed_trials = 3;
    dynres_reset(&d, 100, 60);
    CHECK(d.trial_from == 0 && d.failed_trials == 0);
    memset(&d, 0x7f, sizeof(d));
    dynres_reset(&d, 100, 60);
    CHECK(d.trial_from == 0 && d.failed_trials == 0);
}

// Without a limit nothing changes: the same windows give the same resolutions as the rule before (random windows), and
// a window behind with the GPU busy under a limit still steps down as before
static void test_no_limit_unchanged(void) {
    dynres_t d, old;
    dynres_reset(&d, 100, 60);
    dynres_reset(&old, 100, 60);
    unsigned seed = 7;
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1103515245u + 12345u;
        double fps = 60 + (seed >> 16) % 61, busy = ((seed >> 8) % 103) / 100.0;
        int a = dynres_window_limited(&d, fps, busy, 120, false), b = rule_68a7ef4(&old, fps, busy, 120);
        CHECK(a == b);
        if (a != b)
            break;
    }
    dynres_reset(&d, 100, 60);
    for (int i = 0; i < Dynres_Hold_Down; i++)
        dynres_window_limited(&d, 35.0, 0.99, 40, true);
    CHECK(dynres_window_limited(&d, 35.0, 0.99, 40, true) == 95);
}

int main(void) {
    test_climb_at_limit();
    test_trial_taken_back();
    test_no_limit_unchanged();
    test_trial_rules();
    test_target();
    test_uneven_limit();
    test_lowest_for();
    test_reset();
    test_steps_down_after_hold();
    test_far_behind_two_steps();
    test_floor();
    test_not_gpu_bound();
    test_steps_up_after_headroom();
    test_no_targets();
    test_settles();
    test_up_when_cpu_bound();
    test_hold_after_down();
    test_swings();
    test_keep();
    test_pressure();
    test_prewarm();
    if (failures == 0)
        printf("test_dynres: all passed\n");
    return failures != 0;
}
