#pragma once
#include <stdint.h>

// GpuDiagnostics: 1 logs the window, display mode and Metal layer whenever they change (BO3MACFIX_GPUDIAGNOSTICS)
void gpu_diag_init(int enabled);
// FrameStats' detail: times every command buffer the game commits from the first present on (the class of
// command_buffer's commit is swizzled once), and gives the time the GPU was busy since the last call, and how many
void gpu_diag_watch_command_buffers(void *command_buffer);
double gpu_diag_take_busy(uint64_t *buffers);
// the GPU's busy time in seconds since the watch started, never reset (for DynamicResolution, which takes differences)
double gpu_diag_busy_total(void);
// DynamicResolution's step trace: records when each of the next buffers (at most Gpu_Diag_Trace_Max) to complete was
// committed and when the GPU started and finished it (seconds, CLOCK_UPTIME_RAW); take returns how many and stops
enum { Gpu_Diag_Trace_Max = 64 };
void gpu_diag_trace_begin(int buffers);
int gpu_diag_trace_take(double times[][3], int max);
