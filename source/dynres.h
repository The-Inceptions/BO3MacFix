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
// steps up at 69-79% GPU busy, then down again). A step up needs windows in which the next size would keep the GPU
// under 80% busy, for 8 s in a row; under the target frame rate (CPU-bound) the busy share is first scaled to the
// target's frame time, so a CPU-bound match still comes back up once the GPU has room. The count starts again at every
// change, so after coming down it stays down for 8 s at least.
#define Dynres_Headroom 0.80    // a step up fits when the GPU would be busy less than 80% of the frame time
#define Dynres_Hold_Down 2      // windows after a change before it lowers again (the GPU time settles)
#define Dynres_Hold_Up 32       // windows in a row with headroom before it raises (8 s at 120 fps)
#define Dynres_Far_Behind 0.85  // below 85% of the target frame rate it takes two steps down at once
// Under a frame limit (the target below the display's rate) the GPU slows its clock to the pace it has, so its busy
// share says nothing about room: on Kino at 40 fps it read 70-77% at 60% resolution, 17-19 ms a frame, and the same
// within a few points from 95% down, so the way up above (under 80% after the step, 68% at 60%) never opened and a
// heavy moment left the resolution at the floor for good. There the way up is a trial instead: after the frame rate has
// held the limit for Dynres_Hold_Up_Seconds in a row, one step up; if a window behind with the GPU busy comes within
// Dynres_Trial_Windows after it (the holds after the change first, as for any step down), straight back to the size
// before, and the next trial waits twice as long (up to 256 times, Dynres_Max_Backoff). A trial whose size lasted
// Dynres_Trial_Watch windows clears that (below), and a trial's windows count toward the next one's hold. Shorter
// than the 8 s without a limit because a wrong trial is taken back: it costs a few windows a little under the limit,
// and the doubling makes it rare. 60% -> 100% at 40 fps: 8 steps, about a minute.
#define Dynres_Hold_Up_Seconds 6 // the trial's hold (8 windows at 40 fps)
#define Dynres_Trial_Windows 8   // windows after a trial in which falling behind takes it back (6 s at 40 fps)
// The hold at most doubles 8 times: 256 times the hold, 26 minutes at 40 fps. Where the next size can't hold the limit,
// each trial costs about 2 s a little under it; with 4 that came every 98 s for good, with 6 every 6.4 minutes, with 8
// four or five times in the first half hour and then about every 26 minutes. A steady picture matters more than the
// last 5% of resolution, and a trial that holds still clears the doubling, so the climb after a heavy moment in a
// lighter part of the map stays as quick.
#define Dynres_Max_Backoff 8
// A trial kept after its Dynres_Trial_Windows is still watched until Dynres_Trial_Watch windows after its step up: a
// step down for the GPU within them counts as a failed trial too, and only a trial's size that lasted them all clears
// the count. On battery the GPU's clock can drop a while after a step up (gc16_pwrgpu_climb40: the same still scene
// from 12 ms a frame at 49% busy to 23 ms at 91%, from one window to the next), so a kept trial fell behind a few
// seconds later, stepped down as a plain step, and the next trial came after the shortest hold: a dip every 12 s for
// as long as the scene sat on that edge. Counted, those thin out as where the next size never holds. A clean climb (a
// trial every 9 windows at 40 fps, none failing) is as quick as before.
#define Dynres_Trial_Watch 32 // windows (24 s at 40 fps)

typedef struct {
    int base;     // the user's ResolutionPercent, the most it goes up to
    int lowest;   // the least it goes down to
    int ceiling;  // the most it goes up to for now (BatteryResolution on battery), base otherwise
    double target;      // the frame rate the last window aimed at, 0 before the first (Dynres_Rise_Hold_Seconds)
    double rise_hold;   // seconds of play left in which no step down is taken, after the frame target rose
    int held_windows;   // windows behind with the GPU busy that the hold kept from stepping down since the rise
    bool rise_window;   // the last window was the one in which the target rose (the next one is held too; the log)
    bool after_rise_window; // the last window was held as the one after that window (for the log)
    int percent;  // the resolution now
    int windows_since_change;
    int headroom_windows; // windows in a row in which a step up would have fit (under a limit: held the limit)
    int trial_from;       // under a limit: the size before a step up still on trial, 0 for none
    int trial_windows;    // the trial's windows so far, counted in the match only (not in a pause or the background)
    bool trial_watched;   // the last trial step up is within its Dynres_Trial_Watch windows, no step down since
    int failed_trials;    // under a limit: trials that failed in a row (each doubles the next one's hold)
    int limit;            // the last window's kind of limit (dynres_limit_t)
    int event;            // what the last window did (Dynres_Event_*), for the log
} dynres_t;

