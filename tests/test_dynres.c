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

// one window of the game at 120 Hz: the frame rate (the GPU's when it can't keep up, as measured) and how busy the GPU
// was
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
    // which sets get the mark frame - Dynres_Keep_Age: kept ones older than that, up to Dynres_Keep_Fresh
    uint32_t sizes[Dynres_Sets] = {dynres_size(2624, 1696), s95, cam, s85, 0, s90, s80, s95};
    uint64_t used[Dynres_Sets] = {80, 93, 80, 91, 80, 94, 99, 101};
    unsigned mask = dynres_keep_mask(&k, sizes, used, 100, Dynres_Keep_Age);
    CHECK(!(mask & (1u << 0))); // set 0 is the engine's own
    CHECK(mask & (1u << 1));    // kept size, 7 frames old: held at 6
    CHECK(!(mask & (1u << 2))); // another view's set: the engine's to free
    CHECK(mask & (1u << 3));    // 9 frames old: still held
    CHECK(!(mask & (1u << 4))); // free
    CHECK(!(mask & (1u << 5))); // a size no longer kept
    CHECK(!(mask & (1u << 6))); // a frame old: a view uses it, its own mark stays (never made older)
    CHECK(!(mask & (1u << 7))); // last use after the frame read: left alone
    CHECK(mask == 0xa);
    // the edges: 6 frames old is held already, 10 is the engine's to free, 5 is younger than the mark
    uint64_t edge[Dynres_Sets] = {80, 94, 80, 90, 80, 94, 94, 95};
    CHECK(dynres_keep_mask(&k, sizes, edge, 100, Dynres_Keep_Age) == 0);
    dynres_keep_t none = {{0}};
    CHECK(dynres_keep_mask(&none, sizes, used, 100, Dynres_Keep_Age) == 0);
    // the test switch BO3MACFIX_DYNRESKEEPNEWEST: hold age 0, every kept set used 1 to 9 frames before
    CHECK(dynres_keep_mask(&k, sizes, used, 100, 0) == 0x4a);
    // the counter's first frames: no mark before frame 0
    uint64_t early[Dynres_Sets] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(dynres_keep_mask(&k, sizes, early, 5, Dynres_Keep_Age) == 0);
    CHECK(dynres_keep_mask(&k, sizes, early, 6, Dynres_Keep_Age) == 0);
    CHECK(dynres_keep_mask(&k, sizes, early, 7, Dynres_Keep_Age) == 0xca);
    CHECK(dynres_keep_mask(&k, sizes, used, 100, -1) == 0);
    // a keep list with room: its empty places match no free set; set 0 is never held, even of a kept size
    dynres_keep_t one = {{0}};
    dynres_keep_add(&one, dynres_size(2624, 1696));
    uint32_t with_free[Dynres_Sets] = {dynres_size(2624, 1696), 0, dynres_size(2624, 1696), 0, 0, 0, 0, 0};
    uint64_t aged[Dynres_Sets] = {93, 93, 93, 93, 93, 93, 93, 93};
    CHECK(dynres_keep_mask(&one, with_free, aged, 100, Dynres_Keep_Age) == 0x4);
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
    uint64_t used[Dynres_Sets] = {93, 93, 93, 93};
    CHECK(dynres_keep_mask(&k, sizes, used, 100, Dynres_Keep_Age) == 0xe);
    CHECK(dynres_keep_release(&k));
    CHECK(dynres_keep_mask(&k, sizes, used, 100, Dynres_Keep_Age) == 0); // nothing held: the engine frees them at 11
    CHECK(!dynres_keep_release(&k));                                      // nothing left to let go
    dynres_keep_add(&k, s90);                                             // learned again after the pressure
    CHECK(dynres_keep_mask(&k, sizes, used, 100, Dynres_Keep_Age) == 0x4);
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
// busy share reads 92% there (the GPU clocks down to the pace it has: on Kino it stayed at 19-23 ms a frame from 85%
// down to 60%). Aiming at 45 took it from 100% to the floor in 5% steps (gc11old_pwrgpu_stepskinoboom); aimed at what
// the display shows, nothing changes. And a limit it shows exactly, the same.
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
// down; then every window is at 40.0 fps again with the recorded busy readings. Without a limit's rule (the way up by
// the busy share) it never comes back up: at 90% the next size needs under 72%, at 60% under 68%. With it, back to
// 100%.
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
    CHECK(d.percent == 100 && windows == 8 + 7 * 9); // the first hold, then a trial every 9 windows: 53 s at 40 fps
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
    CHECK(trials >= 2 && trials <= 8);  // 8, 16, 32, 64, 128, 256 ... windows apart
    CHECK(missed <= trials * 3);        // each costs the holds and the window that showed it
    // a trial whose size lasts its watch clears the doubling: at a 40 limit, 90 -> 95 holds for Dynres_Trial_Watch
    // windows (kept after Dynres_Trial_Windows, the count cleared only at the end of the watch)
    dynres_reset(&d, 100, 60);
    d.percent = 90;
    d.failed_trials = 3;
    for (int i = 0; i < 400 && d.percent < 95; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, true);
    CHECK(d.percent == 95);
    for (int i = 0; i < Dynres_Trial_Windows + 1; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, true);
    CHECK(d.trial_from == 0 && d.failed_trials == 3); // kept, still watched
    for (int i = Dynres_Trial_Windows + 1; i < Dynres_Trial_Watch; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, true);
    CHECK(d.failed_trials == 3 && d.percent == 95);
    // the window after the watch clears it, and with the plain hold long past the next trial comes in the same window
    CHECK(dynres_window_limited(&d, 40.0, 0.8, 40, true) == 100 && d.failed_trials == 0);
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

// A GPU-bound map at the display's 120 (DEIMOS: 3.85 + 10.9 * scale^2 ms) for one window at a time: the frame rate
// and the busy share the controller sees
static void deimos_window(int percent, double *fps, double *busy) {
    double ms = gpu_ms((gpu_model_t){ 3.85, 10.9 }, percent);
    *fps = ms <= 1000.0 / 120 ? 120 : 1000.0 / ms;
    *busy = ms * *fps / 1000 >= 0.999 ? 1.0 : ms * *fps / 1000;
}

