// See frame_pace.h.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "frame_pace.h"

static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

// how many refreshes a frame that stayed interval seconds on screen counts as: the nearest whole number, at least 1
static int refreshes_of(double interval, double refresh) {
    long whole = lround(interval / refresh);
    return whole < 1 ? 1 : whole > Frame_Pace_Buckets ? Frame_Pace_Buckets : (int)whole;
}

void frame_pace_summarize(const double *times, int count, double refresh, double *work, frame_pace_t *out) {
    memset(out, 0, sizeof(*out));
    int shown = 0;
    for (int i = 0; i < count; i++)
        if (times[i] > 0)
            work[shown++] = times[i];
    if (shown < 2 || !(refresh > 0))
        return;
    qsort(work, shown, sizeof(work[0]), compare_doubles);
    double span = work[shown - 1] - work[0];
    // the intervals between the times, in place: work[i] becomes how long frame i stayed on screen
    int previous = 0;
    for (int i = 0; i + 1 < shown; i++) {
        double interval = work[i + 1] - work[i];
        work[i] = interval;
        int refreshes = refreshes_of(interval, refresh);
        out->refreshes[refreshes - 1]++;
        if (previous != 0 && refreshes != previous)
            out->changes++;
        previous = refreshes;
        double whole = (double)lround(interval / refresh);
        if (fabs(interval / refresh - (whole < 1 ? 1 : whole)) >= 0.25)
            out->off_grid++;
    }
    out->frames = shown - 1;
    out->fps = span > 0 ? out->frames / span : 0;
    qsort(work, out->frames, sizeof(work[0]), compare_doubles);
    double low = work[(int)(out->frames * 0.99)];
    out->low_1_fps = low > 0 ? 1.0 / low : 0;
    out->worst_ms = work[out->frames - 1] * 1000.0;
}
