// DynamicResolution's controller as plain C: from each window of frames' frame rate and GPU time, the render
// resolution (ResolutionPercent) to use next. No game or macOS calls, so tests/test_dynres.c tests it as is; the glue
// that measures the frames and sets the resolution is in gpu_tweaks.c.
//
// It lowers the resolution in small steps while the game misses its target frame rate (the display's refresh rate or
// a frame limit) and the GPU is busy nearly all the time, and raises it again once the next step up looks like it
// fits in the frame time with room to spare. Each change makes the engine allocate render targets at the new size
// (and free the old ones 11 frames later), so it changes the resolution at most every few windows.
#pragma once
#include <stdbool.h>

#define Dynres_Window_Frames 30 // frames per window (0.25 s at 120 fps): one decision per window
#define Dynres_Step 5           // percentage points per step
#define Dynres_Missed 0.95      // a window misses the target below 95% of its frame rate (114 fps at 120: two late
                                // frames in 30, one is often a hitch that a lower resolution wouldn't prevent)
#define Dynres_GPU_Bound 0.90   // and is GPU-bound when the GPU was busy 90% of its time or more
#define Dynres_Headroom 0.88    // a step up fits when the GPU would be busy less than 88% of the frame time
#define Dynres_Hold_Down 2      // windows after a change before it lowers again (the GPU time settles)
#define Dynres_Hold_Up 8        // windows in a row with headroom before it raises (2 s at 120 fps)
#define Dynres_Far_Behind 0.85  // below 85% of the target frame rate it takes two steps down at once

typedef struct {
    int base;     // the user's ResolutionPercent, the most it goes up to
    int lowest;   // the least it goes down to
    int percent;  // the resolution now
    int windows_since_change;
    int headroom_windows; // windows in a row in which a step up would have fit
} dynres_t;

// starts (again) at base, with lowest as the floor (both ResolutionPercent values, 50 to 200)
void dynres_reset(dynres_t *d, int base, int lowest);

// the lowest ResolutionPercent for a base and DynamicResolution's setting (the lowest share of base, in percent):
// a multiple of Dynres_Step, at least 50 (the engine's minimum)
int dynres_lowest_for(int base, int setting);

// one window: its frame rate, the share of its time the GPU was busy (0 to 1), and the frame rate to reach. Returns the
// ResolutionPercent to render the next frames at.
int dynres_window(dynres_t *d, double fps, double gpu_busy, double target_fps);