// The reviewer's pause (rev-perf sim3/pause.c): a match without a limit settles on DEIMOS, then a minute in the pause
// menu (its 30 fps limit, the GPU clocked down: the readings at 40 fps), then the match again. Nothing changes in the
// pause or after it, as with the rule before (68a7ef4); the climb's first version took it to 100% in the pause and
// back down in 4 changes after.
static void test_pause_without_limit(void) {
    dynres_t d, old;
    dynres_reset(&d, 100, 50);
    dynres_reset(&old, 100, 50);
    double fps, busy;
    for (int i = 0; i < 600; i++) {
        deimos_window(d.percent, &fps, &busy);
        dynres_window_limited(&d, fps, busy, 120, Dynres_No_Limit);
        deimos_window(old.percent, &fps, &busy);
        rule_68a7ef4(&old, fps, busy, 120);
    }
    CHECK(d.percent == old.percent && d.percent < 100);
    int settled = d.percent, changes = 0, old_changes = 0;
    for (int i = 0; i < 240; i++) { // 60 s of pause at 30 fps
        int before = d.percent, old_before = old.percent;
        dynres_window_limited(&d, 30.0, busy_at_40[i % Busy_At_40], 30, Dynres_Situation_Limit);
        rule_68a7ef4(&old, 30.0, busy_at_40[i % Busy_At_40], 30);
        changes += d.percent != before;
        old_changes += old.percent != old_before;
    }
    CHECK(changes == 0 && old_changes == 0 && d.percent == settled);
    for (int i = 0; i < 600; i++) { // the match again
        int before = d.percent, old_before = old.percent;
        deimos_window(d.percent, &fps, &busy);
        dynres_window_limited(&d, fps, busy, 120, Dynres_No_Limit);
        deimos_window(old.percent, &fps, &busy);
        rule_68a7ef4(&old, fps, busy, 120);
        changes += d.percent != before;
        old_changes += old.percent != old_before;
    }
    CHECK(changes == old_changes && d.percent == old.percent && d.percent == settled);
    // and a pause whose readings show room (a light pause scene): still no step up there, the match comes back at its
    // size
    for (int i = 0; i < 400; i++)
        CHECK(dynres_window_limited(&d, 30.0, 0.2, 30, Dynres_Situation_Limit) == settled);
    // the first match window after it doesn't step up on the pause's count either (C3): the count starts again
    deimos_window(d.percent, &fps, &busy);
    CHECK(dynres_window_limited(&d, fps, 0.2, 120, Dynres_No_Limit) == settled);
}

