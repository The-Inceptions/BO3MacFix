// FrameStats' detail: the threads' and the phases' lines as text, plain C (see thread_stats.h). The part that reads
// the process's threads and the clocks is thread_stats_game.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "thread_stats.h"

#define Most_Threads 512

typedef struct {
    int index; // into now
    uint64_t used_ns;
} thread_use_t;

static int compare_use(const void *a, const void *b) {
    const thread_use_t *x = a, *y = b;
    if (x->used_ns != y->used_ns)
        return x->used_ns < y->used_ns ? 1 : -1;
    return x->index - y->index; // the same time: in the order the system listed them
}

static uint64_t time_before(const thread_sample_t *before, int before_count, uint64_t id) {
    for (int i = 0; i < before_count; i++)
        if (before[i].id == id)
            return before[i].cpu_ns;
    return 0; // started in the window: all its time counts
}

// appends to out at *used, keeping track of what snprintf would have written
static void append(char *out, size_t size, int *used, const char *format, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static void append(char *out, size_t size, int *used, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    size_t at = *used < 0 ? size : (size_t)*used;
    int n = vsnprintf(at < size ? out + at : NULL, at < size ? size - at : 0, format, arguments);
    va_end(arguments);
    if (n < 0 || *used < 0)
        *used = -1;
    else
        *used += n;
}

int thread_stats_format(const thread_sample_t *before, int before_count, const thread_sample_t *now, int now_count,
                        const uint64_t *role_ids, int role_count, double seconds, int frames, int shown, char *out,
                        size_t size) {
    static const char *const role_names[] = { "present", "server", "client" };
    if (size > 0)
        out[0] = '\0';
    if (now_count > Most_Threads)
        now_count = Most_Threads;
    thread_use_t use[Most_Threads];
    uint64_t total_ns = 0;
    for (int i = 0; i < now_count; i++) {
        uint64_t then = time_before(before, before_count, now[i].id);
        use[i].index = i;
        use[i].used_ns = now[i].cpu_ns > then ? now[i].cpu_ns - then : 0;
        total_ns += use[i].used_ns;
    }
    qsort(use, (size_t)now_count, sizeof(use[0]), compare_use);
    // % of a core over the window and ms a frame
    double percent_per_ns = seconds > 0 ? 100.0 / (seconds * 1e9) : 0;
    double ms_per_ns = frames > 0 ? 1e-6 / frames : 0;
    int used = 0;
    append(out, size, &used, "(%% of a core, ms a frame) all %i threads %.0f%% %.2f ms", now_count,
           total_ns * percent_per_ns, total_ns * ms_per_ns);
    int listed = 0;
    uint64_t rest_ns = 0;
    int rest = 0;
    for (int i = 0; i < now_count; i++) {
        const thread_sample_t *thread = &now[use[i].index];
        if (listed >= shown || use[i].used_ns == 0) {
            rest_ns += use[i].used_ns;
            rest += use[i].used_ns > 0;
            continue;
        }
        listed++;
        if (thread->name[0] != '\0')
            append(out, size, &used, "; %.*s", (int)sizeof(thread->name), thread->name);
        else
            append(out, size, &used, "; unnamed %llu", (unsigned long long)thread->id);
        int tags = 0;
        for (int role = 0; role < role_count && role < 3; role++) {
            if (role_ids[role] == 0 || role_ids[role] != thread->id)
                continue;
            append(out, size, &used, "%s%s", tags++ == 0 ? " [" : " ", role_names[role]);
        }
        if (tags > 0)
            append(out, size, &used, "]");
        append(out, size, &used, " %.0f%% %.2f ms", use[i].used_ns * percent_per_ns, use[i].used_ns * ms_per_ns);
    }
    if (rest > 0)
        append(out, size, &used, "; %i more %.0f%% %.2f ms", rest, rest_ns * percent_per_ns, rest_ns * ms_per_ns);
    // a role whose thread has no time in the window (it ended, or the library hasn't seen the role yet) says so
    for (int role = 0; role < role_count && role < 3; role++) {
        bool found = false;
        for (int i = 0; i < now_count && role_ids[role] != 0; i++)
            found |= now[i].id == role_ids[role];
        if (!found)
            append(out, size, &used, "; no %s thread seen", role_names[role]);
    }
    return used;
}

int thread_stats_format_phases(const phase_sum_t sums[Phase_Count], int frames, char *out, size_t size) {
    static const char *const names[Phase_Count] = { "server frame (SV_Frame)", "client frame (CL_Frame)",
                                                    "library's view model work" };
    if (size > 0)
        out[0] = '\0';
    int used = 0;
    append(out, size, &used, "(ms a frame, CPU in brackets, calls a frame)");
    double per_frame = frames > 0 ? 1.0 / frames : 0;
    for (int phase = 0; phase < Phase_Count; phase++) {
        append(out, size, &used, "%s %s %.2f ms (%.2f) %.2f", phase == 0 ? "" : ";", names[phase],
               sums[phase].wall_ns * 1e-6 * per_frame, sums[phase].cpu_ns * 1e-6 * per_frame,
               sums[phase].calls * per_frame);
    }
    return used;
}
