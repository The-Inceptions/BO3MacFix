// Tests for thread_stats.c and thread_stats_game.c (FrameStats' detail): the threads' line (CPU time between two
// samples, biggest first, roles, the rest summed), the phases' line, and this process's own threads read by name with
// their CPU time, which is what the game's log shows.
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../source/thread_stats_game.c"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Ms 1000000ULL

static void test_threads_line(void) {
    thread_sample_t before[] = {
        { 10, 100 * Ms, "Main" },
        { 11, 50 * Ms, "Win (WinID 3)" },
        { 12, 10 * Ms, "JQ Worker 0 (Processor 1)" },
        { 13, 7 * Ms, "ended in the window" },
        { 14, 5 * Ms, "" },
    };
    thread_sample_t now[] = {
        { 10, 4100 * Ms, "Main" },                       // 4000 ms in 5 s: 80%
        { 11, 2050 * Ms, "Win (WinID 3)" },              // 2000 ms: 40%
        { 12, 810 * Ms, "JQ Worker 0 (Processor 1)" },   // 800 ms: 16%
        { 14, 105 * Ms, "" },                            // 100 ms: 2%, no name
        { 15, 50 * Ms, "started in the window" },        // all its 50 ms: 1%
        { 16, 0, "idle" },                               // nothing: not listed, not in the rest
    };
    uint64_t roles[3] = { 11, 10, 10 }; // present, server, client
    char line[1000];
    int n = thread_stats_format(before, 5, now, 6, roles, 3, 5.0, 500, 12, line, sizeof(line));
    CHECK(n == (int)strlen(line));
    CHECK(strcmp(line, "(% of a core, ms a frame) all 6 threads 139% 13.90 ms; Main [server client] 80% 8.00 ms; "
                       "Win (WinID 3) [present] 40% 4.00 ms; JQ Worker 0 (Processor 1) 16% 1.60 ms; unnamed 14 2% "
                       "0.20 ms; started in the window 1% 0.10 ms") == 0);
    if (failures)
        printf("%s\n", line);

    // only the two biggest by name, the rest summed (the idle one isn't counted among them)
    n = thread_stats_format(before, 5, now, 6, roles, 3, 5.0, 500, 2, line, sizeof(line));
    CHECK(strcmp(line, "(% of a core, ms a frame) all 6 threads 139% 13.90 ms; Main [server client] 80% 8.00 ms; "
                       "Win (WinID 3) [present] 40% 4.00 ms; 3 more 19% 1.90 ms") == 0);

    // a role whose thread isn't there (not seen yet, or it ended)
    uint64_t missing[3] = { 0, 13, 10 };
    thread_stats_format(before, 5, now, 6, missing, 3, 5.0, 500, 1, line, sizeof(line));
    CHECK(strstr(line, "; Main [client] 80% 8.00 ms; 4 more 59% 5.90 ms; no present thread seen; no server thread seen")
          != NULL);

    // equal times keep the system's order; a time that went backwards counts as nothing
    thread_sample_t same_before[] = { { 1, 10 * Ms, "b" }, { 2, 10 * Ms, "a" }, { 3, 900 * Ms, "back" } };
    thread_sample_t same_now[] = { { 1, 20 * Ms, "b" }, { 2, 20 * Ms, "a" }, { 3, 800 * Ms, "back" } };
    uint64_t none[3] = { 1, 1, 1 };
    thread_stats_format(same_before, 3, same_now, 3, none, 3, 1.0, 100, 5, line, sizeof(line));
    CHECK(strcmp(line, "(% of a core, ms a frame) all 3 threads 2% 0.20 ms; b [present server client] 1% 0.10 ms; "
                       "a 1% 0.10 ms") == 0);

    // a buffer too small is cut, and the length it would have had comes back
    char small[16];
    n = thread_stats_format(before, 5, now, 6, roles, 3, 5.0, 500, 12, small, sizeof(small));
    CHECK(n == (int)strlen(line) || n > (int)sizeof(small));
    CHECK(strlen(small) == sizeof(small) - 1);
    // no frames, no seconds: zeros, not a division by zero
    thread_stats_format(before, 5, now, 6, roles, 3, 0, 0, 1, line, sizeof(line));
    const char *zeros = "(% of a core, ms a frame) all 6 threads 0% 0.00 ms; Main [server client] 0% 0.00 ms;";
    CHECK(strncmp(line, zeros, strlen(zeros)) == 0);
}