// A pause during the climb at a 40 limit: no change in the pause, the trial in progress waits (its windows count in the
// match only) and goes on after it; a pause in which the GPU falls behind takes it back
static void test_pause_during_trial(void) {
    dynres_t d;
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    for (int i = 0; i < 100 && d.trial_from == 0; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    CHECK(d.percent == 65 && d.trial_from == 60);
    for (int i = 0; i < 3; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    int counted = d.trial_windows;
    for (int i = 0; i < 80; i++)
        CHECK(dynres_window_limited(&d, 30.0, busy_at_40[i % Busy_At_40], 30, Dynres_Situation_Limit) == 65);
    CHECK(d.trial_from == 60 && d.trial_windows == counted); // waited
    int windows = 0;
    while (d.trial_from != 0 && windows < 50) {
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
        windows++;
    }
    CHECK(d.percent >= 65 && windows == Dynres_Trial_Windows - counted + 1); // kept after the rest of its windows
    // taken back in a pause if the GPU falls behind there
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    for (int i = 0; i < 100 && d.trial_from == 0; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    for (int i = 0; i < Dynres_Hold_Down; i++)
        dynres_window_limited(&d, 30.0, 0.8, 30, Dynres_Situation_Limit);
    CHECK(dynres_window_limited(&d, 25.0, 1.0, 30, Dynres_Situation_Limit) == 60 && d.event == Dynres_Event_Taken_Back);
}

// The reviewer's map that can't hold the next size (rev-perf sim3/fail.c): a 40 limit, the GPU holds it at 85% but not
// at 90%, 30 minutes. The trials thin out to one in 26 minutes; each costs about 3 windows a little under the limit.
static void test_failing_trials_30_minutes(void) {
    dynres_t d;
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    int trials = 0, short_windows = 0, late_trials = 0, lowest_after = 100, reached = -1;
    for (int i = 0; i < 30 * 60 * 40 / Dynres_Window_Frames; i++) {
        bool holds = d.percent <= 85;
        int before = d.percent;
        double fps = holds ? 40.0 : 36.0, busy = holds ? busy_at_40[i % Busy_At_40] : 0.99;
        dynres_window_limited(&d, fps, busy, 40, Dynres_Match_Limit);
        if (before == 85 && d.percent == 90) {
            trials++;
            late_trials += i >= 15 * 60 * 40 / Dynres_Window_Frames;
        }
        short_windows += !holds;
        if (d.percent == 85 && reached < 0)
            reached = i;
        if (reached >= 0 && d.percent < lowest_after)
            lowest_after = d.percent;
    }
    CHECK(reached >= 0 && lowest_after == 85);
    CHECK(trials <= 9 && late_trials <= 1);
    CHECK(short_windows <= 3 * trials);
    CHECK(d.failed_trials == Dynres_Max_Backoff);
    // and a trial taken back at the cap leaves it there (the hold doesn't grow past 256 times)
    d.percent = 85;
    d.trial_from = 0;
    for (int i = 0; i < 10000 && d.trial_from == 0; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    CHECK(d.trial_from == 85);
    for (int i = 0; i < Dynres_Hold_Down; i++)
        dynres_window_limited(&d, 36.0, 0.99, 40, Dynres_Match_Limit);
    CHECK(dynres_window_limited(&d, 36.0, 0.99, 40, Dynres_Match_Limit) == 85);
    CHECK(d.failed_trials == Dynres_Max_Backoff);
}

// The kinds of limit don't share their counts (C3), and what a window did is what the log says (C4)
static void test_limit_kinds(void) {
    dynres_t d;
    // a long stretch at a match limit, then the limit goes (plugged in): a trial in progress is kept as it is, the
    // doubling forgotten, and the count of the way up without a limit starts from 0
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    for (int i = 0; i < 100 && d.trial_from == 0; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    d.failed_trials = 2;
    CHECK(dynres_window_limited(&d, 100.0, 0.99, 120, Dynres_No_Limit) == 65); // behind without a limit: no take-back
    CHECK(d.trial_from == 0 && d.failed_trials == 0 && d.headroom_windows == 0);
    // a trial kept in the window that also steps down: a late failure (still watched), logged as such, not as the trial
    // taken back
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    for (int i = 0; i < 100 && d.trial_from == 0; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    CHECK(d.event == Dynres_Event_Trial);
    for (int i = 0; i < Dynres_Trial_Windows; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    CHECK(d.trial_from == 60);
    CHECK(dynres_window_limited(&d, 35.0, 0.99, 40, Dynres_Match_Limit) == 60 && d.event == Dynres_Event_Failed_Late);
    CHECK(d.trial_from == 0 && d.failed_trials == 1 && !d.trial_watched);
    // under a situation's limit no trial starts, however long it holds
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    for (int i = 0; i < 400; i++)
        CHECK(dynres_window_limited(&d, 30.0, 0.5, 30, Dynres_Situation_Limit) == 60);
    // and its count doesn't carry into the match: a full hold there before the first trial
    int windows = 0;
    while (d.percent == 60 && windows < 100) {
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
        windows++;
    }
    CHECK(windows == 8); // 6 s at 40 fps
    // the way up without a limit counts 31 windows with room, then a match limit begins (unplugged): its trial needs
    // its own full hold, not one more window
    dynres_reset(&d, 100, 60);
    d.percent = 80;
    for (int i = 0; i < Dynres_Hold_Up - 1; i++)
        CHECK(dynres_window_limited(&d, 120.0, 0.3, 120, Dynres_No_Limit) == 80);
    CHECK(d.headroom_windows == Dynres_Hold_Up - 1);
    for (int i = 0; i < 7; i++)
        CHECK(dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit) == 80);
    CHECK(dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit) == 85);
}

// The cycle of gc16_pwrgpu_climb40 (patch 0003): a size that holds the limit for 9 to 28 windows after the trial step
// up and then falls behind (the GPU's clock drops), kept by the trial and stepped down plainly a little later. Counted
// as failed trials, these thin out like the map that can't hold the next size; without, one every 7 to 9 s for good.
static void test_late_falls_thin_out(void) {
    int falls_after[] = { 9, 10, 11, 12, 20, 28 }; // gc16: 9 to 12; up to the watch's 32 (24 s at 40 fps)
    for (int f = 0; f < 6; f++) {
        int k = falls_after[f];
        dynres_t d;
        dynres_reset(&d, 100, 60);
        d.percent = 90;
        int trials = 0, late_trials = 0, short_windows = 0, at_95 = 0, changes = 0;
        for (int i = 0; i < 30 * 60 * 40 / Dynres_Window_Frames; i++) {
            bool holds = d.percent <= 90 || (d.percent == 95 && at_95 < k); // 100% never holds
            int before = d.percent;
            dynres_window_limited(&d, holds ? 40.0 : 34.6, holds ? busy_at_40[i % Busy_At_40] : 0.99, 40,
                                  Dynres_Match_Limit);
            at_95 = d.percent == 95 ? at_95 + 1 : 0;
            if (before == 90 && d.percent == 95) {
                trials++;
                late_trials += i >= 15 * 60 * 40 / Dynres_Window_Frames;
            }
            changes += d.percent != before;
            short_windows += !holds;
        }
        if (getenv("DYNRES_TEST_VERBOSE"))
            printf("  late falls after %i windows: %i trials (%i in the second quarter hour), %i changes, %i windows "
                   "short\n", k, trials, late_trials, changes, short_windows);
        CHECK(trials <= 9 && late_trials <= 1); // patch 0002: 120 to 141 trials, a change every 7 to 9 s
        CHECK(changes <= 20 && short_windows <= 2 * trials);
        CHECK(d.failed_trials == Dynres_Max_Backoff);
    }
}

// A heavy moment long after the last trial (its size lasted the watch) is a plain step down: no failure counted, the
// next trial after the plain hold. Within the watch but in a pause, the step only ends the watch.
static void test_late_heavy_moment(void) {
    dynres_t d;
    dynres_reset(&d, 100, 60);
    d.percent = 90;
    for (int i = 0; i < 100 && d.percent < 100; i++) // 90 -> 95 -> 100 in trials
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    CHECK(d.percent == 100 && d.trial_watched);
    for (int i = 0; i < Dynres_Trial_Watch + 1; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    CHECK(!d.trial_watched && d.failed_trials == 0);
    CHECK(dynres_window_limited(&d, 30.0, 0.99, 40, Dynres_Match_Limit) == 90); // the heavy moment: two steps
    CHECK(d.event == Dynres_Event_None && d.failed_trials == 0);
    int windows = 0;
    while (d.percent == 90 && windows < 100) {
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
        windows++;
    }
    CHECK(windows == 8); // the plain hold, 6 s at 40 fps
    // a step down for the GPU in a pause during the watch: not counted, and it ends the watch, so a fall in the match
    // after it isn't counted either
    dynres_reset(&d, 90, 60); // a base of 90: no trial after this one
    d.percent = 85;
    for (int i = 0; i < 100 && d.percent < 90; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    for (int i = 0; i < Dynres_Trial_Windows + 1; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    CHECK(d.trial_from == 0 && d.trial_watched);
    CHECK(dynres_window_limited(&d, 25.0, 0.99, 30, Dynres_Situation_Limit) == 80);
    CHECK(d.failed_trials == 0 && !d.trial_watched);
    for (int i = 0; i < Dynres_Hold_Down; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    CHECK(dynres_window_limited(&d, 36.0, 0.99, 40, Dynres_Match_Limit) == 75 && d.failed_trials == 0);
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

// the set the engine's lookup frees when all 8 are taken: the least recently used of 1-7, the lower one on a tie
static void test_eviction_victim(void) {
    uint32_t full[Dynres_Sets] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint64_t used[Dynres_Sets] = {0, 50, 40, 60, 40, 70, 80, 90};
    CHECK(dynres_eviction_victim(full, used) == 2); // 40 twice: the lower one
    used[4] = 39;
    CHECK(dynres_eviction_victim(full, used) == 4);
    used[1] = 1;
    CHECK(dynres_eviction_victim(full, used) == 1); // set 0's last use (0) doesn't count: it's never freed
    uint64_t same[Dynres_Sets] = {5, 5, 5, 5, 5, 5, 5, 5};
    CHECK(dynres_eviction_victim(full, same) == 1);
    used[7] = 0;
    CHECK(dynres_eviction_victim(full, used) == 7);
    for (int free_set = 0; free_set < Dynres_Sets; free_set++) {
        uint32_t sizes[Dynres_Sets] = {1, 2, 3, 4, 5, 6, 7, 8};
        sizes[free_set] = 0;
        CHECK(dynres_eviction_victim(sizes, used) == -1); // a free set is taken first, nothing freed
    }
}

// what the engine did between two looks at its table
static void test_table_changes(void) {
    uint32_t s0 = dynres_size(2624, 1696), a = dynres_size(2496, 1616), b = dynres_size(2368, 1528),
             c = dynres_size(2232, 1448), d = dynres_size(2104, 1360), e = dynres_size(1976, 1272),
             f = dynres_size(1840, 1192), g = dynres_size(1712, 1104);
    dynres_set_change_t out[Dynres_Sets];
    {
        uint32_t before[Dynres_Sets] = {s0, a, b, 0, c, f, 0, g};
        uint64_t use[Dynres_Sets] = {100, 99, 80, 0, 95, 86, 0, 91};
        uint32_t now[Dynres_Sets] = {s0, a, 0, d, e, g, 0, g};
        CHECK(dynres_table_changes(before, use, now, 101, out) == 1);
        CHECK(out[0] == Dynres_Set_Same);
        CHECK(out[1] == Dynres_Set_Same);
        CHECK(out[2] == Dynres_Set_Freed); // 21 frames unused: the sweep
        CHECK(out[3] == Dynres_Set_Made);
        CHECK(out[4] == Dynres_Set_Taken); // 6 frames old, made again for another size: only the eviction
        CHECK(out[5] == Dynres_Set_Freed); // 15 frames unused: the sweep, then made again for another size
        CHECK(out[6] == Dynres_Set_Same);
        CHECK(out[7] == Dynres_Set_Same);
    }
    {
        // 10 frames: the sweep can't have freed it yet; 11: it can
        uint32_t before[Dynres_Sets] = {s0, a, b};
        uint64_t use[Dynres_Sets] = {100, 91, 90};
        uint32_t now[Dynres_Sets] = {s0, c, d};
        CHECK(dynres_table_changes(before, use, now, 101, out) == 1);
        CHECK(out[1] == Dynres_Set_Taken && out[2] == Dynres_Set_Freed);
    }
    {
        // a rebuild: every set freed, a set in use left free; the one made again in its place isn't taken
        uint32_t before[Dynres_Sets] = {s0, a, b, c};
        uint64_t use[Dynres_Sets] = {100, 100, 95, 97};
        uint32_t now[Dynres_Sets] = {s0, d, 0, 0};
        CHECK(dynres_table_changes(before, use, now, 101, out) == 0);
        CHECK(out[0] == Dynres_Set_Rebuilt);
        CHECK(out[1] == Dynres_Set_Rebuilt && out[2] == Dynres_Set_Rebuilt && out[3] == Dynres_Set_Rebuilt);
    }
    {
        // a rebuild at another size of set 0 (a display change)
        uint32_t before[Dynres_Sets] = {s0, a};
        uint64_t use[Dynres_Sets] = {100, 100};
        uint32_t now[Dynres_Sets] = {dynres_size(3456, 2234), b};
        CHECK(dynres_table_changes(before, use, now, 101, out) == 0);
        CHECK(out[0] == Dynres_Set_Rebuilt && out[1] == Dynres_Set_Rebuilt);
    }
    {
        // a last use after the frame read (the engine marked it between the two reads): as young as can be
        uint32_t before[Dynres_Sets] = {s0, a};
        uint64_t use[Dynres_Sets] = {100, 102};
        uint32_t now[Dynres_Sets] = {s0, b};
        CHECK(dynres_table_changes(before, use, now, 101, out) == 1);
        CHECK(out[1] == Dynres_Set_Taken);
    }
}

// A model of the engine's render target sets as its lookup 0x100e8d843 treats them (wip/night/rt-rebuild.md): the
// sweep of sets 1-7 unused for Dynres_Engine_Frees frames, a hit, the first free set, else the least recently used of
// sets 1-7 (the lower one on a tie). It records which frame used which incarnation of each set, so that a set freed
// while the render thread may still draw with it can be told: the main thread's lookups of frame c run while the render
// thread draws frame c-1, whose passes read the sets of frames c-4 to c-1.
typedef struct {
    uint32_t size[Dynres_Sets];
    uint64_t last_use[Dynres_Sets];
    int generation[Dynres_Sets]; // a new one each time a set is made
    int used[64][Dynres_Sets];   // per frame (counter % 64): 1 + the generation of each set a view used it in, 0 none
    uint64_t counter;
    int made, evictions, evictions_in_flight, freed_in_flight, most_in_use;
    uint64_t youngest_eviction; // the age of the youngest set taken by an eviction
} engine_t;

static void engine_init(engine_t *e, uint32_t set0) {
    memset(e, 0, sizeof(*e));
    e->size[0] = set0;
    e->counter = 1000;
    e->youngest_eviction = UINT64_MAX;
}

static bool engine_in_flight(const engine_t *e, int set) {
    for (uint64_t frame = e->counter - Dynres_In_Flight_Age; frame < e->counter; frame++)
        if (e->used[frame % 64][set] == e->generation[set] + 1)
            return true;
    return false;
}

static int engine_lookup(engine_t *e, uint32_t size) {
    for (int set = 1; set < Dynres_Sets; set++)
        if (e->size[set] != 0 && e->counter - e->last_use[set] >= Dynres_Engine_Frees) {
            e->freed_in_flight += engine_in_flight(e, set);
            e->size[set] = 0;
            e->last_use[set] = 0;
        }
    int set = -1;
    for (int i = 0; i < Dynres_Sets && set < 0; i++)
        if (e->size[i] == size)
            set = i;
    if (set < 0) {
        int victim = dynres_eviction_victim(e->size, e->last_use);
        for (int i = 0; i < Dynres_Sets && set < 0; i++)
            if (e->size[i] == 0)
                set = i;
        if (set < 0) {
            set = 1;
            for (int i = 2; i < Dynres_Sets; i++)
                if (e->last_use[i] < e->last_use[set]) // the engine's cmovae: a tie keeps the lower one
                    set = i;
            CHECK(victim == set);
            uint64_t age = e->counter - e->last_use[set];
            e->evictions++;
            if (age < e->youngest_eviction)
                e->youngest_eviction = age;
            bool in_flight = engine_in_flight(e, set);
            e->evictions_in_flight += in_flight;
            e->freed_in_flight += in_flight;
        } else {
            CHECK(victim == -1);
        }
        e->size[set] = size;
        e->generation[set]++;
        e->made++;
    }
    e->last_use[set] = e->counter;
    e->used[e->counter % 64][set] = e->generation[set] + 1;
    int in_use = 0;
    for (int i = 0; i < Dynres_Sets; i++)
        in_use += e->size[i] != 0;
    if (in_use > e->most_in_use)
        e->most_in_use = in_use;
    return set;
}

static void engine_next_frame(engine_t *e) {
    e->counter++;
    memset(e->used[e->counter % 64], 0, sizeof(e->used[0]));
}

// the glue's hold of the kept sets, at the counter the render thread reads in the present
static void engine_keep(engine_t *e, const dynres_keep_t *keep, int hold_age) {
    unsigned mask = dynres_keep_mask(keep, e->size, e->last_use, e->counter, hold_age);
    for (int set = 1; set < Dynres_Sets; set++)
        if (mask & (1u << set)) {
            CHECK(e->counter - hold_age > e->last_use[set]); // only ever younger
            e->last_use[set] = e->counter - hold_age;
        }
}

static uint32_t percent_size(int percent) {
    return dynres_size((uint16_t)((2624 * percent / 100 + 7) & ~7), (uint16_t)((1696 * percent / 100 + 7) & ~7));
}

// the stress test BO3MACFIX_DYNRESTESTFRAMES: the warm-up keeps 95/90/85%, then the view takes another of five other
// sizes every frame. 3 kept sets and set 0 leave four sets for five sizes, so every step frees one. Held at the
// newest frame (before), the kept sets are never the least recently used, and the set freed is the one the frame drawn
// 4 frames before used, which the render thread's passes are still reading; held at Dynres_Keep_Age, the kept sets go
// first, and once they are gone the five sizes fit.
static void engine_stress(int hold_age, bool keep_first, engine_t *e) {
    dynres_keep_t keep = {{0}};
    int cycle[] = {80, 75, 70, 65, 60};
    engine_init(e, percent_size(100));
    for (int percent = 95; percent >= 85; percent -= 5) { // the warm-up
        engine_lookup(e, percent_size(percent));
        dynres_keep_add(&keep, percent_size(percent));
        engine_next_frame(e);
    }
    for (int frame = 0; frame < 300; frame++) {
        if (keep_first)
            engine_keep(e, &keep, hold_age);
        engine_lookup(e, percent_size(cycle[frame % 5]));
        if (!keep_first)
            engine_keep(e, &keep, hold_age);
        engine_next_frame(e);
    }
}

static void test_engine_stress(void) {
    engine_t e;
    for (int order = 0; order < 2; order++) {
        engine_stress(Dynres_Keep_Age, order, &e);
        CHECK(e.freed_in_flight == 0);
        CHECK(e.evictions <= 3);          // the three kept sets, once each
        CHECK(e.youngest_eviction > Dynres_In_Flight_Age);
        engine_stress(0, order, &e);      // held at the newest frame, as before
        CHECK(e.evictions > 250);         // every step
        CHECK(e.evictions_in_flight > 250);
        CHECK(e.youngest_eviction <= Dynres_In_Flight_Age);
    }
}

// DynamicResolution's own steps in a match: one every 2 windows at most (60 frames), up and down between 60 and 100%,
// the new set of each step learned and kept. With one view the table never fills (5 sets at most). A crowded table
// besides: the user's own size in sets 1-7 (set 0 rebuilt while DynamicResolution held 80%) and an extra view that
// renders at one of 4 camera sizes in turn, a new one each frame (more than a real camera does, to fill the table):
// then every step finds all 8 sets taken, and the engine frees one. Held at the newest frame, the kept sets are never
// that one, and the cameras' sets the render thread is drawing with are; held at Dynres_Keep_Age, never.
static void engine_play(int hold_age, bool extra_views, engine_t *e) {
    dynres_keep_t keep = {{0}};
    engine_init(e, percent_size(extra_views ? 80 : 100));
    int percent = 100, direction = -5, learn = 0;
    uint32_t cameras[] = {dynres_size(1224, 750), dynres_size(1024, 1024), dynres_size(512, 512),
                          dynres_size(640, 480)};
    for (int frame = 0; frame < 6000; frame++) {
        if (frame % 60 == 59) { // a step
            percent += direction;
            if (percent <= 60 || percent >= 100)
                direction = -direction;
            learn = 12;
        }
        engine_keep(e, &keep, hold_age);
        int set = engine_lookup(e, percent_size(percent));
        if (learn > 0) { // the sets that appear in the frames after a step are learned (dynres_keep_tick)
            learn--;
            if (set != 0)
                dynres_keep_add(&keep, e->size[set]);
        }
        if (extra_views)
            engine_lookup(e, cameras[frame % 4]);
        engine_next_frame(e);
    }
}

static void test_engine_play(void) {
    engine_t e;
    for (int hold_age = 0; hold_age <= Dynres_Keep_Age; hold_age += Dynres_Keep_Age) {
        engine_play(hold_age, false, &e);
        CHECK(e.evictions == 0 && e.freed_in_flight == 0);
        CHECK(e.most_in_use <= 5); // set 0, 3 kept and the size now (the one before is freed before the next step)
        CHECK(e.made > 50);        // the steps made sets: the play ran
    }
    engine_play(Dynres_Keep_Age, true, &e);
    CHECK(e.evictions > 20);        // the table is full at the steps
    CHECK(e.freed_in_flight == 0);  // and no set the render thread may draw with is freed
    CHECK(e.youngest_eviction > Dynres_In_Flight_Age);
    engine_play(0, true, &e);       // before: the same play does free them
    CHECK(e.evictions_in_flight > 100);
}

// the edge of what the render thread may be drawing with: 2 sets held at the newest frame and 6 other sizes in turn,
// one per frame, leave 5 sets for 6 sizes, so the engine takes the one used 5 frames before at every step; the frame
// being drawn and the 3 its passes read back don't have it. With 3 held and 5 sizes in 4 sets, the one used 4 frames
// before: they do.
static void engine_edge(int held, engine_t *e) {
    dynres_keep_t keep = {{0}};
    int cycle[] = {80, 75, 70, 65, 60, 55};
    engine_init(e, percent_size(100));
    for (int i = 0; i < held; i++) {
        engine_lookup(e, percent_size(95 - 5 * i));
        dynres_keep_add(&keep, percent_size(95 - 5 * i));
        engine_next_frame(e);
    }
    for (int frame = 0; frame < 300; frame++) {
        engine_keep(e, &keep, 0);
        engine_lookup(e, percent_size(cycle[frame % (8 - held)]));
        engine_next_frame(e);
    }
}

static void test_engine_edge(void) {
    engine_t e;
    engine_edge(2, &e);
    CHECK(e.evictions > 250);
    CHECK(e.youngest_eviction == Dynres_In_Flight_Age + 1);
    CHECK(e.freed_in_flight == 0);
    engine_edge(3, &e);
    CHECK(e.evictions > 250);
    CHECK(e.youngest_eviction == Dynres_In_Flight_Age);
    CHECK(e.evictions_in_flight == e.evictions);
}

// BatteryResolution's ceiling: off, or plugged in, it's the base; on battery its share of the base, rounded as the floor
static void test_battery_ceiling(void) {
    CHECK(dynres_battery_ceiling(100, 0, true) == 100);   // off
    CHECK(dynres_battery_ceiling(100, 75, false) == 100); // plugged in
    CHECK(dynres_battery_ceiling(100, 75, true) == 75);
    CHECK(dynres_battery_ceiling(100, 85, true) == 85);
    CHECK(dynres_battery_ceiling(80, 70, true) == 55);    // 56, down to a whole step
    CHECK(dynres_battery_ceiling(60, 75, true) == 50);    // not below the engine's 50
    CHECK(dynres_battery_ceiling(150, 70, true) == 105);
    dynres_t d;
    dynres_reset(&d, 100, 85);
    CHECK(d.ceiling == 100);
    dynres_set_ceiling(&d, 120); // never above the base
    CHECK(d.ceiling == 100);
    dynres_set_ceiling(&d, 75);
    CHECK(d.ceiling == 75);
    dynres_reset(&d, 100, 85); // a reset lifts it
    CHECK(d.ceiling == 100);
}

// Setting the ceiling to the base every window changes nothing: the same windows give the same sizes, events and
// failed trials as a controller whose ceiling is never set, with and without a frame limit. (Both run this code: what
// it shows is that dynres_set_ceiling(base), what the glue does plugged in or with the setting off, is a no-op. That
// this code decides as the controller before BatteryResolution did was compared outside the tests, by the review of
// 2026-11-18: the old and the new controller over 6 million windows, identical.)
static void test_ceiling_at_base_changes_nothing(void) {
    for (int kind = 0; kind < 3; kind++) {
        dynres_t with, without;
        dynres_reset(&with, 100, 60);
        dynres_reset(&without, 100, 60);
        unsigned seed = 1 + kind;
        for (int i = 0; i < 20000; i++) {
            seed = seed * 1103515245u + 12345u;
            double target = kind == 0 ? 120 : 40;
            double fps = target * (0.7 + ((seed >> 16) % 31) / 100.0), busy = ((seed >> 8) % 103) / 100.0;
            dynres_set_ceiling(&with, 100);
            int a = dynres_window_limited(&with, fps, busy, target, (dynres_limit_t)kind);
            int b = dynres_window_limited(&without, fps, busy, target, (dynres_limit_t)kind);
            CHECK(a == b && with.event == without.event && with.failed_trials == without.failed_trials);
            if (a != b)
                break;
        }
    }
}

// On battery: at once down to the ceiling, even right after a change and with the frame rate met; no step up past it
static void test_ceiling_on_battery(void) {
    dynres_t d;
    dynres_reset(&d, 100, 85);
    dynres_set_ceiling(&d, 70);
    CHECK(dynres_window(&d, 60, 0.40, 60) == 70 && d.event == Dynres_Event_Ceiling); // the first window
    // plenty of room for many windows: stays at the ceiling, every window
    int moved = 0;
    for (int i = 0; i < 1000; i++)
        moved += dynres_window(&d, 60, 0.20, 60) != 70;
    CHECK(moved == 0);
    // GPU-bound at the ceiling, which is below DynamicResolution's floor: it doesn't go lower, nor back to the floor
    for (int i = 0; i < 100; i++)
        moved += dynres_window(&d, 40, 0.99, 60) != 70;
    CHECK(moved == 0);
    // a ceiling above DynamicResolution's floor keeps its range under the ceiling: from 90 down to 85, not lower
    dynres_reset(&d, 100, 85);
    dynres_set_ceiling(&d, 90);
    CHECK(dynres_window(&d, 120, 0.50, 120) == 90);
    for (int i = 0; i < 100; i++)
        dynres_window(&d, 80, 0.99, 120);
    CHECK(d.percent == 85);
    int above = 0;
    for (int i = 0; i < 1000; i++)
        above += dynres_window(&d, 120, 0.30, 120) > 90;
    CHECK(above == 0 && d.percent == 90); // back up in steps, but never past the ceiling
}

// Plugged in again: at once back to DynamicResolution's floor if below it, and from there in the usual steps
static void test_ceiling_lifted(void) {
    dynres_t d;
    dynres_reset(&d, 100, 85);
    dynres_set_ceiling(&d, 70);
    dynres_window(&d, 60, 0.40, 60);
    CHECK(d.percent == 70);
    dynres_set_ceiling(&d, 100);
    CHECK(dynres_window(&d, 60, 0.40, 60) == 85); // at once, without the hold
    for (int i = 0; i < Dynres_Hold_Up - 1; i++)
        CHECK(dynres_window(&d, 120, 0.50, 120) == 85);
    CHECK(dynres_window(&d, 120, 0.50, 120) == 90); // then the usual step up after the headroom's hold
    // DynamicResolution off (the floor is the base): straight back to the user's own resolution
    dynres_reset(&d, 100, 100);
    dynres_set_ceiling(&d, 75);
    CHECK(dynres_window(&d, 30, 0.30, 30) == 75);
    for (int i = 0; i < 100; i++)
        CHECK(dynres_window(&d, 30, 0.99, 30) == 75); // GPU-bound or not, nothing else happens on battery
    dynres_set_ceiling(&d, 100);
    CHECK(dynres_window(&d, 30, 0.30, 30) == 100);
    for (int i = 0; i < 100; i++)
        CHECK(dynres_window(&d, 20, 0.99, 30) == 100); // and no dynamic steps down with DynamicResolution off
}

// A base that isn't a whole number of steps: the steps from it don't land on the ceiling, and a step up from under the
// ceiling stops at it instead of going past it
static void test_ceiling_between_steps(void) {
    dynres_t d;
    dynres_reset(&d, 83, 50);
    for (int i = 0; i < 100 && d.percent > 58; i++)
        dynres_window(&d, 110, 0.99, 120); // 83, 78, 73, 68, 63, 58
    CHECK(d.percent == 58);
    dynres_set_ceiling(&d, dynres_battery_ceiling(83, 75, true)); // 62, down to 60
    CHECK(d.ceiling == 60);
    int above = 0;
    for (int i = 0; i < 500; i++)
        above += dynres_window(&d, 120, 0.20, 120) > 60;
    CHECK(above == 0 && d.percent == 60);
}

// At the ceiling (below the base) there's no step up to wait for: the windows with room don't count as a change, so a
// GPU-bound window right after them may step down at once
static void test_ceiling_no_idle_change(void) {
    dynres_t d;
    dynres_reset(&d, 100, 85);
    dynres_set_ceiling(&d, 90);
    CHECK(dynres_window(&d, 120, 0.50, 120) == 90);
    for (int i = 0; i < Dynres_Hold_Up; i++)
        CHECK(dynres_window(&d, 120, 0.30, 120) == 90);
    CHECK(dynres_window(&d, 105, 0.99, 120) == 85);
}

// a window without a frame rate (a stall, the first after a pause) still applies the ceiling
static void test_ceiling_without_frame_rate(void) {
    dynres_t d;
    dynres_reset(&d, 100, 85);
    dynres_set_ceiling(&d, 75);
    CHECK(dynres_window(&d, 0, 0, 60) == 75);
    dynres_set_ceiling(&d, 100);
    CHECK(dynres_window(&d, 60, 0.5, 0) == 85);
}

// Under the battery's frame limit (the trials of dynres.h): the trials climb to the ceiling and stop there; a trial in
// progress when the ceiling comes down just ends (not counted as failed), and a trial is never taken back above it
static void test_ceiling_and_trials(void) {
    dynres_t d;
    dynres_reset(&d, 100, 60);
    dynres_start_at_floor(&d);
    dynres_set_ceiling(&d, 70);
    int windows = 0, above = 0;
    while (windows < 400) {
        above += dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit) > 70;
        windows++;
    }
    CHECK(above == 0 && d.percent == 70); // 60, 65, 70 by trials, then no more
    CHECK(d.failed_trials == 0);
    // unplugged in the middle of a trial (85 -> 90 still on trial): down to the ceiling at once, the trial ends
    dynres_reset(&d, 100, 60);
    d.percent = 85;
    for (int i = 0; i < 400 && d.percent < 90; i++)
        dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit);
    CHECK(d.percent == 90 && d.trial_from == 85 && d.trial_watched);
    d.failed_trials = 2;
    dynres_set_ceiling(&d, 70);
    CHECK(dynres_window_limited(&d, 40.0, 0.8, 40, Dynres_Match_Limit) == 70);
    CHECK(d.event == Dynres_Event_Ceiling && d.trial_from == 0 && !d.trial_watched && d.failed_trials == 2);
    // GPU-bound at once after that: it holds first (a change was just made), and never goes back up to the trial's 85
    for (int i = 0; i < 20; i++)
        CHECK(dynres_window_limited(&d, 30.0, 1.0, 40, Dynres_Match_Limit) <= 70);
    CHECK(d.percent == 60); // down to the floor under the ceiling, the GPU still behind
    // in the pause menu (a situation's limit): the ceiling applies at once all the same, both ways
    dynres_reset(&d, 100, 60);
    dynres_set_ceiling(&d, 75);
    CHECK(dynres_window_limited(&d, 30.0, 0.5, 30, Dynres_Situation_Limit) == 75);
    dynres_reset(&d, 100, 85);
    d.percent = 85;
    dynres_set_ceiling(&d, 70);
    CHECK(dynres_window_limited(&d, 30.0, 0.5, 30, Dynres_Situation_Limit) == 70);
    dynres_set_ceiling(&d, 100);
    CHECK(dynres_window_limited(&d, 30.0, 0.5, 30, Dynres_Situation_Limit) == 85);
    CHECK(dynres_window_limited(&d, 30.0, 0.2, 30, Dynres_Situation_Limit) == 85); // and no further in a pause
}

// BatteryResolution's log line: a game started plugged in says nothing (gcp3_perfbatt_battresplug said "plugged in,
// up to 100% again" at its first window, on mains from its first moment); on battery once per change; plugged in
// again only after that
static void test_ceiling_news(void) {
    int logged = 0;
    CHECK(dynres_ceiling_news(100, 100, &logged) == Dynres_Ceiling_Nothing && logged == 100); // started plugged in
    CHECK(dynres_ceiling_news(100, 100, &logged) == Dynres_Ceiling_Nothing);
    CHECK(dynres_ceiling_news(70, 100, &logged) == Dynres_Ceiling_On_Battery && logged == 70); // unplugged
    CHECK(dynres_ceiling_news(70, 100, &logged) == Dynres_Ceiling_Nothing);
    CHECK(dynres_ceiling_news(100, 100, &logged) == Dynres_Ceiling_Plugged_In && logged == 100); // plugged in again
    CHECK(dynres_ceiling_news(100, 100, &logged) == Dynres_Ceiling_Nothing);
    // started on battery: said at once; another setting on battery: said again
    logged = 0;
    CHECK(dynres_ceiling_news(70, 100, &logged) == Dynres_Ceiling_On_Battery);
    CHECK(dynres_ceiling_news(85, 100, &logged) == Dynres_Ceiling_On_Battery && logged == 85);
    // a new base (the controller started again, the log's memory cleared) plugged in: nothing
    logged = 0;
    CHECK(dynres_ceiling_news(90, 90, &logged) == Dynres_Ceiling_Nothing && logged == 90);
}

// Plugged in (the ceiling rises and the battery's frame limit lifts in the same window): that window and the holds after
// it don't step down, though its frames look far behind the display's rate and the GPU reads busy at the slow clock of
// the limit; a controller without the rise (DynamicResolution alone, at the same size) steps down two in that window,
// which is what the plug-in did before (the review of 2026-11-18, finding 1). After the holds, a window really behind
// with the GPU busy steps down as DynamicResolution does.
static void test_plug_in_hold(void) {
    dynres_t d, e;
    dynres_reset(&d, 100, 60);
    dynres_set_ceiling(&d, 70);
    CHECK(dynres_window_limited(&d, 40.0, 0.5, 40, Dynres_Match_Limit) == 70); // unplugged: at once
    for (int i = 0; i < 20; i++)
        dynres_window_limited(&d, 40.0, 0.92, 40, Dynres_Match_Limit);
    CHECK(d.percent == 70 && !d.ceiling_rose);
    dynres_reset(&e, 100, 60); // DynamicResolution alone, settled at 70% (a heavy moment brought it there)
    e.percent = 70;
    e.windows_since_change = 20;
    dynres_set_ceiling(&d, 100); // plugged in
    CHECK(d.ceiling_rose);
    CHECK(dynres_window_limited(&d, 45.0, 0.94, 120, Dynres_No_Limit) == 70 && !d.ceiling_rose);
    CHECK(dynres_window_limited(&e, 45.0, 0.94, 120, Dynres_No_Limit) == 60); // without the rise: two steps down
    for (int i = 0; i < Dynres_Hold_Down; i++)
        CHECK(dynres_window_limited(&d, 45.0, 0.94, 120, Dynres_No_Limit) == 70);
    CHECK(dynres_window_limited(&d, 110.0, 0.99, 120, Dynres_No_Limit) == 65); // then DynamicResolution's own step
    // a ceiling coming down (unplugged) is no rise; nor a ceiling set to what it is
    dynres_reset(&d, 100, 60);
    dynres_set_ceiling(&d, 85);
    CHECK(!d.ceiling_rose);
    dynres_set_ceiling(&d, 85);
    CHECK(!d.ceiling_rose);
    // a rise that moves the size at once (BatteryResolution under DynamicResolution's floor): the move, and the flag
    // ends with it (the change's own holds follow)
    dynres_reset(&d, 100, 85);
    dynres_set_ceiling(&d, 70);
    dynres_window_limited(&d, 40.0, 0.5, 40, Dynres_Match_Limit);
    dynres_set_ceiling(&d, 100);
    CHECK(dynres_window_limited(&d, 45.0, 0.94, 120, Dynres_No_Limit) == 85 && !d.ceiling_rose &&
          d.event == Dynres_Event_Ceiling);
}

// A warm-up ended early (the feature turned off in the middle of it) doesn't resume: nothing more until a map arms it
// again, and then it starts afresh with the base of then (the review of 2026-11-18, finding 5)
static void test_prewarm_cancel(void) {
    dynres_prewarm_t p;
    dynres_prewarm_init(&p);
    CHECK(dynres_prewarm_frame(&p, 100, 85, true, false) == 95);
    CHECK(dynres_prewarm_running(&p));
    dynres_prewarm_cancel(&p);
    CHECK(!dynres_prewarm_running(&p));
    for (int i = 0; i < 20; i++)
        CHECK(dynres_prewarm_frame(&p, 90, 75, true, true) == 0); // the feature on again, the user's value now 90
    p.armed = true; // the next map
    CHECK(dynres_prewarm_frame(&p, 90, 75, true, false) == 85);
    CHECK(dynres_prewarm_frame(&p, 90, 75, true, true) == 80);
    CHECK(dynres_prewarm_frame(&p, 90, 75, true, true) == 75);
    CHECK(dynres_prewarm_frame(&p, 90, 75, true, true) == 90); // back to the base of now, not 100
}

// The match's warm-up under the ceiling: the ceiling's size first and the steps under it, down to the floor under it;
// back to the user's value at the end. At the base it's the warm-up without a ceiling.
static void test_prewarm_under_ceiling(void) {
    dynres_prewarm_t p, q;
    dynres_prewarm_init(&p);
    CHECK(dynres_prewarm_frame_up_to(&p, 100, 60, 70, true, false) == 70);
    CHECK(dynres_prewarm_frame_up_to(&p, 100, 60, 70, true, true) == 65);
    CHECK(dynres_prewarm_frame_up_to(&p, 100, 60, 70, true, true) == 60);
    CHECK(dynres_prewarm_frame_up_to(&p, 100, 60, 70, true, true) == 100);
    CHECK(!dynres_prewarm_running(&p));
    // the ceiling under DynamicResolution's floor (85): just the ceiling
    dynres_prewarm_init(&p);
    CHECK(dynres_prewarm_frame_up_to(&p, 100, 85, 70, true, false) == 70);
    CHECK(dynres_prewarm_frame_up_to(&p, 100, 85, 70, true, true) == 100);
    // DynamicResolution off (the floor is the base): just the ceiling too
    dynres_prewarm_init(&p);
    CHECK(dynres_prewarm_frame_up_to(&p, 100, 100, 75, true, false) == 75);
    CHECK(dynres_prewarm_frame_up_to(&p, 100, 100, 75, true, true) == 100);
    // the ceiling at the base: frame by frame the same as dynres_prewarm_frame
    dynres_prewarm_init(&p);
    dynres_prewarm_init(&q);
    for (int i = 0; i < 40; i++) {
        bool ready = i > 3, set = i % 3 == 0;
        CHECK(dynres_prewarm_frame_up_to(&p, 100, 85, 100, ready, set) == dynres_prewarm_frame(&q, 100, 85, ready, set));
    }
}

int main(void) {
    test_eviction_victim();
    test_table_changes();
    test_engine_stress();
    test_engine_play();
    test_engine_edge();
    test_climb_at_limit();
    test_trial_taken_back();
    test_no_limit_unchanged();
    test_trial_rules();
    test_pause_without_limit();
    test_pause_during_trial();
    test_failing_trials_30_minutes();
    test_limit_kinds();
    test_late_falls_thin_out();
    test_late_heavy_moment();
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
    test_battery_ceiling();
    test_ceiling_at_base_changes_nothing();
    test_ceiling_on_battery();
    test_ceiling_lifted();
    test_ceiling_between_steps();
    test_ceiling_no_idle_change();
    test_ceiling_without_frame_rate();
    test_ceiling_and_trials();
    test_prewarm_under_ceiling();
    test_ceiling_news();
    test_plug_in_hold();
    test_prewarm_cancel();
    if (failures == 0)
        printf("test_dynres: all passed\n");
    return failures != 0;
}
