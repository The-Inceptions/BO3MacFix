#pragma once
#include <stdint.h>

// GpuDiagnostics: 1 logs the window, display mode and Metal layer whenever they change (BO3MACFIX_GPUDIAGNOSTICS)
void gpu_diag_init(int enabled);
// FrameStats' detail: times every command buffer the game commits from the first present on (the class of
// command_buffer's commit is swizzled once), and gives the time the GPU was busy since the last call, and how many
void gpu_diag_watch_command_buffers(void *command_buffer);
double gpu_diag_take_busy(uint64_t *buffers);
