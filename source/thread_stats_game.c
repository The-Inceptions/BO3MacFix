// FrameStats' detail in the game: reads the process's own threads (task_threads and thread_info on mach_task_self(),
// the same numbers ps -M shows, plus each thread's name) and times the phases, see thread_stats.h. Nothing here runs
// before thread_stats_init, which only FrameStats' detail calls.
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "thread_stats.h"

#define Most_Threads 512
#define Threads_Shown 12

bool thread_stats_phases_on = false;
static bool on = false;
static _Atomic uint64_t role_ids[3]; // the thread seen in each role (Thread_Role_* bit index), 0 not yet
static struct {
    _Atomic uint64_t calls, wall_ns, cpu_ns;
} phases[Phase_Count];

void thread_stats_init(void) {
    on = true;
    thread_stats_phases_on = true;
}

static uint64_t current_thread_id(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return id;
}

void thread_stats_note_role(int role) {
    if (!on)
        return;
    for (int bit = 0; bit < 3; bit++) {
        if ((role & (1 << bit)) == 0)
            continue;
        // a role moves to another thread only when it really does (a map restart can start threads anew)
        uint64_t id = current_thread_id();
        if (atomic_load_explicit(&role_ids[bit], memory_order_relaxed) != id)
            atomic_store_explicit(&role_ids[bit], id, memory_order_relaxed);
    }
}

phase_start_t thread_stats_phase_begin(void) {
    return (phase_start_t){ clock_gettime_nsec_np(CLOCK_UPTIME_RAW), clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) };
}

void thread_stats_phase_end(int phase, phase_start_t start) {
    uint64_t wall = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), cpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    atomic_fetch_add_explicit(&phases[phase].calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&phases[phase].wall_ns, wall - start.wall_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&phases[phase].cpu_ns, cpu - start.cpu_ns, memory_order_relaxed);
    // the first call of a window tells which thread runs the phase (cheap: a compare most of the time)
    if (atomic_load_explicit(&phases[phase].calls, memory_order_relaxed) == 1)
        thread_stats_note_role(phase == Phase_Server ? Thread_Role_Server : Thread_Role_Client);
}

// every thread of the process now: id, CPU time, name
static int sample_threads(thread_sample_t *samples, int most) {
    thread_act_array_t threads;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return 0;
    int taken = 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        thread_identifier_info_data_t identifier;
        mach_msg_type_number_t size = THREAD_IDENTIFIER_INFO_COUNT;
        thread_extended_info_data_t extended;
        mach_msg_type_number_t extended_size = THREAD_EXTENDED_INFO_COUNT;
        if (taken < most &&
            thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&identifier, &size) == KERN_SUCCESS &&
            thread_info(threads[i], THREAD_EXTENDED_INFO, (thread_info_t)&extended, &extended_size) == KERN_SUCCESS) {
            samples[taken].id = identifier.thread_id;
            samples[taken].cpu_ns = extended.pth_user_time + extended.pth_system_time;
            memcpy(samples[taken].name, extended.pth_name, sizeof(samples[taken].name));
            samples[taken].name[sizeof(samples[taken].name) - 1] = '\0';
            taken++;
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(threads[0]));
    return taken;
}

// the samples at the start and end of the window (current is the start's), whether there is none at the start yet
static thread_sample_t samples[2][Most_Threads];
static int counts[2], current = 0;
static bool window_first = true;
static uint64_t sampled_at = 0;

void thread_stats_window(double seconds, int frames) {
    if (!on)
        return;
    // the render thread calls this: it's the one that presents
    thread_stats_note_role(Thread_Role_Present);
    int next = current ^ 1;
    counts[next] = sample_threads(samples[next], Most_Threads);
    // FrameStats turned off and on again from the menu starts a new window without a sample at its start: that window
    // is skipped, like the first
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    double since = (now - sampled_at) * 1e-9;
    if (since > seconds * 1.25 + 0.5)
        window_first = true;
    sampled_at = now;
    uint64_t roles[3];
    for (int i = 0; i < 3; i++)
        roles[i] = atomic_load_explicit(&role_ids[i], memory_order_relaxed);
    char line[1600];
    // the first window has no sample before it: its threads' times would be since they started
    if (!window_first) {
        thread_stats_format(samples[current], counts[current], samples[next], counts[next], roles, 3, seconds, frames,
                            Threads_Shown, line, sizeof(line));
        printf("frame stats: threads %s\n", line);
    }
    phase_sum_t sums[Phase_Count];
    for (int phase = 0; phase < Phase_Count; phase++) {
        sums[phase].calls = atomic_exchange_explicit(&phases[phase].calls, 0, memory_order_relaxed);
        sums[phase].wall_ns = atomic_exchange_explicit(&phases[phase].wall_ns, 0, memory_order_relaxed);
        sums[phase].cpu_ns = atomic_exchange_explicit(&phases[phase].cpu_ns, 0, memory_order_relaxed);
    }
    if (!window_first) {
        thread_stats_format_phases(sums, frames, line, sizeof(line));
        printf("frame stats: phases %s\n", line);
    }
    window_first = false;
    current = next;
}
