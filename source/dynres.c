// See dynres.h.
#include "dynres.h"
#include "frame_limit.h"
#include <dispatch/dispatch.h> // DISPATCH_MEMORYPRESSURE_*

void dynres_reset(dynres_t *d, int base, int lowest) {
    d->base = base;
    d->lowest = lowest < base ? lowest : base;
    d->percent = base;
    d->windows_since_change = 0;
    d->headroom_windows = 0;
    d->trial_from = 0;
    d->trial_windows = 0;
    d->trial_watched = false;
    d->failed_trials = 0;
    d->limit = Dynres_No_Limit;
    d->event = Dynres_Event_None;
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

int dynres_start_at_floor(dynres_t *d) {
    d->trial_from = 0;
    d->trial_watched = false;
    return change(d, d->lowest);
}

// a trial that didn't hold, taken back or failed late: no longer on trial or watched, and the next one's hold doubles,
// up to Dynres_Max_Backoff times
static void trial_failed(dynres_t *d) {
    d->trial_from = 0;
    d->trial_watched = false;
    if (d->failed_trials < Dynres_Max_Backoff)
        d->failed_trials++;
}

// under a frame limit: the windows in a row at the limit's rate before a trial step up, doubled for each trial that
// failed in a row
static int trial_hold(const dynres_t *d, double target_fps) {
    int windows = (int)(Dynres_Hold_Up_Seconds * target_fps / Dynres_Window_Frames + 0.5);
    if (windows < Dynres_Hold_Down + 1)
        windows = Dynres_Hold_Down + 1;
    return windows << d->failed_trials;
}

int dynres_window(dynres_t *d, double fps, double gpu_busy, double target_fps) {
    return dynres_window_limited(d, fps, gpu_busy, target_fps, Dynres_No_Limit);
}

int dynres_window_limited(dynres_t *d, double fps, double gpu_busy, double target_fps, dynres_limit_t limit) {
    d->windows_since_change++;
    d->event = Dynres_Event_None;
    // another kind of limit: the count of windows with room starts again; back to none, a trial is kept as it is
    if ((int)limit != d->limit) {
        d->headroom_windows = 0;
        if (limit == Dynres_No_Limit) {
            d->trial_from = 0;
            d->trial_watched = false;
            d->failed_trials = 0;
        }
        d->limit = limit;
    }
    if (target_fps <= 0 || fps <= 0)
        return d->percent;
    bool settled = d->windows_since_change > Dynres_Hold_Down;
    bool behind = fps < target_fps * Dynres_Missed;

    // a trial step up under a limit: taken back at once if the GPU falls behind within its first Dynres_Trial_Windows,
    // kept after them, and watched until Dynres_Trial_Watch: only a size that lasted that long clears the count of
    // failed trials (a step down within it counts as one, below). Its windows count in the match only: a pause or the
    // background holds any size and says nothing about it.
    if (d->trial_watched && limit == Dynres_Match_Limit) {
        d->trial_windows++;
        if (d->trial_windows > Dynres_Trial_Windows)
            d->trial_from = 0;
        if (d->trial_windows > Dynres_Trial_Watch) {
            d->trial_watched = false;
            d->failed_trials = 0;
        }
    }
    if (d->trial_from != 0 && settled && behind && gpu_busy >= Dynres_GPU_Bound) {
        int back = d->trial_from;
        trial_failed(d);
        d->event = Dynres_Event_Taken_Back;
        return change(d, back);
    }

    // behind and the GPU is why: a step down, two when far behind
    if (behind && gpu_busy >= Dynres_GPU_Bound) {
        d->headroom_windows = 0;
        if (!settled || d->percent <= d->lowest)
            return d->percent;
        int steps = fps < target_fps * Dynres_Far_Behind ? 2 : 1;
        int percent = d->percent - steps * Dynres_Step;
        // a kept trial still watched, at a limit of the match: it failed all the same, late (Dynres_Trial_Watch); in a
        // pause or the background the step only ends the watch
        if (d->trial_watched && limit == Dynres_Match_Limit) {
            trial_failed(d);
            d->event = Dynres_Event_Failed_Late;
        }
        d->trial_watched = false;
        return change(d, percent > d->lowest ? percent : d->lowest);
    }

    if (d->percent >= d->base)
        return d->percent;
    // in a pause, the menus or the background: no step up of any kind (see dynres_window_limited)
    if (limit == Dynres_Situation_Limit)
        return d->percent;
    int up = d->percent + Dynres_Step < d->base ? d->percent + Dynres_Step : d->base;
    // under a limit: room is the limit held, for the trial's hold in a row (the busy share is the slowed clock's)
    if (limit == Dynres_Match_Limit) {
        if (behind) {
            d->headroom_windows = 0;
            return d->percent;
        }
        // (the hold is longer than the holds after a change, so this is always settled)
        if (++d->headroom_windows < trial_hold(d, target_fps) || d->trial_from != 0)
            return d->percent;
        int from = d->percent;
        change(d, up);
        d->trial_from = from;
        d->trial_windows = 0;
        d->trial_watched = true;
        d->event = Dynres_Event_Trial;
        return up;
    }

    // a step up, once it would have fit for a while: the GPU's work grows with the pixels, the square of the scale
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

int dynres_target(int display_fps, int frame_limit_fps, int max_fps) {
    int target = display_fps > 0 ? display_fps : 0;
    int limit = frame_limit_shown(frame_limit_fps, display_fps); // never above the display's rate
    if (limit > 0)
        target = limit;
    if (max_fps > 0 && (target <= 0 || max_fps < target))
        target = max_fps;
    return target;
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

bool dynres_keep_release(dynres_keep_t *keep) {
    bool kept = false;
    for (int i = 0; i < Dynres_Keep_Max; i++) {
        kept |= keep->size[i] != 0;
        keep->size[i] = 0;
    }
    return kept;
}

bool dynres_pressure_after(unsigned long events, bool before) {
    if (events & (DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL))
        return true;
    if (events & DISPATCH_MEMORYPRESSURE_NORMAL)
        return false;
    return before;
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
