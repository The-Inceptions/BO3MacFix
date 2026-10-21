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
#include <stdint.h>

#define Dynres_Window_Frames 30 // frames per window (0.25 s at 120 fps): one decision per window
#define Dynres_Step 5           // percentage points per step
#define Dynres_Missed 0.95      // a window misses the target below 95% of its frame rate (114 fps at 120: two late
                                // frames in 30, one is often a hitch that a lower resolution wouldn't prevent)
#define Dynres_GPU_Bound 0.90   // and is GPU-bound when the GPU was busy 90% of its time or more
// Raising is slow on purpose: every step costs one frame of extra GPU work (dynres-step: +0 to +20 ms, unavoidable),
// and in play the load swings every few seconds, so a quick step up was mostly undone a second later (the user's log:
// steps up at 69-79% GPU busy, then down again). A step up needs a window that held the frame rate (not a CPU-bound
// one, where the GPU's room says nothing) and in which the next size would keep the GPU under 80% busy, for 8 s in a
// row. The count starts again at every change, so after coming down it stays down for 8 s at least.
#define Dynres_Headroom 0.80    // a step up fits when the GPU would be busy less than 80% of the frame time
#define Dynres_Hold_Down 2      // windows after a change before it lowers again (the GPU time settles)
#define Dynres_Hold_Up 32       // windows in a row with headroom before it raises (8 s at 120 fps)
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

// Keeping the render targets of DynamicResolution's sizes. The engine's cache of render target sets (offsets.h) frees
// sets 1-7 once they haven't been used for 11 frames (set 0, ResolutionPercent's own, stays), so every step to a size
// other than the user's allocated its set again: one frame of 25-67 ms each time, 2-4 frames after the step. The glue
// learns the size of each set that appears right after a step and, every frame, sets those sets' last use to the
// engine's frame counter, so a step back to a size it used before finds its set in the cache.
#define Dynres_Sets 8      // the engine's sets; 0 is never freed
#define Dynres_Keep_Max 3  // sizes kept (the steps from 95% to 85%), the oldest goes when a 4th comes
#define Dynres_Keep_Fresh 5 // only a set used in the last 5 frames is kept (the engine frees at 11): one the engine
                            // already let go is left alone

typedef struct {
    uint32_t size[Dynres_Keep_Max]; // width << 16 | height, most recent first; 0 for none
} dynres_keep_t;

static inline uint32_t dynres_size(uint16_t width, uint16_t height) {
    return (uint32_t)width << 16 | height;
}

// a set of this size appeared after a step: keep it (0, or one already kept, only moves it first)
void dynres_keep_add(dynres_keep_t *keep, uint32_t size);

// the sets (bit i for set i, 1-7) whose last use to set to the frame counter: in use (size not 0), of a kept size,
// and used within the last Dynres_Keep_Fresh frames
unsigned dynres_keep_mask(const dynres_keep_t *keep, const uint32_t sizes[Dynres_Sets],
                          const uint64_t last_use[Dynres_Sets], uint64_t frame);

// Warming up at a match start. The kept sets go when a map loads, so the first step to each size in a match still
// allocated its set: one frame of 25-58 ms each, in the middle of the play. The match's first frames, while the screen
// is still black and the players can't move, render one frame at each size the controller can reach instead, then go
// back to the user's own. Each frame there allocates its set, where it can't be felt, and dynres_keep_tick keeps them.
#define Dynres_Prewarm_Wait 8 // frames to wait at a size for its set to appear before going on without it

typedef struct {
    int percent[Dynres_Keep_Max]; // the sizes to visit, nearest the user's first
    int count;                    // how many
    int index;                    // the one being visited, -1 while not warming up
    int frames;                   // frames at it so far
    int base;                     // the value to go back to
    bool armed;                   // a map loaded and its match hasn't been warmed up yet
} dynres_prewarm_t;

// the ResolutionPercent values the controller can step to from base, nearest first, down to lowest: at most
// Dynres_Keep_Max, as many as stay kept. Returns how many.
int dynres_prewarm_sizes(int base, int lowest, int out[Dynres_Keep_Max]);

// not warming up, and armed: a map is about to load (the game's start counts as one)
void dynres_prewarm_init(dynres_prewarm_t *p);

// each frame: ready when a match runs and the engine rendered its 3D view this frame, new_set when the set of the size
// it's at is there (it appeared since the last write, or was there before). Starts when armed and ready, then goes on to the next size once the set of the size
// it's at appeared (or after Dynres_Prewarm_Wait frames), and back to base after the last. Returns the
// ResolutionPercent to write now, or 0 for none.
int dynres_prewarm_frame(dynres_prewarm_t *p, int base, int lowest, bool ready, bool new_set);

// the set (1-7) of the size the engine renders at percent, from set 0's size at base (each side scaled and rounded up to
// 8 by the engine, so within 8 pixels), or -1 for none. Finds a set the warm-up visits that was there before it.
int dynres_set_for_percent(const uint32_t sizes[Dynres_Sets], int base, int percent);

static inline bool dynres_prewarm_running(const dynres_prewarm_t *p) {
    return p->index >= 0;
}