// the kind of limit a window ran under: none (the display's rate), one of the match itself (BatteryFPS, LowPowerFPS,
// FramePacingFPS, the game's com_maxfps), or one of a situation outside the play (the pause menu's, the menus', the
// background's: frame_limit_for_frame's Limit_* bits)
typedef enum { Dynres_No_Limit = 0, Dynres_Match_Limit = 1, Dynres_Situation_Limit = 2 } dynres_limit_t;
enum { Dynres_Event_None, Dynres_Event_Trial, Dynres_Event_Taken_Back, Dynres_Event_Failed_Late, Dynres_Event_Ceiling,
       Dynres_Event_Held, Dynres_Event_Too_Slow };

// After the frame target rises, for whatever reason (a frame limit lifted or raised: the pause menu or a menu closed,
// the game in front again, plugged in, BatteryFPS or BatterySaver changed on the settings page, a display with a higher
// refresh rate), a window behind the new target with the GPU busy is no reason to step down while the GPU's clock can
// still be coming up: macOS clocks the GPU down to just fit a frame limit (Kino paused at 30 fps: the GPU 781 ms busy a
// second), so "behind the target and the GPU busy" is true by construction for a while after the rise. Seen in the
// game: both closes of the pause menu in one run on battery stepped 100% -> 95% in the window that spanned them
// (gcp1_perfbatt_pausekino: "37.9 fps of 40, GPU busy 98%"); a plug-in stepped 70% -> 60% four windows after it, the GPU
// at 97 fps and 99% busy where it later held 120 fps at 60% with 56% (gc26_perfbatt_battplug). So for
// Dynres_Rise_Hold_Seconds of play, counted from the window in which the target rose and including it, these windows
// take no step down and no trial take-back:
// - the window in which the target rose, whatever its frame rate: it was partly paced to the old target;
// - the window after it, whatever its frame rate too, and even when the window of the rise took the whole 3 s (30
//   frames at a limit of 10): it has the first frames after the rise when the rise came in the last frames of its
//   window. (The limiter paces a frame by the limit of the present before it, and a window's target is that of its
//   last present, so a limit lifted at a window's last present leaves all 30 of that window's frames at the old pace;
//   the frames drawn while the GPU is still clocked for the old limit are then the next window's. A scene that clock
//   fitted tightly starts under the share below though it holds the new target at full clock: the review of
//   2026-11-18, wip/night/review-share-hold.md, F1. Seen in the game: Kino after a lift and after a plug-in,
//   gc30_perfbatt_liftplain and battplug, 57% and 60% of the target in this window, then 120 fps at full clock; the rule
//   before took two steps down at once in both.)
// - a later one only while its frame rate is at least Dynres_Rise_Hold_Share of the new target: a clock coming up.
// A window further below it is a scene that can't keep up and is judged as without a rise. Every window logged after a
// rise says which is which: a clock coming up was at 65% of the target or more from the second window after the rise
// window on (gc27/gc28 liftplain, battplug: 86.7, 88.9, 90.0, 91.3, 92.3 ... of 120; gc30 liftplain and battplug: 78.3
// and 83.7, 65% and 70%; the journeys' unpauses on mains: 88.0, 96.7 of 120; pausebatt: 40 of 40), while DEIMOS at 100%,
// which can't hold 120, was flat at 55-58% (gc28_perfbatt_deimoslift: 66.1 to 69.9 fps of 120, from its second window
// on; gc30_lead_deimoslift2: 52% in that window). 65% lies between them, with no margin left on the clock's side:
// gc30 liftplain's window was 65.25%, and on that ramp (54 to 113 fps over 3 s) a lift later in its window puts it
// under 65% (a model of the ramp, wip/agents/rev-int14/ramp: for 22 of the 30 frames the lift can come at). What each
// side costs: a clock window under 65% is judged, two steps down at once where there were none; a heavy scene at 65% or
// more (DEIMOS at 85% gives 82-91 fps of 120: 68-76%) waits up to the 3 s for its first step, as it did with a plain
// 3 s hold. DEIMOS at 100% gets its first step in the second window after the rise window, about 1.5-1.7 s after the
// rise (gc30_lead_deimoslift2: 1.7 s; the 3 s hold: 3.7 s; no hold: 0.6 s).
// A window at the floor isn't held (nothing to step down to: none is said either). Everything else goes on as without
// a rise: steps up, the count toward the next step up or trial in windows with room (a held window starts it again, as
// a window behind with the GPU busy always did), a fall of the target, BatteryResolution's moves at once (a rise in the
// window of such a move is seen in the next one, which counts as the window of the rise).
#define Dynres_Rise_Hold_Seconds 3.0
#define Dynres_Rise_Hold_Share 0.65

