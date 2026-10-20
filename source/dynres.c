// See dynres.h.
#include "dynres.h"

void dynres_reset(dynres_t *d, int base, int lowest) {
    d->base = base;
    d->lowest = lowest < base ? lowest : base;
    d->percent = base;
    d->windows_since_change = 0;
    d->headroom_windows = 0;
}

int dynres_lowest_for(int base, int setting) {
    int lowest = base * setting / 100;
    lowest -= lowest % Dynres_Step;
    if (lowest < 50)
        lowest = 50;
    return lowest < base ? lowest : base;
}

static int change(dynres_t *d, int percent) {
    d->percent = percent;
    d->windows_since_change = 0;
    d->headroom_windows = 0;
    return percent;
}

int dynres_window(dynres_t *d, double fps, double gpu_busy, double target_fps) {
    d->windows_since_change++;
    if (target_fps <= 0 || fps <= 0)
        return d->percent;
    bool settled = d->windows_since_change > Dynres_Hold_Down;

    // behind and the GPU is why: a step down, two when far behind
    if (fps < target_fps * Dynres_Missed && gpu_busy >= Dynres_GPU_Bound) {
        d->headroom_windows = 0;
        if (!settled || d->percent <= d->lowest)
            return d->percent;
        int steps = fps < target_fps * Dynres_Far_Behind ? 2 : 1;
        int percent = d->percent - steps * Dynres_Step;
        return change(d, percent > d->lowest ? percent : d->lowest);
    }

    // a step up, once it would have fit for a while: the GPU's work grows with the pixels, the square of the scale
    if (d->percent >= d->base)
        return d->percent;
    int up = d->percent + Dynres_Step < d->base ? d->percent + Dynres_Step : d->base;
    double scale = (double)up / d->percent;
    // at a frame rate under the target the GPU time per frame counts, not its share of the slower frames
    double busy = fps < target_fps ? gpu_busy * target_fps / fps : gpu_busy;
    if (busy * scale * scale >= Dynres_Headroom) {
        d->headroom_windows = 0;
        return d->percent;
    }
    if (++d->headroom_windows < Dynres_Hold_Up || !settled)
        return d->percent;
    return change(d, up);
}