static void test_phases_line(void) {
    phase_sum_t sums[Phase_Count] = { { 500, 1500 * Ms, 1400 * Ms }, { 1000, 2000 * Ms, 1900 * Ms } };
    char line[400];
    int n = thread_stats_format_phases(sums, 500, line, sizeof(line));
    CHECK(n == (int)strlen(line));
    CHECK(strcmp(line, "(ms a frame, CPU in brackets, calls a frame) server frame (SV_Frame) 3.00 ms (2.80) 1.00; "
                       "client frame (CL_Frame) 4.00 ms (3.80) 2.00") == 0);
    phase_sum_t empty[Phase_Count] = { { 0, 0, 0 }, { 0, 0, 0 } };
    thread_stats_format_phases(empty, 0, line, sizeof(line));
    CHECK(strcmp(line, "(ms a frame, CPU in brackets, calls a frame) server frame (SV_Frame) 0.00 ms (0.00) 0.00; "
                       "client frame (CL_Frame) 0.00 ms (0.00) 0.00") == 0);
}

static volatile int burning = 1;
static void *burner(void *argument) {
    (void)argument;
    pthread_setname_np("test burner");
    uint64_t start = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    while (burning && clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - start < 300 * Ms)
        ;
    return NULL;
}

static void spin(uint64_t ns) {
    uint64_t start = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    while (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - start < ns)
        ;
}

// the part in the game: nothing before thread_stats_init, then this process's threads by name with their time
static void test_in_process(void) {
    // before init: no role is noted
    thread_stats_note_role(Thread_Role_Present);
    CHECK(atomic_load(&role_ids[0]) == 0);
    CHECK(!thread_stats_phases_on);
    thread_stats_init();
    CHECK(thread_stats_phases_on);

    thread_sample_t first[Most_Threads];
    int first_count = sample_threads(first, Most_Threads);
    CHECK(first_count >= 1);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, burner, NULL) == 0);
    // the burner names itself and burns; it must still be there when the second sample is taken, so sample while it
    // runs: its 300 ms of CPU take at least that long
    spin(100 * Ms);
    thread_sample_t second[Most_Threads];
    int second_count = sample_threads(second, Most_Threads);
    burning = 0;
    pthread_join(thread, NULL);
    bool found = false;
    for (int i = 0; i < second_count; i++) {
        if (strcmp(second[i].name, "test burner") == 0) {
            found = true;
            CHECK(second[i].cpu_ns > 10 * Ms);
            CHECK(second[i].id != 0);
        }
    }
    CHECK(found);
    // this thread's own time went up by about the 100 ms it spun
    uint64_t self = current_thread_id(), self_before = 0, self_after = 0;
    for (int i = 0; i < first_count; i++)
        if (first[i].id == self)
            self_before = first[i].cpu_ns;
    for (int i = 0; i < second_count; i++)
        if (second[i].id == self)
            self_after = second[i].cpu_ns;
    CHECK(self_after >= self_before + 90 * Ms);

    // a phase: its calls, wall clock and CPU time, and the role of the thread that ran it
    phase_start_t start = thread_stats_phase_begin();
    spin(20 * Ms);
    thread_stats_phase_end(Phase_Client, start);
    CHECK(atomic_load(&phases[Phase_Client].calls) == 1);
    CHECK(atomic_load(&phases[Phase_Client].cpu_ns) >= 19 * Ms);
    CHECK(atomic_load(&phases[Phase_Client].wall_ns) >= atomic_load(&phases[Phase_Client].cpu_ns) - 1 * Ms);
    CHECK(atomic_load(&role_ids[2]) == self);
    CHECK(atomic_load(&role_ids[1]) == 0);

    // the windows: the first only takes a sample and empties the phases, the second prints both lines
    thread_stats_window(1.0, 100);
    CHECK(atomic_load(&role_ids[0]) == self);
    CHECK(atomic_load(&phases[Phase_Client].calls) == 0);
    CHECK(!window_first);
    CHECK(counts[current] >= 1);
    // the second window prints both lines (to the log in the game); its samples are the threads of now
    thread_stats_window(1.0, 100);
    CHECK(!window_first);
}

int main(void) {
    test_threads_line();
    test_phases_line();
    test_in_process();
    if (failures == 0)
        printf("thread_stats: all tests passed\n");
    return failures != 0;
}