// starts (again) at base, with lowest as the floor (both ResolutionPercent values, 50 to 200), and no ceiling below base
void dynres_reset(dynres_t *d, int base, int lowest);

// BatteryResolution: the 3D scene at most at a share of ResolutionPercent while the Mac runs on battery, to save the
// GPU's power. On battery at a frame limit the GPU has room, so the controller above keeps (or climbs back to) the
// user's resolution, and every frame costs the GPU what it costs there: on DEIMOS at 40 fps 11.4 W of GPU at 100%, on
// Kino 3.3 W at 100% and 2.1 W at 70% (wip/night/perf-battery2.md). The ceiling is the most the resolution may be
// for now, a ResolutionPercent value (base or more: none). Below the resolution now, the next window goes down to it at
// once, whatever the frame rate, and a trial step up in progress ends without counting as failed; the floor is then the
// ceiling where that's below lowest. Raised again (plugged in), the next window goes back up at once to lowest if it's
// below that (with DynamicResolution off, lowest is base: the user's own resolution), and from there in the usual
// steps. No step of any kind goes above it.
void dynres_set_ceiling(dynres_t *d, int ceiling);

// (Plugged in with BatteryFPS on, the battery's frame limit lifts in the same window the ceiling rises: the hold after
// a rise of the frame target, above, keeps that moment from stepping down. The ceiling rising alone changes no load.)

// the ceiling for BatteryResolution's setting (the share of base, in percent, 50 to 95; 0 = off): base while it's off or
// the Mac isn't on battery, else that share of base as dynres_lowest_for rounds it
int dynres_battery_ceiling(int base, int setting, bool on_battery);

// BatteryResolution's log line for a window, from the ceiling and the base as in dynres_t and *logged, the ceiling the
// log last named (0 for none yet), which it updates: on battery (a ceiling under the base) when that's new, plugged in
// ("up to the base again") only after an on battery line, and nothing when the ceiling didn't change. A game started
// plugged in names nothing: its first ceiling is the base, which is only remembered.
enum { Dynres_Ceiling_Nothing, Dynres_Ceiling_On_Battery, Dynres_Ceiling_Plugged_In };
int dynres_ceiling_news(int ceiling, int base, int *logged);

// the lowest ResolutionPercent for a base and DynamicResolution's setting (the lowest share of base, in percent):
// a multiple of Dynres_Step, at least 50 (the engine's minimum)
int dynres_lowest_for(int base, int setting);

// one window: its frame rate, the share of its time the GPU was busy (0 to 1), and the frame rate to reach. Returns the
// ResolutionPercent to render the next frames at.
int dynres_window(dynres_t *d, double fps, double gpu_busy, double target_fps);

