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

void dynres_keep_add(dynres_keep_t *keep, uint32_t size) {
    if (size == 0)
        return;
    int at = Dynres_Keep_Max - 1; // not kept yet: the oldest goes
    for (int i = 0; i < Dynres_Keep_Max; i++)
        if (keep->size[i] == size)
            at = i;
    for (int i = at; i > 0; i--)
        keep->size[i] = keep->size[i - 1];
    keep->size[0] = size;
}

unsigned dynres_keep_mask(const dynres_keep_t *keep, const uint32_t sizes[Dynres_Sets],
                          const uint64_t last_use[Dynres_Sets], uint64_t frame) {
    unsigned mask = 0;
    for (int set = 1; set < Dynres_Sets; set++) {
        if (sizes[set] == 0 || last_use[set] > frame || frame - last_use[set] > Dynres_Keep_Fresh)
            continue;
        for (int i = 0; i < Dynres_Keep_Max; i++)
            if (keep->size[i] == sizes[set])
                mask |= 1u << set;
    }
    return mask;
}

int dynres_prewarm_sizes(int base, int lowest, int out[Dynres_Keep_Max]) {
    int count = 0;
    for (int percent = base - Dynres_Step; count < Dynres_Keep_Max; percent -= Dynres_Step) {
        int size = percent > lowest ? percent : lowest; // the controller's steps stop at the floor
        if (size >= base || (count > 0 && size == out[count - 1]))
            break;
        out[count++] = size;
    }
    return count;
}

void dynres_prewarm_init(dynres_prewarm_t *p) {
    p->count = 0;
    p->index = -1;
    p->frames = 0;
    p->base = 0;
    p->armed = true;
}

int dynres_prewarm_frame(dynres_prewarm_t *p, int base, int lowest, bool ready, bool new_set) {
    if (p->index < 0) {
        if (!p->armed || !ready)
            return 0;
        p->armed = false;
        p->count = dynres_prewarm_sizes(base, lowest, p->percent);
        if (p->count == 0)
            return 0;
        p->base = base;
        p->index = 0;
        p->frames = 0;
        return p->percent[0];
    }
    if (!new_set && ++p->frames < Dynres_Prewarm_Wait)
        return 0;
    p->frames = 0;
    if (++p->index < p->count)
        return p->percent[p->index];
    p->index = -1;
    return p->base;
}

int dynres_set_for_percent(const uint32_t sizes[Dynres_Sets], int base, int percent) {
    if (sizes[0] == 0 || base <= 0)
        return -1;
    int width = (int)(sizes[0] >> 16) * percent / base, height = (int)(sizes[0] & 0xffff) * percent / base;
    for (int set = 1; set < Dynres_Sets; set++) {
        int w = (int)(sizes[set] >> 16), h = (int)(sizes[set] & 0xffff);
        if (sizes[set] != 0 && w >= width && w <= width + 8 && h >= height && h <= height + 8)
            return set;
    }
    return -1;
}
