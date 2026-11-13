// FrameStats' detail (BO3MACFIX_FRAMESTATSDETAIL=1): where the game's CPU time goes, two more lines each window.
// - "frame stats: threads": each thread's CPU time over the window, by the name the thread gave itself ("Main",
//   "JQ Worker n (Processor m)", "Win (WinID n)", Apple's and the library's threads), tagged with what the library saw
//   it do: [present] the thread that presents (the render thread), [server] the one that runs SV_Frame, [client] the
//   one that runs the client's frame. Read from the process's own threads (task_threads and thread_info on
//   mach_task_self(): nothing attaches to anything), once a window, on the render thread.
// - "frame stats: phases": the time a frame spends in the server's frame (SV_Frame) and the client's frame (CL_Frame),
//   wall clock and the thread's own CPU time, timed in the co-op pause's hooks on those two functions
//   (aae_coop_pause.c), which only time anything while the detail is on.
// The formatting is plain C without the game or macOS (tests/test_thread_stats.c tests it as is).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// what the library saw a thread do, as bits
enum {
    Thread_Role_Present = 1, // presents frames (Aspyr's present, gpu_tweaks.c): the render thread
    Thread_Role_Server = 2,  // runs the server's frame (SV_Frame)
    Thread_Role_Client = 4,  // runs the client's frame (CL_Frame)
};

// one thread at one moment: its id (thread_identifier_info's, unique for the process's life), its CPU time so far (user
// and system, ns) and its name ("" when it has none)
typedef struct {
    uint64_t id;
    uint64_t cpu_ns;
    char name[64];
} thread_sample_t;

// the line's text after "frame stats: threads ": every thread's CPU time between two samples, biggest first, as % of a
// core over seconds and ms a frame, the first `shown` by name and the rest summed. A thread missing from before
// started in the window and counts with all its time; one missing from now ended in it and isn't counted (its time
// is lost). roles_of gives each id's role bits. Returns what snprintf returns.
int thread_stats_format(const thread_sample_t *before, int before_count, const thread_sample_t *now, int now_count,
                        const uint64_t *role_ids, int role_count, double seconds, int frames, int shown, char *out,
                        size_t size);

// the phases a frame is timed in
enum { Phase_Server, Phase_Client, Phase_Count };

// one phase's sums over a window: calls, wall clock and the calling thread's CPU time (ns)
typedef struct {
    uint64_t calls, wall_ns, cpu_ns;
} phase_sum_t;

// the line's text after "frame stats: phases ": ms a frame in each phase (wall clock, and CPU in brackets), calls a
// frame. Returns what snprintf returns.
int thread_stats_format_phases(const phase_sum_t sums[Phase_Count], int frames, char *out, size_t size);

// --- the part that runs in the game (thread_stats_game.c) ---

// turns the two lines on (FrameStats' detail); before this nothing here runs or reads a clock
void thread_stats_init(void);

// whether the phases are timed: the co-op pause's hooks ask this before anything else, so with the detail off they do
// exactly what they did
extern bool thread_stats_phases_on;

// the calling thread does what role says (bits above); cheap after the first time
void thread_stats_note_role(int role);

// a phase's timing: begin gives the start, end adds the time since to the phase (and notes the thread's role)
typedef struct {
    uint64_t wall_ns, cpu_ns;
} phase_start_t;
phase_start_t thread_stats_phase_begin(void);
void thread_stats_phase_end(int phase, phase_start_t start);

// from the render thread at the end of a FrameStats window: prints the two lines for the window just ended
void thread_stats_window(double seconds, int frames);