// the same, saying what kind of limit the target is. Under a limit of the match below the display's rate the way up is
// the trial described at Dynres_Hold_Up_Seconds. Under a situation's limit (a pause, the menus, the background) there
// is no step up of any kind, so a match comes back at the size it left with; a trial in progress waits (its windows
// count in the match only) and is still taken back if a window there falls behind with the GPU busy. The count of
// windows with room starts again whenever the kind of limit changes (it means another thing in each); back to no limit,
// a trial in progress is kept as it is and the doubling forgotten. With Dynres_No_Limit it is dynres_window.
int dynres_window_limited(dynres_t *d, double fps, double gpu_busy, double target_fps, dynres_limit_t limit);

// BO3MACFIX_DYNRESSTARTLOW (a test switch, off by default): puts the resolution at the floor now, as a change, so that
// a game check can watch the way back up at a limit without a heavy moment to bring it down. Returns the new value.
int dynres_start_at_floor(dynres_t *d);

// The frame rate to reach (dynres_window's target): the display's refresh rate, our frame limit for the frame as the
// display can show it (frame_limit_shown: BatteryFPS 45 on the 120 Hz panel is 40 fps, never 45), and the game's own
// com_maxfps, whichever is lowest; 0 for none known. Aiming at the limit itself took the resolution down to the floor
// on a light map at a steady 40.0 fps ("40.0 fps of 45, GPU busy 92%", 100 -> 60 in 5% steps): a frame rate the panel
// never shows. com_maxfps isn't rounded: the engine's own limiter doesn't hold frames on screen, it only waits.
int dynres_target(int display_fps, int frame_limit_fps, int max_fps);

// Keeping the render targets of DynamicResolution's sizes. The engine's cache of render target sets (offsets.h) frees
// sets 1-7 once they haven't been used for 11 frames (set 0, ResolutionPercent's own, stays), so every step to a size
// other than the user's allocated its set again: one frame of 25-67 ms each time, 2-4 frames after the step. The glue
// learns the size of each set that appears right after a step and, every frame, sets those sets' last use, so a step
// back to a size it used before finds its set in the cache.
//
// The last use it sets is Dynres_Keep_Age frames old, not the newest. When all 8 sets are taken and a view needs
// another size, the engine's lookup frees the least recently used of sets 1-7 on the spot, without waiting for the
// render thread (wip/night/rt-rebuild.md), and the render thread draws the frame before with the sets of that frame and
// of the 3 before it (the view's history of its sets): their last uses are at most 4 frames old when the lookup runs. A
// kept set marked with the newest frame would never be the least recently used, so the engine's choice would fall on a
// set the frame being drawn uses. Marked 6 frames old, a kept set nobody draws with goes first, and a set a view really
// used keeps the engine's own, younger mark (the glue only ever makes a last use younger).
#define Dynres_Sets 8      // the engine's sets; 0 is never freed
#define Dynres_Keep_Max 3  // sizes kept (the steps from 95% to 85%), the oldest goes when a 4th comes
#define Dynres_Keep_Age 6  // frames: the age a kept set is held at, between the render thread's 4 and the engine's 11
#define Dynres_Keep_Fresh 9 // a kept set is held while its last use is at most 9 frames old (the engine frees at 11):
                            // one older the engine is letting go of, and is left alone

typedef struct {
    uint32_t size[Dynres_Keep_Max]; // width << 16 | height, most recent first; 0 for none
} dynres_keep_t;

static inline uint32_t dynres_size(uint16_t width, uint16_t height) {
    return (uint32_t)width << 16 | height;
}

// a set of this size appeared after a step: keep it (0, or one already kept, only moves it first)
void dynres_keep_add(dynres_keep_t *keep, uint32_t size);

// the sets (bit i for set i, 1-7) whose last use to set to frame - hold_age (Dynres_Keep_Age; 0 only for the test
// switch BO3MACFIX_DYNRESKEEPNEWEST, which holds them at the newest frame as before): in use (size not 0), of a kept
// size, and last used more than hold_age and at most Dynres_Keep_Fresh frames before frame. A younger one is in use
// by a view (or already held), whose own mark stays.
unsigned dynres_keep_mask(const dynres_keep_t *keep, const uint32_t sizes[Dynres_Sets],
                          const uint64_t last_use[Dynres_Sets], uint64_t frame, int hold_age);

// The engine's own rules for its sets, which the glue's log and the tests follow (offsets.h, wip/night/rt-rebuild.md).
#define Dynres_Engine_Frees 11 // the lookup frees sets 1-7 whose last use is this many frames old or more
#define Dynres_In_Flight_Age 4 // when the main thread's lookup runs, the render thread draws the frame before, whose
                               // passes read the sets of that frame and of the 3 before it: last used 1 to 4 frames
                               // before. A set freed at that age is freed under the render thread.

// the set the engine's lookup frees for a size it doesn't have when all 8 sets are taken: the least recently used of
// sets 1-7, the lower one on a tie (0x100e8d938), whatever its age. -1 while a set is free (size 0).
int dynres_eviction_victim(const uint32_t sizes[Dynres_Sets], const uint64_t last_use[Dynres_Sets]);

// What the engine did to each of sets 1-7 between two looks at its table (the glue looks once a frame): from the
// sizes and last uses then and the sizes and frame counter now. A set unused for Dynres_Engine_Frees frames can be
// freed by the lookup's sweep (and made again for another size). A set in use goes in a rebuild of every render target
// (the video restart's or the renderer's, which wait for the render thread first and free every set, so a set in use
// is left free, or set 0 changes size); or the lookup takes it for another size because all 8 were taken: freed and
// made again on the spot, while the render thread may still draw with it. out[0] is about set 0 (same or rebuilt).
// Returns how many sets were taken that way.
typedef enum {
    Dynres_Set_Same,
    Dynres_Set_Made,    // was free, now in use
    Dynres_Set_Freed,   // by the sweep (now free, or made again for another size)
    Dynres_Set_Rebuilt, // by a rebuild of every render target
    Dynres_Set_Taken,   // by the lookup's eviction
} dynres_set_change_t;
int dynres_table_changes(const uint32_t sizes_before[Dynres_Sets], const uint64_t last_use_before[Dynres_Sets],
                         const uint32_t sizes_now[Dynres_Sets], uint64_t frame_now,
                         dynres_set_change_t out[Dynres_Sets]);

// forget every kept size, so dynres_keep_mask gives 0 and the engine frees those sets itself 11 frames after their last
// use (the one in use stays). Returns whether any size was kept.
bool dynres_keep_release(dynres_keep_t *keep);

// The kept sets cost memory: about 0.9 GB for the 95/90/85% sets of a 2624x1696 scene (wip/night/dynres-mem.md). So
// while macOS reports memory pressure (warn or critical), nothing is kept and no warm-up starts; once it's back to
// normal, the steps learn the sizes again. events: the DISPATCH_MEMORYPRESSURE_* bits of a memory pressure dispatch
// source's event. Returns whether there's pressure from then on (before: whether there was).
bool dynres_pressure_after(unsigned long events, bool before);

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

// the same with BatteryResolution's ceiling (dynres_battery_ceiling): below base (on battery) it visits the ceiling and
// the steps under it, down to the floor under it, instead of the steps under base, so that the controller's first step
// down to the ceiling finds that size's set kept; it still goes back to base at the end. At base or above it's
// dynres_prewarm_frame.
int dynres_prewarm_frame_up_to(dynres_prewarm_t *p, int base, int lowest, int ceiling, bool ready, bool new_set);

// ends a warm-up in progress without going back anywhere (the caller restores the user's value itself): for the
// feature turned off in the middle of one, which otherwise resumed later with the sizes and the base of its start
// (and could undo a Render Resolution change made in between). Doesn't arm it again; a map load does.
void dynres_prewarm_cancel(dynres_prewarm_t *p);

// the set (1-7) of the size the engine renders at percent, from set 0's size at base (each side scaled and rounded up to
// 8 by the engine, so within 8 pixels), or -1 for none. Finds a set the warm-up visits that was there before it.
int dynres_set_for_percent(const uint32_t sizes[Dynres_Sets], int base, int percent);

static inline bool dynres_prewarm_running(const dynres_prewarm_t *p) {
    return p->index >= 0;
}
