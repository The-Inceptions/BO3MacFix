// Frame queue, pacing and upscaling tweaks for Aspyr's Metal presenter (its D3D Present is at 0x1000e8b78).
// - FramesInFlight: Aspyr creates its frames-in-flight semaphore with a fixed count of 2, and its
//   IDXGIDevice1::SetMaximumFrameLatency ignores the value, so MaxFrameLatency in config.ini does nothing.
// - FramePacingFPS: presents with presentDrawable:afterMinimumDuration: instead of presentDrawable:, so
//   each frame stays on screen for at least 1/fps seconds and frames come out evenly on ProMotion displays.
// - MetalFXUpscale: when the game runs below the screen's resolution, upscales each frame to the screen's
//   native size with MetalFX instead of letting Core Animation stretch it, see metalfx.m. 2 upscales to the
//   panel's resolution instead when the display is in a scaled mode.
// - FrameStats: logs the frame rate, 1% low and worst frame time every few seconds, for benchmarking.
// - BatteryFPS: paces frames to that rate only while the Mac runs on battery, to save power.
// - LowPowerFPS: the same while macOS's Low Power Mode is on, the user's own request to save power.
// - PauseMenuFPS: paces frames to that rate while the pause menu is open in a match (notices.lua says when), to save
//   power.
// - MenuFPS: the same in the menus outside a match (the main menu, the lobby, the mods menu; notices.lua says when).
// - BackgroundFPS: the same while the game isn't the active app, or is hidden or minimized (app_focus.m says when).
//   The lowest of these limits that applies wins, see frame_limit.c.
// - DynamicResolution: lowers the 3D scene's resolution (ResolutionPercent) in small steps while the GPU can't keep
//   up with the display's refresh rate, and raises it again when it can, see dynres.h.
// - MetalHUD: turns on macOS's Metal performance HUD.
#include <math.h>
#include <os/lock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <notify.h>
#include <sys/sysctl.h>
#include <objc/runtime.h>
#include <objc/message.h>

#include "app_focus.h"
#include "dynres.h"
#include "battery_saver.h"
#include "display_sleep.h"
#include "display_sleep_mac.h"
#include "frame_limit.h"
#include "gc_bridge.h"
#include "frame_pace.h"
#include "gpu_tweaks.h"
#include "metalfx.h"
#include "prepatch.h"
#include "offsets.h"
#include "script_errors.h"
#include "lua_heap.h"
#include "pmem_spill.h"
#include "script_vars.h"
#include "gpu_diag.h"
#include "thread_stats.h"

extern uint64_t game_base_address;

static frame_limits_t limits; // the frame rate limits (frame_limit.h) and the situations some apply in
static SEL sel_present_after_minimum_duration;
static int metalfx_active = 0;
static int frame_stats_seconds = 0; // 0 when not logging frame stats
static int frame_stats_detail = 0; // BO3MACFIX_FRAMESTATSDETAIL=1: also the script errors thrown in each window
static atomic_uint frame_stats_session = 0; // goes up each time FrameStats is turned on from the menu
// BO3MACFIX_FRAMEPACESTATS=1: also a "frame pacing:" line, how many refreshes each frame stayed on screen (frame_pace.h)
static int frame_pace_stats = 0;

static int compare_floats(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

// the "frame pacing:" line for the frames shown since the last one (BO3MACFIX_FRAMEPACESTATS=1)
static void frame_pace_log(int display_fps) {
    static double shown[8192], work[8192];
    int count = gpu_diag_take_presented(shown, (int)(sizeof(shown) / sizeof(shown[0])));
    if (display_fps <= 0)
        return;
    frame_pace_t pace;
    frame_pace_summarize(shown, count, 1.0 / display_fps, work, &pace);
    if (pace.frames == 0) {
        printf("frame pacing: no frame shown (%i presented)\n", count);
        return;
    }
    double share = 100.0 / pace.frames;
    printf("frame pacing: shown %.1f fps, 1%% low %.1f fps, worst %.1f ms; on screen 1 refresh %.0f%%, 2 %.0f%%, 3 %.0f%%, "
           "4 or more %.0f%% (of %i Hz); %.0f%% changed from the frame before, %.0f%% off the refresh grid (%i frames)\n",
           pace.fps, pace.low_1_fps, pace.worst_ms, pace.refreshes[0] * share, pace.refreshes[1] * share,
           pace.refreshes[2] * share, pace.refreshes[3] * share, display_fps, pace.changes * share,
           pace.off_grid * share, pace.frames);
}

// times each present, and every frame_stats_seconds logs the average frame rate, the 1% low (the frame rate of the
// slowest 1% of frames) and the worst frame time
static void frame_stats_tick(void) {
    static mach_timebase_info_data_t timebase;
    static uint64_t last_present = 0, window_start = 0;
    static float frame_ms[8192];
    static int frames = 0, stored = 0;
    static unsigned int session = 0;
    if (timebase.denom == 0)
        mach_timebase_info(&timebase);
    uint64_t now = mach_absolute_time();
    // turned off and on again from the menu: the time since the last present before that isn't a frame
    unsigned int current = atomic_load(&frame_stats_session);
    if (current != session) {
        session = current;
        last_present = window_start = 0;
        frames = stored = 0;
        if (frame_pace_stats)
            gpu_diag_forget_presented();
    }
    if (last_present != 0) {
        if (stored < (int)(sizeof(frame_ms) / sizeof(frame_ms[0])))
            frame_ms[stored++] = (float)((double)(now - last_present) * timebase.numer / timebase.denom / 1e6);
        frames++;
    }
    last_present = now;
    if (window_start == 0)
        window_start = now;
    double seconds = (double)(now - window_start) * timebase.numer / timebase.denom / 1e9;
    if (seconds < frame_stats_seconds || frames == 0)
        return;
    qsort(frame_ms, stored, sizeof(frame_ms[0]), compare_floats);
    float low_1 = frame_ms[(int)(stored * 0.99)];
    char display[24] = "";
    int display_fps = present_display_max_fps();
    if (display_fps > 0)
        snprintf(display, sizeof(display), ", display %i Hz", display_fps);
    char detail[400] = "";
    if (frame_stats_detail) {
        uint64_t buffers;
        double busy = gpu_diag_take_busy(&buffers);
        int n = snprintf(detail, sizeof(detail), ", GPU busy %.2f ms a frame (%.0f%%), %.1f command buffers a frame",
                         busy * 1000.0 / frames, busy * 100.0 / seconds, (double)buffers / frames);
        uint32_t server_errors, client_errors;
        if (script_errors_take_thrown(&server_errors, &client_errors) && n > 0 && n < (int)sizeof(detail))
            n += snprintf(detail + n, sizeof(detail) - n, ", script errors %u server, %u client", server_errors,
                          client_errors);
        if (n > 0 && n < (int)sizeof(detail)) {
            uint32_t server_limit, client_limit;
            uint32_t server_used = script_variables_used(0, &server_limit);
            uint32_t client_used = script_variables_used(1, &client_limit);
            size_t lua_capacity, lua_used = lua_heap_used(&lua_capacity);
            size_t files_more, files_used = pmem_spill_used(&files_more);
            snprintf(detail + n, sizeof(detail) - n, ", script variables %u of %u server, %u of %u client, UI Lua "
                     "%.1f of %zu MB, loaded files %zu of 2048 MB and %zu MB more", server_used, server_limit,
                     client_used, client_limit, lua_used / 1048576.0, lua_capacity >> 20, files_used >> 20,
                     files_more >> 20);
        }
    }
    printf("frame stats: %.1f fps, 1%% low %.1f fps, worst frame %.1f ms (%i frames in %.1f s%s)%s\n",
           frames / seconds, 1000.0f / low_1, frame_ms[stored - 1], frames, seconds, display, detail);
    if (frame_pace_stats)
        frame_pace_log(display_fps);
    if (frame_stats_detail)
        thread_stats_window(seconds, frames);
    frames = stored = 0;
    window_start = now;
}

// The frame rate limits keep each frame on screen for at least 1/fps seconds, which a display
// with a fixed refresh rate rounds up to whole refreshes: 120 fps on a 144 Hz display shows each frame for 2
// refreshes, so 72 fps. Says so when the game's display or the rate changes to one like that.
static void check_pacing(double min_duration) {
    static int last_fps = 0, last_display_fps = 0;
    int fps = (int)lround(1.0 / min_duration), display_fps = present_display_max_fps();
    if (display_fps <= 0 || (fps == last_fps && display_fps == last_display_fps))
        return;
    last_fps = fps;
    last_display_fps = display_fps;
    int refreshes = frame_limit_refreshes(fps, display_fps);
    if (refreshes <= 1 || display_fps % fps == 0 || present_display_variable_refresh())
        return;
    printf("pacing at %i fps comes out at %i fps on this %i Hz display, %i or %i fps would be even\n", fps,
           display_fps / refreshes, display_fps, display_fps / refreshes, display_fps / (refreshes - 1));
}

// presentDrawable:afterMinimumDuration: alone doesn't always hold the game back: on the built-in ProMotion panel with
// macOS 27, a plain Metal loop presenting with a minimum duration of 1/30 s still ran at 120 fps, and Kino with
// BatteryFPS 60 at 70-110 fps. So the render thread also waits here for the frame's turn, which is what saves power on
// battery. The turns are counted from the first paced frame (start + n / fps), so a late wake-up doesn't push back the
// frames after it, and the rate is exactly fps on average. A frame that's a whole turn late starts the count again.
static void wait_for_turn(double min_duration) {
    static mach_timebase_info_data_t timebase;
    static uint64_t next = 0;
    static double counted_duration = 0;
    if (timebase.denom == 0)
        mach_timebase_info(&timebase);
    uint64_t interval = (uint64_t)(min_duration * 1e9 * timebase.denom / timebase.numer);
    uint64_t now = mach_absolute_time();
    if (min_duration != counted_duration || next == 0 || now >= next + interval) {
        counted_duration = min_duration;
        next = now + interval;
        return;
    }
    if (now < next)
        mach_wait_until(next);
    next += interval;
}

// the situational limits that started or stopped applying with this frame, and the rate it's paced at now
static void log_limits(int started, int stopped, int fps) {
    static const struct {
        int bit;
        const char *start, *stop;
    } names[] = {
        { Limit_Pause_Menu, "pause menu open in a match", "pause menu closed" },
        { Limit_Menus, "in the menus", "left the menus" },
        { Limit_Background, "the game is in the background", "the game is in front again" },
        { Limit_Idle, "nothing pressed in the menu for 10 s", "input in the menu again" },
    };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        if (started & names[i].bit)
            printf("BO3MacFix: %s, limiting to %i fps\n", names[i].start, fps);
        if ((stopped & names[i].bit) && fps > 0)
            printf("BO3MacFix: %s, limiting to %i fps\n", names[i].stop, fps);
        else if (stopped & names[i].bit)
            printf("BO3MacFix: %s, frame limit off\n", names[i].stop);
    }
}

#pragma mark - DynamicResolution

static int redirect_present(code_patch_fn checked_code_patch);
static bool present_live(void);

// DynamicResolution: 0 off, else the lowest share of ResolutionPercent it goes down to, in percent
static atomic_int dynres_setting = 0;
// BatteryResolution: 0 off, else the most the resolution may be on battery, as a share of ResolutionPercent in percent
static atomic_int battery_resolution = 0;
static int battery_ceiling_logged = 0; // the render thread's: the ceiling the log last named, 0 for none yet
static dynres_t dynres; // the render thread's
static char dynres_too_slow[80]; // the render thread's: the "why" of a step a window in the rise hold took (dynres_tick)
// BO3MACFIX_DYNRESSTARTLOW=1, a test switch (off by default): the controller's first decision after each start (a
// match's warm-up, a new base) puts the resolution at the floor, so a game check can watch it climb back at a frame
// limit
static int dynres_start_low = -1;
static bool dynres_start_low_pending = false;
// what DynamicResolution last put into r_sceneResolutionMultiplier, 0 while the dvar holds the user's own value;
// and the user's own value then, for config.ini (gpu_tweaks_dynres_held_base)
static atomic_int dynres_written = 0, dynres_base = 0;
static atomic_bool dynres_reconfigure = false; // the setting changed in the menu: start again from the user's value
// BO3MACFIX_DYNRESTEST="100,85,70" cycles through these every BO3MACFIX_DYNRESTESTSECONDS (8) instead, for testing;
// with BO3MACFIX_DYNRESTESTFRAMES=n every n frames (dynres_test_fast_tick)
static int dynres_test[8], dynres_test_count = 0, dynres_test_seconds = 8, dynres_test_frames = 0;

static char *scene_multiplier_dvar(void) {
    return *(char *volatile *)(game_base_address + ADDR_dvar_r_sceneResolutionMultiplier);
}

// sets ResolutionPercent's value and its latched value alike. With nothing latched R_CheckVidRestart has nothing to
// apply and leaves the render targets alone; the next frame's view setup reads the value and takes render targets
// of that size from the engine's cache (see offsets.h). The latched value goes first: if R_CheckVidRestart looks
// between the two writes, it applies the new value itself, with one full rebuild of the render targets.
static void scene_multiplier_set(char *dvar, int percent) {
    *(volatile int *)(dvar + dvar_Latched) = percent;
    *(volatile int *)(dvar + dvar_Value) = percent;
}

// the sets of DynamicResolution's sizes, kept in the engine's render target cache (dynres.h)
static dynres_keep_t dynres_keep;
// warming those sizes up in a match's first frames (dynres.h); armed again by each map load (gpu_tweaks_ui_gone)
static dynres_prewarm_t dynres_prewarm = {.index = -1, .armed = true};
static atomic_bool dynres_prewarm_arm = false;
static int dynres_learn_frames = 0;           // frames after a step in which a new set is one of ours
static uint32_t dynres_learn_from[Dynres_Sets]; // the sets' sizes before
// while macOS reports memory pressure, nothing is kept and no warm-up starts (dynres.h)
static atomic_bool dynres_pressure = false;

static void dynres_pressure_event(unsigned long events) {
    atomic_store(&dynres_pressure, dynres_pressure_after(events, atomic_load(&dynres_pressure)));
}

// macOS's memory pressure events, from the first frame DynamicResolution runs. With BO3MACFIX_TESTHOOKS=1, also
// com.bo3macfix.test.mempressure: `notifyutil -s com.bo3macfix.test.mempressure 2` (warn; 1 normal) then `notifyutil -p`
// it, as if macOS sent that event. BO3MACFIX_DYNRESPRESSURE=1 (a test switch) acts as if macOS reported memory
// pressure for the whole run: nothing is kept, so every step makes its set and the engine frees the one before.
static void dynres_pressure_watch(void) {
    const char *forced = getenv("BO3MACFIX_DYNRESPRESSURE");
    if (forced != NULL && atoi(forced) != 0) {
        printf("dynamic resolution: BO3MACFIX_DYNRESPRESSURE, as if macOS reported memory pressure all the time\n");
        dynres_pressure_event(DISPATCH_MEMORYPRESSURE_WARN);
        return;
    }
    // the level now (the source only tells changes): 1 normal, 2 warn, 4 critical, the DISPATCH_MEMORYPRESSURE_* bits
    int level = 0;
    size_t length = sizeof(level);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &length, NULL, 0) == 0)
        dynres_pressure_event((unsigned long)level);
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    static dispatch_source_t source;
    source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                    DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
                                        DISPATCH_MEMORYPRESSURE_CRITICAL, queue);
    if (source != NULL) {
        dispatch_source_set_event_handler(source, ^{
            dynres_pressure_event(dispatch_source_get_data(source));
        });
        dispatch_resume(source);
    }
    const char *env = getenv("BO3MACFIX_TESTHOOKS");
    int token;
    if (env != NULL && atoi(env) != 0)
        notify_register_dispatch("com.bo3macfix.test.mempressure", &token, queue, ^(int t) {
            uint64_t state = 0;
            notify_get_state(t, &state);
            dynres_pressure_event((unsigned long)state);
        });
}

// the engine's 8 render target sets: their sizes and last uses; false before the renderer made its table
static bool rt_sets_read(uint32_t sizes[Dynres_Sets], uint64_t last_use[Dynres_Sets]) {
    char *table = *(char *volatile *)(game_base_address + ADDR_rt_table);
    if (table == NULL)
        return false;
    volatile uint64_t *use = (volatile uint64_t *)(game_base_address + ADDR_rt_set_last_use);
    for (int i = 0; i < Dynres_Sets; i++) {
        volatile uint16_t *size = (volatile uint16_t *)(table + rt_set_size + i * rt_set_stride);
        sizes[i] = dynres_size(size[0], size[1]);
        last_use[i] = use[i];
    }
    return true;
}

// BO3MACFIX_DYNRESKEEPNEWEST=1, a test switch (off by default): hold the kept sets at the newest frame, as before
// (dynres.h, Dynres_Keep_Age), to compare
static int dynres_keep_newest = -1;

// what the engine did to its sets since the last frame (dynres_table_changes): a set it took while all 8 were in
// use is always said (rare, and the frame being drawn may still read it); with BO3MACFIX_DYNRESDEBUG=1 also each set
// made, freed by the sweep or rebuilt
static void dynres_log_changes(const uint32_t sizes[Dynres_Sets], uint64_t frame) {
    static bool seen = false;
    static uint32_t seen_sizes[Dynres_Sets];
    static uint64_t seen_use[Dynres_Sets], seen_frame;
    static int taken = 0, taken_young = 0, debug_lines = 0, debug = -1;
    if (debug < 0)
        debug = getenv("BO3MACFIX_DYNRESDEBUG") != NULL;
    uint64_t *use = (uint64_t *)(game_base_address + ADDR_rt_set_last_use);
    if (seen) {
        dynres_set_change_t change[Dynres_Sets];
        dynres_table_changes(seen_sizes, seen_use, sizes, frame, change);
        for (int i = 0; i < Dynres_Sets; i++) {
            int w0 = (int)(seen_sizes[i] >> 16), h0 = (int)(seen_sizes[i] & 0xffff);
            int w = (int)(sizes[i] >> 16), h = (int)(sizes[i] & 0xffff);
            // its age at the last look; the engine took it a frame or so later. A view using it in between would
            // have made it the newest set, which the engine takes only when every set was used in that frame.
            long long age = seen_use[i] <= seen_frame ? (long long)(seen_frame - seen_use[i]) : 0;
            long long now_age = (long long)(frame - seen_use[i]);
            if (change[i] == Dynres_Set_Taken) {
                bool young = age <= Dynres_In_Flight_Age;
                taken++;
                taken_young += young;
                if (taken <= 3 || (taken & (taken - 1)) == 0)
                    printf("dynamic resolution: the engine took set %i (%ix%i, last used %lld frames before) for "
                           "%ix%i, all 8 sets being in use%s (%i times, %i of them at most %i frames old)\n", i, w0,
                           h0, age, w, h, young ? "; the frame being drawn may still read it" : "", taken,
                           taken_young, Dynres_In_Flight_Age);
            } else if (debug && change[i] != Dynres_Set_Same && (++debug_lines <= 40 || debug_lines % 64 == 0)) {
                if (change[i] == Dynres_Set_Made)
                    printf("dynamic resolution: the engine made set %i (%ix%i) (%i)\n", i, w, h, debug_lines);
                else if (change[i] == Dynres_Set_Freed && sizes[i] == 0)
                    printf("dynamic resolution: the engine freed set %i (%ix%i) %lld frames after its last use "
                           "(%i)\n", i, w0, h0, now_age, debug_lines);
                else if (change[i] == Dynres_Set_Freed)
                    printf("dynamic resolution: the engine freed set %i (%ix%i) %lld frames after its last use and "
                           "made it %ix%i (%i)\n", i, w0, h0, now_age, w, h, debug_lines);
                else if (change[i] == Dynres_Set_Rebuilt && i == 0)
                    printf("dynamic resolution: the engine rebuilt its render targets, set 0 %ix%i (%i)\n", w, h,
                           debug_lines);
            }
        }
    }
    seen = true;
    seen_frame = frame;
    for (int i = 0; i < Dynres_Sets; i++) {
        seen_sizes[i] = sizes[i];
        seen_use[i] = __atomic_load_n(&use[i], __ATOMIC_RELAXED); // with the marks this tick wrote
    }
}

// each frame: learn the sets that appear right after a step, and hold the sets of those sizes at a last use
// Dynres_Keep_Age frames old, so the engine's lookup doesn't free them after 11 frames, and frees one of them first
// when it has to make room (dynres.h). Only the last use is written, never a size or a render target: which sets are
// valid stays the engine's own (a set is in use while its size isn't 0). Returns how many sets appeared; *view_3d says
// whether the engine's lookup used a set in the last 2 frames (the 3D view renders), not counting the last uses this
// wrote itself.
static int dynres_keep_tick(bool *view_3d) {
    static uint64_t written_use[Dynres_Sets]; // the last use this wrote to each set, 0 for none
    uint32_t sizes[Dynres_Sets];
    uint64_t last_use[Dynres_Sets];
    int appeared = 0;
    *view_3d = false;
    if (!rt_sets_read(sizes, last_use))
        return 0;
    uint64_t frame = *(volatile uint64_t *)(game_base_address + ADDR_rt_frame_counter);
    if (dynres_keep_newest < 0) {
        const char *env = getenv("BO3MACFIX_DYNRESKEEPNEWEST");
        dynres_keep_newest = env != NULL && atoi(env) != 0;
        if (dynres_keep_newest)
            printf("dynamic resolution: BO3MACFIX_DYNRESKEEPNEWEST, holding kept render targets at the newest frame\n");
    }
    for (int i = 0; i < Dynres_Sets; i++)
        *view_3d |= sizes[i] != 0 && last_use[i] <= frame && frame - last_use[i] <= 2 && last_use[i] != written_use[i];
    if (dynres_learn_frames > 0) {
        dynres_learn_frames--;
        for (int i = 1; i < Dynres_Sets; i++) {
            if (sizes[i] == 0 || sizes[i] == dynres_learn_from[i])
                continue;
            bool known = false;
            for (int k = 0; k < Dynres_Keep_Max; k++)
                known |= dynres_keep.size[k] == sizes[i];
            if (!known)
                printf("dynamic resolution: keeping the %ix%i render targets (set %i)\n", sizes[i] >> 16,
                       sizes[i] & 0xffff, i);
            else // a kept size's set the engine had freed: what keeping should prevent
                printf("dynamic resolution: the engine made the %ix%i render targets again (set %i)\n", sizes[i] >> 16,
                       sizes[i] & 0xffff, i);
            dynres_keep_add(&dynres_keep, sizes[i]);
            dynres_learn_from[i] = sizes[i];
            appeared++;
        }
    }
    static dispatch_once_t watching;
    dispatch_once(&watching, ^{
        dynres_pressure_watch();
    });
    static bool was_pressure = false;
    bool pressure = atomic_load(&dynres_pressure);
    if (pressure != was_pressure)
        printf(pressure ? "dynamic resolution: macOS reports memory pressure, keeping no render targets until it's over\n"
                        : "dynamic resolution: the memory pressure is over, keeping render targets again\n");
    was_pressure = pressure;
    // the engine frees what isn't refreshed any more after 11 frames; the steps learn the sizes again afterwards
    if (pressure && dynres_keep_release(&dynres_keep))
        printf("dynamic resolution: letting the engine free the kept render targets\n");
    int hold_age = dynres_keep_newest ? 0 : Dynres_Keep_Age;
    unsigned mask = dynres_keep_mask(&dynres_keep, sizes, last_use, frame, hold_age);
    uint64_t *use = (uint64_t *)(game_base_address + ADDR_rt_set_last_use);
    for (int i = 1; i < Dynres_Sets; i++) {
        written_use[i] = 0;
        if (!(mask & (1u << i)))
            continue;
        // only over the mark read above: if the engine marked the set since (a view used it, or the sweep freed it
        // and set 0), its own mark stays
        uint64_t expected = last_use[i], mark = frame - (uint64_t)hold_age;
        if (__atomic_compare_exchange_n(&use[i], &expected, mark, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            written_use[i] = mark;
    }
    dynres_log_changes(sizes, frame);
    return appeared;
}

// a step: the sets as they are now, to tell the new one
static void dynres_keep_step(void) {
    uint64_t last_use[Dynres_Sets];
    if (rt_sets_read(dynres_learn_from, last_use))
        dynres_learn_frames = 12;
    static int debug = -1;
    if (debug < 0)
        debug = getenv("BO3MACFIX_DYNRESDEBUG") != NULL;
    if (debug && dynres_learn_frames == 12) {
        uint64_t frame = *(volatile uint64_t *)(game_base_address + ADDR_rt_frame_counter);
        char line[400] = "";
        for (int i = 0, n = 0; i < Dynres_Sets && n < (int)sizeof(line); i++)
            if (dynres_learn_from[i] != 0)
                n += snprintf(line + n, sizeof(line) - n, " %i:%ix%i@%lld", i, dynres_learn_from[i] >> 16,
                              dynres_learn_from[i] & 0xffff, (long long)(frame - last_use[i]));
        printf("dynamic resolution: sets (frames since used)%s\n", line);
    }
}

// the warm-up is at percent: keep the set of that size even if it was there before the warm-up (the match started at a
// value the controller had lowered), so it doesn't "appear" after a step. Whether that set is there (warmed up).
static bool dynres_keep_visited(int percent) {
    uint32_t sizes[Dynres_Sets];
    uint64_t last_use[Dynres_Sets];
    if (!rt_sets_read(sizes, last_use))
        return false;
    int set = dynres_set_for_percent(sizes, dynres.base, percent);
    if (set < 0)
        return false;
    for (int k = 0; k < Dynres_Keep_Max; k++)
        if (dynres_keep.size[k] == sizes[set])
            return true;
    printf("dynamic resolution: keeping the %ix%i render targets (set %i)\n", sizes[set] >> 16, sizes[set] & 0xffff, set);
    dynres_keep_add(&dynres_keep, sizes[set]);
    return true;
}

// whether the local player's controls are frozen, as in a match's intro before the players can move: 1 yes, 0 no, -1
// unknown (this Mac doesn't run the server, or there's no player yet). Reads through mach_vm_read_overwrite, so a
// stale pointer can't crash; without sv_running, g_entities is what the last game here left, so it isn't read.
static int player_frozen(void) {
    uint64_t dvar = 0, entities = 0, client = 0;
    uint32_t type = 0, flags = 0;
    uint8_t running = 0;
    mach_vm_size_t got = 0;
#define READ(address, out) (mach_vm_read_overwrite(mach_task_self(), (address), sizeof(out), (mach_vm_address_t)&(out), &got) == KERN_SUCCESS && got == sizeof(out))
    if (!READ(game_base_address + ADDR_sv_running, dvar) || dvar == 0 || !READ(dvar + dvar_Type, type) || type != 1 ||
        !READ(dvar + dvar_Value, running) || running == 0)
        return -1;
    if (!READ(game_base_address + ADDR_g_entities_Ptr, entities) || entities == 0 ||
        !READ(entities + gentity_Client, client) || client == 0 || !READ(client + gclient_Flags, flags))
        return -1;
#undef READ
    return (flags & gclient_Frozen) != 0;
}

static const char *frozen_text(int frozen) {
    return frozen > 0 ? "frozen" : frozen == 0 ? "free" : "unknown";
}

// a match's first frames: one frame at each size the controller can reach, then back to the user's value (dynres.h).
// True while it warms up (the controller waits, and starts again from the user's value after it). A map loading arms
// it again.
static bool dynres_prewarm_tick(char *dvar, uint64_t now, bool view_3d, int appeared) {
    static uint64_t started = 0, last = 0, ended = 0;
    static float frame_ms[24];
    static int frames = 0, visited = 0, debug = -1, watch = 0;
    if (debug < 0)
        debug = getenv("BO3MACFIX_DYNRESDEBUG") != NULL;
    if (atomic_exchange(&dynres_prewarm_arm, false))
        dynres_prewarm.armed = true;
    // debug: when the player gets control after the warm-up (looked at every 6th frame for 60 s)
    if (watch > 0 && --watch % 6 == 0) {
        int frozen = player_frozen();
        if (frozen == 0 || watch == 0) {
            printf("dynamic resolution: the player's controls are %s %.2f s after the warm-up\n", frozen_text(frozen),
                   (now - ended) / 1e9);
            watch = 0;
        }
    }
    bool running = dynres_prewarm_running(&dynres_prewarm);
    if (!running && !dynres_prewarm.armed)
        return false;
    if (running && frames < (int)(sizeof(frame_ms) / sizeof(frame_ms[0])))
        frame_ms[frames++] = (float)((now - last) / 1e6);
    bool warmed = running && dynres_keep_visited(dynres_prewarm.percent[dynres_prewarm.index]);
    last = now;
    // the 3D view renders outside the menus, and the player's controls are frozen (the intro), or else the match's UI
    // polls (a game this Mac doesn't host, or a map whose intro doesn't freeze the player)
    bool ready = false;
    if (!running && dynres_prewarm.armed && view_3d && !frame_limit_in_menus(&limits, now))
        ready = player_frozen() == 1 || frame_limit_in_match(&limits, now);
    if (ready && atomic_load(&dynres_pressure)) {
        // its sets wouldn't be kept: no warm-up for this map
        printf("dynamic resolution: the match started under memory pressure, no warm-up\n");
        dynres_prewarm.armed = false;
        return false;
    }
    // on battery with BatteryResolution, the sizes under its ceiling (dynres.h)
    int ceiling = dynres_battery_ceiling(dynres.base, atomic_load(&battery_resolution), atomic_load(&limits.on_battery));
    int percent = dynres_prewarm_frame_up_to(&dynres_prewarm, dynres.base, dynres.lowest, ceiling, ready,
                                             warmed || appeared > 0);
    if (percent == 0)
        return running;
    if (!running) {
        started = now;
        frames = 0;
        visited = 0;
        printf("dynamic resolution: the match started, warming up %i sizes%s%s\n", dynres_prewarm.count,
               debug ? ", the player's controls are " : "", debug ? frozen_text(player_frozen()) : "");
    }
    dynres_keep_step();
    scene_multiplier_set(dvar, percent);
    if (dynres_prewarm_running(&dynres_prewarm)) {
        atomic_store(&dynres_written, percent);
        visited++;
        return true;
    }
    atomic_store(&dynres_written, 0);
    dynres_reset(&dynres, dynres.base, dynres.lowest);
    dynres_start_low_pending = true;
    char times[24 * 8] = "";
    for (int i = 0, n = 0; i < frames && n < (int)sizeof(times); i++)
        n += snprintf(times + n, sizeof(times) - n, i == 0 ? "%.1f" : ", %.1f", frame_ms[i]);
    printf("dynamic resolution: warmed up %i sizes in %i frames (%.0f ms), back to %i%%; the frames took %s ms\n",
           visited, frames, (now - started) / 1e6, dynres.base, times);
    if (debug) {
        int frozen = player_frozen();
        printf("dynamic resolution: the player's controls are %s at the end of the warm-up\n", frozen_text(frozen));
        ended = now;
        watch = frozen > 0 ? 60 * 120 : 0;
    }
    return true;
}

// the frame rate to reach: the display's refresh rate, or a lower limit (ours as the display shows it, or MaxFPS), see
// dynres_target
static int dynres_target_fps(int frame_limit_fps) {
    static void *maxfps = NULL;
    if (maxfps == NULL)
        maxfps = ((void *(*)(uint32_t))(game_base_address + ADDR_Dvar_FindByHash))(Hash_com_maxfps);
    int max_fps = maxfps != NULL ? ((int (*)(void *))(game_base_address + ADDR_Dvar_GetInt))(maxfps) : 0;
    return dynres_target(present_display_max_fps(), frame_limit_fps, max_fps);
}

// BO3MACFIX_DYNRESTRACE=1: a step's next presents and the command buffers that complete around them, in ms from the
// write, to tell a long frame's GPU work from a wait on the CPU side. Diagnostics only.
enum { Dynres_Trace_Presents = 12, Dynres_Trace_Wait = 6 };
static int dynres_trace = -1, dynres_traced = -1; // presents recorded since the step, -1 = not tracing
static double dynres_trace_at[Dynres_Trace_Presents], dynres_trace_t0;
static uint64_t dynres_trace_frame[Dynres_Trace_Presents + 1];

static void dynres_trace_step(uint64_t now) {
    if (dynres_trace < 0)
        dynres_trace = getenv("BO3MACFIX_DYNRESTRACE") != NULL;
    if (!dynres_trace)
        return;
    dynres_trace_t0 = now / 1e9;
    dynres_trace_frame[Dynres_Trace_Presents] = *(volatile uint64_t *)(game_base_address + ADDR_rt_frame_counter);
    dynres_traced = 0;
    gpu_diag_trace_begin(Gpu_Diag_Trace_Max);
}

static void dynres_trace_present(uint64_t now) {
    if (dynres_traced < 0)
        return;
    if (dynres_traced < Dynres_Trace_Presents) {
        dynres_trace_at[dynres_traced] = now / 1e9 - dynres_trace_t0;
        dynres_trace_frame[dynres_traced] = *(volatile uint64_t *)(game_base_address + ADDR_rt_frame_counter);
        dynres_traced++;
        return;
    }
    if (++dynres_traced < Dynres_Trace_Presents + Dynres_Trace_Wait)
        return;
    dynres_traced = -1;
    static double times[Gpu_Diag_Trace_Max][3];
    int count = gpu_diag_trace_take(times, Gpu_Diag_Trace_Max);
    static char line[6000];
    int n = snprintf(line, sizeof(line), "dynamic resolution: trace (ms from the write): presents");
    for (int i = 0; i < Dynres_Trace_Presents && n < (int)sizeof(line) - 40; i++)
        n += snprintf(line + n, sizeof(line) - n, " %.1f(f+%lld)", dynres_trace_at[i] * 1e3,
                      (long long)(dynres_trace_frame[i] - dynres_trace_frame[Dynres_Trace_Presents]));
    n += snprintf(line + n, sizeof(line) - n, "; %i buffers committed@GPU start-end:", count);
    for (int i = 0; i < count && n < (int)sizeof(line) - 40; i++)
        n += snprintf(line + n, sizeof(line) - n, " %.1f@%.1f-%.1f", (times[i][0] - dynres_trace_t0) * 1e3,
                      (times[i][1] - dynres_trace_t0) * 1e3, (times[i][2] - dynres_trace_t0) * 1e3);
    printf("%s\n", line);
}

// BO3MACFIX_DYNRESTESTFRAMES=n with BO3MACFIX_DYNRESTEST, a stress test (off by default): after the match's warm-up,
// the next size of the test's list every n frames, as fast as the engine can change its render target sets. The sizes
// aren't learned meanwhile (the warm-up's stay kept beside them), so a list of more sizes than the sets the engine has
// left makes its lookup take a set in use at every step, as when all 8 are taken (dynres_log_changes says which and
// how old). A line every 2 s.
static void dynres_test_fast_tick(char *dvar, uint64_t now) {
    static uint64_t since = 0;
    static int frames = 0, index = 0, steps = 0;
    dynres_learn_frames = 0; // the warm-up's sizes stay the kept ones
    if (since == 0) {
        since = now;
        printf("dynamic resolution: test, the next of %i sizes every %i frames from now on\n", dynres_test_count,
               dynres_test_frames);
    }
    if (++frames >= dynres_test_frames) {
        frames = 0;
        index = (index + 1) % dynres_test_count;
        int percent = dynres_test[index];
        scene_multiplier_set(dvar, percent);
        atomic_store(&dynres_written, percent == dynres.base ? 0 : percent);
        steps++;
    }
    if (now - since >= 2000000000ULL) {
        printf("dynamic resolution: test, %i steps in %.1f s (one every %i frames), now %i%%\n", steps,
               (now - since) / 1e9, dynres_test_frames, dynres_test[index]);
        since = now;
        steps = 0;
    }
}

// from the render thread, for each frame
static void dynres_tick(int frame_limit_fps) {
    static uint64_t window_start = 0, last_present = 0, test_start = 0;
    static int frames = 0, logging = 0, from = 0, to = 0, test_index = 0;
    static double busy_start = 0, window_fps = 0, window_busy = 0;
    static const char *why = "", *logged_why = ""; // what the change was, for its log line
    static float after[4];
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    dynres_trace_present(now);
    // the frames right after a change show what it cost: the engine allocates render targets for the new size
    if (logging > 0 && last_present != 0) {
        after[4 - logging] = (float)((now - last_present) / 1e6);
        if (--logging == 0)
            printf("dynamic resolution: %i%% -> %i%% (%.1f fps of %i, GPU busy %.0f%%%s), the next frames took "
                   "%.1f, %.1f, %.1f, %.1f ms\n", from, to, window_fps, dynres_target_fps(frame_limit_fps),
                   window_busy * 100,
                   logged_why, after[0], after[1], after[2], after[3]);
    }
    last_present = now;
    char *dvar = scene_multiplier_dvar();
    if (dvar == NULL)
        return;
    bool view_3d;
    int appeared = dynres_keep_tick(&view_3d);
    int current = *(volatile int *)(dvar + dvar_Value), latched = *(volatile int *)(dvar + dvar_Latched);
    int written = atomic_load(&dynres_written), setting = atomic_load(&dynres_setting);
    int battery = atomic_load(&battery_resolution);
    bool active = setting != 0 || battery != 0; // DynamicResolution, BatteryResolution or both
    bool reconfigure = atomic_exchange(&dynres_reconfigure, false);
    if (written != 0 && (current != written || latched != written)) {
        // something else set ResolutionPercent (the options): that's the user's value from now on
        printf("dynamic resolution: ResolutionPercent was set to %i, going on from there\n", latched);
        atomic_store(&dynres_written, 0);
        written = 0;
        dynres.base = 0;
    } else if (written != 0 && (reconfigure || !active)) {
        // turned off or changed in the menu: the user's value back first, and a warm-up in progress ends (it would
        // otherwise resume when the feature is turned on again, with the base it started with)
        int base = atomic_load(&dynres_base);
        dynres_prewarm_cancel(&dynres_prewarm);
        scene_multiplier_set(dvar, base);
        atomic_store(&dynres_written, 0);
        printf(!active ? "dynamic resolution off, back to %i%%\n" : "dynamic resolution: back to %i%% for the new setting\n", base);
        dynres.base = 0;
        return;
    }
    if (reconfigure)
        dynres.base = 0;
    if (written == 0 && (!active || current != latched)) // off, or a change the engine applies first
        return;
    if (written == 0 && dynres.base != current) {
        // with DynamicResolution off (BatteryResolution alone) the controller's floor is the user's own value: it only
        // ever goes down to BatteryResolution's ceiling
        dynres_reset(&dynres, current, setting != 0 ? dynres_lowest_for(current, setting) : current);
        battery_ceiling_logged = 0;
        atomic_store(&dynres_base, current);
        window_start = 0;
        dynres_start_low_pending = true;
        if (setting != 0)
            printf("dynamic resolution: between %i%% and %i%%\n", dynres.lowest, dynres.base);
        else
            printf("dynamic resolution: for BatteryResolution only: %i%% plugged in, %i%% at most on battery\n",
                   dynres.base, dynres_battery_ceiling(dynres.base, battery, true));
    }
    if (dynres_prewarm_tick(dvar, now, view_3d, appeared)) {
        window_start = 0; // the controller's first window starts after it
        return;
    }
    if (dynres_test_frames > 0 && dynres_test_count > 0 && !dynres_prewarm.armed) { // in a match, after its warm-up
        dynres_test_fast_tick(dvar, now);
        return;
    }
    if (window_start == 0) {
        window_start = now;
        frames = 0;
        busy_start = gpu_diag_busy_total();
        return;
    }
    if (++frames < Dynres_Window_Frames)
        return;
    double seconds = (now - window_start) / 1e9, busy_total = gpu_diag_busy_total();
    window_fps = frames / seconds;
    window_busy = (busy_total - busy_start) / seconds;
    window_start = now;
    frames = 0;
    busy_start = busy_total;
    int percent;
    if (dynres_test_count > 0) {
        if (test_start == 0)
            test_start = now;
        if (now - test_start >= (uint64_t)dynres_test_seconds * 1000000000ULL) {
            test_start = now;
            test_index = (test_index + 1) % dynres_test_count;
        }
        percent = dynres_test[test_index];
    } else {
        if (dynres_start_low < 0)
            dynres_start_low = getenv("BO3MACFIX_DYNRESSTARTLOW") != NULL &&
                               atoi(getenv("BO3MACFIX_DYNRESSTARTLOW")) != 0;
        // the kind of limit (dynres.h): a situation's (the pause menu, the menus, the background: the limiter's Limit_*
        // bits, whatever the match's own limit), else one of the match below the display's rate, where the way up is a
        // trial (Dynres_Hold_Up_Seconds), else none
        int display_fps = present_display_max_fps(), target = dynres_target_fps(frame_limit_fps);
        dynres_limit_t limit = limits.logged != 0 ? Dynres_Situation_Limit
                               : display_fps > 0 && target > 0 && target < display_fps ? Dynres_Match_Limit
                                                                                       : Dynres_No_Limit;
        if (dynres_start_low > 0 && dynres_start_low_pending) {
            percent = dynres_start_at_floor(&dynres);
            dynres.event = Dynres_Event_None;
            printf("dynamic resolution: BO3MACFIX_DYNRESSTARTLOW, starting at the floor, %i%%\n", percent);
        } else {
            // BatteryResolution's ceiling for this window, said in the log when it changes
            dynres_set_ceiling(&dynres, dynres_battery_ceiling(dynres.base, battery, atomic_load(&limits.on_battery)));
            if (battery == 0)
                battery_ceiling_logged = 0; // off: turned on again later on battery, it is said again
            if (battery != 0) {
                int news = dynres_ceiling_news(dynres.ceiling, dynres.base, &battery_ceiling_logged);
                if (news == Dynres_Ceiling_On_Battery)
                    printf("dynamic resolution: on battery, the 3D scene at %i%% at most (BatteryResolution)\n",
                           dynres.ceiling);
                else if (news == Dynres_Ceiling_Plugged_In)
                    printf("dynamic resolution: plugged in, the 3D scene up to %i%% again (BatteryResolution)\n",
                           dynres.ceiling);
            }
            percent = dynres_window_limited(&dynres, window_fps, window_busy, target, limit);
            // the frame target rose and this window, behind with the GPU busy, wasn't taken as a reason to step down
            // (dynres.h, Dynres_Rise_Hold_Seconds): said for the first such window after a rise, each with
            // BO3MACFIX_DYNRESDEBUG; the window of the rise, the one after it, or one at Dynres_Rise_Hold_Share of the
            // target or more
            static int rise_debug = -1;
            if (rise_debug < 0)
                rise_debug = getenv("BO3MACFIX_DYNRESDEBUG") != NULL;
            if (dynres.event == Dynres_Event_Held && (dynres.held_windows == 1 || rise_debug)) {
                char share[64] = " in the window of the rise";
                if (!dynres.rise_window)
                    snprintf(share, sizeof(share), ", %.0f%% of it%s", window_fps * 100 / target,
                             dynres.after_rise_window ? " in the window after the rise's" : "");
                printf("dynamic resolution: the frame target rose to %i, no step down while the GPU catches up "
                       "(%.1f fps%s, GPU busy %.0f%%, %.1f s of the hold left)\n",
                       target, window_fps, share, window_busy * 100, dynres.rise_hold > 0 ? dynres.rise_hold : 0.0);
            }
            // a window of the hold too far under the new target for a clock coming up: judged as without a rise; the
            // step it takes says so, with the time from the start of the window of the rise to the end of this one
            if (dynres.event == Dynres_Event_Too_Slow)
                snprintf(dynres_too_slow, sizeof(dynres_too_slow), ", %.0f%% of the target %.1f s after it rose",
                         window_fps * 100 / target, Dynres_Rise_Hold_Seconds - dynres.rise_hold);
        }
        dynres_start_low_pending = false;
        why = dynres.event == Dynres_Event_Trial ? ", a trial under the frame limit"
              : dynres.event == Dynres_Event_Taken_Back ? ", the trial taken back"
              : dynres.event == Dynres_Event_Failed_Late ? ", soon after the last trial, which counts as failed"
              : dynres.event == Dynres_Event_Ceiling ? ", BatteryResolution's ceiling"
              : dynres.event == Dynres_Event_Too_Slow ? dynres_too_slow : "";
    }
    int value = written != 0 ? written : current;
    if (percent == value || logging > 0)
        return;
    from = value;
    to = percent;
    logging = 4;
    logged_why = why;
    dynres_keep_step();
    dynres_trace_step(now);
    scene_multiplier_set(dvar, percent);
    atomic_store(&dynres_written, percent == dynres.base ? 0 : percent);
}

int gpu_tweaks_dynres_held_base(void) {
    return atomic_load(&dynres_written) != 0 ? atomic_load(&dynres_base) : 0;
}

static void dynres_read_test(void) {
    const char *env = getenv("BO3MACFIX_DYNRESTEST");
    for (const char *p = env; p != NULL && *p != '\0' && dynres_test_count < 8;) {
        int percent = atoi(p);
        if (percent >= 50 && percent <= 200)
            dynres_test[dynres_test_count++] = percent;
        p = strchr(p, ',');
        if (p != NULL)
            p++;
    }
    env = getenv("BO3MACFIX_DYNRESTESTSECONDS");
    if (env != NULL && atoi(env) > 0)
        dynres_test_seconds = atoi(env);
    env = getenv("BO3MACFIX_DYNRESTESTFRAMES");
    if (env != NULL && atoi(env) > 0)
        dynres_test_frames = atoi(env);
}

void gpu_tweaks_dynres_init(int setting, code_patch_fn checked_code_patch) {
    const char *env = getenv("BO3MACFIX_DYNAMICRESOLUTION");
    if (env != NULL)
        setting = atoi(env);
    dynres_read_test();
    if (dynres_test_count > 0 && setting == 0)
        setting = 50;
    if (setting < 50 || setting > 95)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no DynamicResolution\n");
        return;
    }
    atomic_store(&dynres_setting, setting);
    if (dynres_test_count > 0)
        printf("dynamic resolution: testing, %i steps every %i s\n", dynres_test_count, dynres_test_seconds);
    else
        printf("dynamic resolution on, down to %i%% of ResolutionPercent when the GPU can't keep up\n", setting);
}

// called instead of objc_msgSend for Aspyr's [commandBuffer presentDrawable:drawable]
static void present_drawable(id command_buffer, SEL sel, id drawable) {
    bool resolution_control = atomic_load(&dynres_setting) != 0 || atomic_load(&battery_resolution) != 0 ||
                              atomic_load(&dynres_written) != 0;
    if (frame_stats_detail || resolution_control)
        gpu_diag_watch_command_buffers(command_buffer);
    if (frame_stats_seconds > 0)
        frame_stats_tick();
    if (metalfx_active)
        metalfx_before_present(command_buffer);
    // the lowest limit that applies to this frame; the situational ones are logged here, where they start or stop
    int started, stopped;
    int fps = frame_limit_for_frame(&limits, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), &started, &stopped);
    if (started | stopped)
        log_limits(started, stopped, fps);
    if (resolution_control)
        dynres_tick(fps);
    double min_duration = fps > 0 ? 1.0 / fps : 0;
    static unsigned int paced_frames = 0;
    if (min_duration > 0 && paced_frames++ % 120 == 0)
        check_pacing(min_duration);
    static int supported = -1;
    if (min_duration > 0 && supported == -1)
        supported = class_respondsToSelector(object_getClass(command_buffer), sel_present_after_minimum_duration);
    if (min_duration > 0)
        wait_for_turn(min_duration);
    if (frame_pace_stats && frame_stats_seconds > 0)
        gpu_diag_watch_presented((void *)drawable);
    if (min_duration > 0 && supported)
        ((void (*)(id, SEL, id, double))objc_msgSend)(command_buffer, sel_present_after_minimum_duration, drawable, min_duration);
    else
        ((void (*)(id, SEL, id))objc_msgSend)(command_buffer, sel, drawable);
}

// a pointer-sized slot that a rip-relative call at address can reach (+-2GB), on a page past the end of the game's
// image. The calls we redirect are all in Aspyr's present function, so they share one page.
static void **alloc_slot_near(uint64_t address) {
    static void **page = NULL;
    static unsigned int used = 0;
    for (uint64_t at = (address + 0x20000000) & ~0xFFFFFULL; page == NULL && at < address + 0x78000000; at += 0x1000000) {
        mach_vm_address_t slot = at;
        if (mach_vm_allocate(mach_task_self(), &slot, 0x1000, VM_FLAGS_FIXED) == KERN_SUCCESS)
            page = (void **)slot;
    }
    if (page == NULL || used >= 0x1000 / sizeof(void *) || (uint64_t)page - address >= 0x7FF00000)
        return NULL;
    return &page[used++];
}

// points a call qword ptr [rip + disp32] that goes through objc_msgSend's pointer at function instead,
// returns 1 if the game's code was changed
static int redirect_objc_msgSend_call(uint32_t address, const uint8_t *expected, void *function, code_patch_fn checked_code_patch) {
    // the pre-patched executable already calls a stub here that jumps through a slot
    if (prepatch_redirect_call(address, function))
        return 1;
    uint8_t *code = (uint8_t *)(game_base_address + address);
    if (memcmp(code, expected, 6) != 0) {
        printf("unexpected code at %08x, not patching\n", address);
        return 0;
    }
    void **slot = alloc_slot_near((uint64_t)code);
    if (slot == NULL) {
        printf("no free memory near %08x, not patching\n", address);
        return 0;
    }
    *slot = function;
    int32_t disp = (int32_t)((int64_t)(uint64_t)slot - (int64_t)((uint64_t)code + 6));
    uint8_t call_slot[6] = { 0xFF, 0x15 };
    memcpy(call_slot + 2, &disp, sizeof(disp));
    checked_code_patch(address, expected, call_slot, sizeof(call_slot));
    return memcmp(code, call_slot, sizeof(call_slot)) == 0;
}

// pacing and MetalFX both need Aspyr's presentDrawable: call to go through present_drawable
static int present_redirected = -1; // -1 not tried yet, 1 present goes through present_drawable

static int redirect_present(code_patch_fn checked_code_patch) {
    static const uint8_t call_objc_msgSend[] = { 0xFF, 0x15, 0xD9, 0xDB, 0x87, 0x01 };
    if (present_redirected == -1)
        present_redirected = redirect_objc_msgSend_call(ADDR_ASL_Present_PresentDrawableCall, call_objc_msgSend,
                                                        (void *)present_drawable, checked_code_patch);
    return present_redirected;
}

static void frames_in_flight_patch(int frames, code_patch_fn checked_code_patch) {
    static const uint8_t mov_edi_2[] = { 0xBF, 0x02, 0x00, 0x00, 0x00 };
    uint8_t mov_edi_frames[] = { 0xBF, (uint8_t)frames, 0x00, 0x00, 0x00 };
    checked_code_patch(ADDR_ASL_FramesInFlightSemaphoreInstr, mov_edi_2, mov_edi_frames, sizeof(mov_edi_frames));
    printf("allowing %i frames in flight\n", frames);
}

static void frame_pacing_patch(int fps, code_patch_fn checked_code_patch) {
    if (!redirect_present(checked_code_patch)) {
        printf("not pacing frames\n");
        return;
    }
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    atomic_store(&limits.pacing_fps, fps);
    printf("pacing frames at %i fps\n", fps);
}

void gpu_tweaks_init(int frames_in_flight, int pacing_fps, code_patch_fn checked_code_patch) {
    const char *env = getenv("BO3MACFIX_FRAMESINFLIGHT");
    if (env != NULL)
        frames_in_flight = atoi(env);
    env = getenv("BO3MACFIX_FRAMEPACINGFPS");
    if (env != NULL)
        pacing_fps = atoi(env);

    // the ring of command buffers behind it has 3 slots, so more than 3 can't be in flight anyway
    if (frames_in_flight == 1 || frames_in_flight == 3)
        frames_in_flight_patch(frames_in_flight, checked_code_patch);
    if (pacing_fps >= 30 && pacing_fps <= 240)
        frame_pacing_patch(pacing_fps, checked_code_patch);
}

void gpu_tweaks_metalfx_init(int metalfx_upscale, code_patch_fn checked_code_patch) {
    const char *env = getenv("BO3MACFIX_METALFXUPSCALE");
    if (env != NULL)
        metalfx_upscale = atoi(env);
    if (metalfx_upscale == 0)
        return;

    // Aspyr's [blitEncoder copyFromTexture:backbuffer ... toTexture:drawable.texture ...] in its present function
    static const uint8_t call_objc_msgSend[] = { 0xFF, 0x15, 0xDC, 0xDD, 0x87, 0x01 };
    if (!redirect_present(checked_code_patch) ||
        !redirect_objc_msgSend_call(ADDR_ASL_Present_CopyToDrawableCall, call_objc_msgSend, metalfx_copy_function(),
                                    checked_code_patch)) {
        printf("MetalFX: couldn't hook Aspyr's present, not upscaling\n");
        return;
    }
    metalfx_active = metalfx_init(metalfx_upscale);
}

void gpu_tweaks_frame_stats_init(int seconds, code_patch_fn checked_code_patch) {
    const char *env = getenv("BO3MACFIX_FRAMESTATS");
    if (env != NULL)
        seconds = atoi(env);
    const char *pace = getenv("BO3MACFIX_FRAMEPACESTATS");
    frame_pace_stats = pace != NULL && atoi(pace) != 0;
    if (seconds <= 0)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no frame stats\n");
        return;
    }
    frame_stats_seconds = seconds;
    const char *detail = getenv("BO3MACFIX_FRAMESTATSDETAIL");
    frame_stats_detail = detail != NULL && atoi(detail) != 0;
    // the threads' CPU time by name and the time in the server's and the client's frame, two more lines a window
    if (frame_stats_detail) {
        thread_stats_init();
        thread_stats_set_contexts((const volatile uint64_t *)(game_base_address + ADDR_Sys_ThreadIds));
    }
    printf("logging frame stats every %i s%s\n", seconds, frame_pace_stats ? ", and when the frames are shown" : "");
}

// whether the Mac is running on battery right now (BO3MACFIX_ONBATTERY=1 pretends it is, for testing)
static bool running_on_battery(void) {
    const char *env = getenv("BO3MACFIX_ONBATTERY");
    if (env != NULL)
        return atoi(env) != 0;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info == NULL)
        return false;
    CFStringRef source = IOPSGetProvidingPowerSourceType(info);
    bool battery = source != NULL && CFStringCompare(source, CFSTR(kIOPMBatteryPowerKey), 0) == kCFCompareEqualTo;
    CFRelease(info);
    return battery;
}

// whether macOS's Low Power Mode is on (System Settings > Battery, or Control Center), from NSProcessInfo's
// lowPowerModeEnabled (macOS 12 and later; off before that). BO3MACFIX_LOWPOWERMODE=1 pretends it is, for testing.
static bool low_power_mode_on(void) {
    const char *env = getenv("BO3MACFIX_LOWPOWERMODE");
    if (env != NULL)
        return atoi(env) != 0;
    Class process_info_class = objc_getClass("NSProcessInfo");
    SEL enabled = sel_registerName("isLowPowerModeEnabled");
    if (process_info_class == Nil)
        return false;
    id info = ((id (*)(Class, SEL))objc_msgSend)(process_info_class, sel_registerName("processInfo"));
    if (info == nil || !class_respondsToSelector(object_getClass(info), enabled))
        return false;
    return ((BOOL (*)(id, SEL))objc_msgSend)(info, enabled) != NO;
}

// BatterySaver (battery_saver.h): the user's own BatteryFPS, BatteryResolution, IdleMenuFPS, BackgroundFPS and
// IdleDisplaySleep as set at the start or on the settings page, and the level; what applies (battery_saver_effective,
// which only ever adds saving, and only on battery) goes where each setting's value has always gone
static battery_settings_t user_battery;
static os_unfair_lock user_battery_lock = OS_UNFAIR_LOCK_INIT;
static atomic_int saver_level = Battery_Saver_Off;

static void saver_apply(void) {
    // computed and stored under the lock: a settings page change and a plug-in seen by the power watch can come at
    // the same moment from two threads, and the older result must not be stored last (the review of the follow-ups,
    // B4). display_sleep_set_mode takes display_sleep_mac.c's own lock, which never calls back here.
    os_unfair_lock_lock(&user_battery_lock);
    battery_settings_t e =
        battery_saver_effective(atomic_load(&saver_level), atomic_load(&limits.on_battery), user_battery);
    atomic_store(&limits.battery_fps, e.battery_fps);
    atomic_store(&battery_resolution, e.battery_resolution);
    atomic_store(&limits.idle_menu_fps, e.idle_menu_fps);
    if (e.idle_menu_fps == 0)
        atomic_store(&limits.idle, false);
    atomic_store(&limits.background_fps, e.background_fps);
    display_sleep_set_mode(e.idle_display_sleep);
    os_unfair_lock_unlock(&user_battery_lock);
}

// what applies on battery with BatterySaver's level, for the log lines
static battery_settings_t saver_on_battery(void) {
    os_unfair_lock_lock(&user_battery_lock);
    battery_settings_t e = battery_saver_effective(atomic_load(&saver_level), true, user_battery);
    os_unfair_lock_unlock(&user_battery_lock);
    return e;
}

// a setting's line says the player's own value; while BatterySaver's level sets another on battery, this one says
// that too (the review of the follow-ups, B3). After the setting's own line, and for all five with BatterySaver's.
static void saver_note(const char *name, int own, int applies) {
    int level = atomic_load(&saver_level);
    if (level != Battery_Saver_Off && own != applies)
        printf("battery saver: %s is %i, BatterySaver %s makes it %i on battery\n", name, own,
               level == Battery_Saver_Saver ? "Saver" : "Longest", applies);
}

// a user's own value changed (startup or the settings page): what applies follows
static void saver_user(int *field, int value) {
    os_unfair_lock_lock(&user_battery_lock);
    *field = value;
    os_unfair_lock_unlock(&user_battery_lock);
    saver_apply();
}

// IdleDisplaySleep: the situation as the frame limits know it, for display_sleep_mac.c to decide on (nothing to do with
// the setting off: it returns at once)
static void display_sleep_tick(void) {
    display_sleep_update(frame_limit_away_from_play(&limits, clock_gettime_nsec_np(CLOCK_UPTIME_RAW)),
                         atomic_load(&limits.on_battery));
}

// checks the power source and Low Power Mode now and then every 5 seconds (once, however often it's asked), for
// BatteryFPS and LowPowerFPS, and says so when one of them changes while its limit is set
static void start_power_watch(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        atomic_store(&limits.on_battery, running_on_battery());
        atomic_store(&limits.low_power, low_power_mode_on());
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC);
        dispatch_source_set_event_handler(timer, ^{
            bool battery = running_on_battery();
            bool changed = battery != atomic_exchange(&limits.on_battery, battery);
            if (changed && atomic_load(&saver_level) != Battery_Saver_Off)
                saver_apply(); // BatterySaver's menus and background values apply on battery only
            if (changed && atomic_load(&limits.battery_fps) > 0)
                printf("%s, %s\n", battery ? "running on battery" : "plugged in", battery ? "limiting the frame rate" : "frame rate unlimited");
            bool low_power = low_power_mode_on();
            if (low_power != atomic_exchange(&limits.low_power, low_power) && atomic_load(&limits.low_power_fps) > 0)
                printf("Low Power Mode %s, %s\n", low_power ? "on" : "off",
                       low_power ? "limiting the frame rate" : "its frame limit is off");
            display_sleep_tick();
        });
        dispatch_resume(timer);
    });
}

bool gpu_tweaks_on_battery(void) {
    start_power_watch();
    return atomic_load(&limits.on_battery);
}

void gpu_tweaks_battery_fps_init(int fps, code_patch_fn checked_code_patch) {
    const char *env = getenv("BO3MACFIX_BATTERYFPS");
    if (env != NULL)
        fps = atoi(env);
    if (fps < 30 || fps > 240)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no BatteryFPS\n");
        return;
    }
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    start_power_watch();
    saver_user(&user_battery.battery_fps, fps);
    printf("limiting the frame rate to %i fps on battery (%s now)\n", fps, atomic_load(&limits.on_battery) ? "on battery" : "plugged in");
}

void gpu_tweaks_battery_resolution_init(int setting, code_patch_fn checked_code_patch) {
    if (setting < 50 || setting > 95)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no BatteryResolution\n");
        return;
    }
    start_power_watch();
    saver_user(&user_battery.battery_resolution, setting);
    printf("the 3D scene at %i%% of ResolutionPercent at most on battery (%s now)\n", setting,
           atomic_load(&limits.on_battery) ? "on battery" : "plugged in");
}

void gpu_tweaks_low_power_fps_init(int fps, code_patch_fn checked_code_patch) {
    if (fps < 30 || fps > 240)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no LowPowerFPS\n");
        return;
    }
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    start_power_watch();
    atomic_store(&limits.low_power_fps, fps);
    printf("limiting the frame rate to %i fps in Low Power Mode (%s now)\n", fps,
           atomic_load(&limits.low_power) ? "on" : "off");
}

void gpu_tweaks_pause_menu_fps_init(int fps, code_patch_fn checked_code_patch) {
    if (fps < 30 || fps > 240)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no PauseMenuFPS\n");
        return;
    }
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    atomic_store(&limits.pause_menu_fps, fps);
    printf("limiting the frame rate to %i fps in the pause menu\n", fps);
}

// IdleMenuFPS: when a controller last gave input (the XInput hook, gpu_tweaks_controller_input), 0 never
static _Atomic uint64_t controller_input_at = 0;

// IdleMenuFPS: whether nothing has been pressed for Idle_Menu_Seconds, looked at with each of notices.lua's polls
// (four times a second in the menus and in a match); the keyboard, mouse and trackpad from macOS's own event times
// whether the game is paused (cl_paused): the pause menu of a co-op or LAN match doesn't pause it
static bool game_paused(void) {
    const uint8_t *dvar = *(const uint8_t *const volatile *)(game_base_address + ADDR_dvar_cl_paused);
    return dvar != NULL && *(const volatile int *)(dvar + dvar_Value) != 0;
}

static void idle_menu_tick(uint64_t now) {
    if (atomic_load(&limits.idle_menu_fps) == 0) {
        atomic_store(&limits.idle, false);
        return;
    }
    double hid = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState, kCGAnyInputEventType);
    uint64_t pad = atomic_load(&controller_input_at);
    double controller = pad != 0 && now > pad ? (now - pad) / 1e9 : pad != 0 ? 0.0 : 1e9;
    // only with ControllerBridge (else a controller player's menu would count as idle while used), and in the menus
    // or a pause that really paused (the review of 2026-11-18, M1 and M4)
    atomic_store(&limits.idle, frame_limit_idle_menu(hid, controller, gc_bridge_enabled(),
                                                     frame_limit_in_menus(&limits, now), game_paused()));
}

void gpu_tweaks_controller_input(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, int16_t left_x,
                                 int16_t left_y, int16_t right_x, int16_t right_y) {
    if (atomic_load_explicit(&limits.idle_menu_fps, memory_order_relaxed) == 0 ||
        !display_sleep_is_input(buttons, left_trigger, right_trigger, left_x, left_y, right_x, right_y))
        return;
    atomic_store(&controller_input_at, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    atomic_store(&limits.idle, false); // the menu's own limit from the next frame, not the next poll
}

void gpu_tweaks_pause_menu_open(bool open) {
    frame_limit_set_pause_menu(&limits, open, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    idle_menu_tick(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    display_sleep_tick();
}

void gpu_tweaks_menu_fps_init(int fps, code_patch_fn checked_code_patch) {
    if (fps < 30 || fps > 240)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no MenuFPS\n");
        return;
    }
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    atomic_store(&limits.menu_fps, fps);
    printf("limiting the frame rate to %i fps in the menus\n", fps);
}

void gpu_tweaks_menus_open(bool in_menus) {
    frame_limit_set_menus(&limits, in_menus, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    idle_menu_tick(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    display_sleep_tick();
}

void gpu_tweaks_ui_gone(void) {
    frame_limit_ui_gone(&limits);
    display_sleep_tick(); // a map loads: play goes on
    atomic_store(&dynres_prewarm_arm, true); // a map loads: warm DynamicResolution's sizes up when its match starts
}

void gpu_tweaks_idle_menu_fps_init(int fps, code_patch_fn checked_code_patch) {
    if (fps < 10 || fps > 60)
        return;
    if (!gc_bridge_enabled()) {
        printf("IdleMenuFPS needs ControllerBridge (only through it is a controller's input seen), so it stays off\n");
        return;
    }
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no IdleMenuFPS\n");
        return;
    }
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    saver_user(&user_battery.idle_menu_fps, fps);
    printf("limiting the frame rate to %i fps in a menu left untouched for %.0f s\n", fps, Idle_Menu_Seconds);
}

// from the main thread (app_focus.m)
static void background_changed(bool background) {
    atomic_store(&limits.in_background, background);
    display_sleep_tick();
}

void gpu_tweaks_background_fps_init(int fps, code_patch_fn checked_code_patch) {
    if (fps < 10 || fps > 240)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no BackgroundFPS\n");
        return;
    }
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    saver_user(&user_battery.background_fps, fps);
    app_focus_watch(background_changed);
    printf("limiting the frame rate to %i fps in the background\n", fps);
}

// IdleDisplaySleep (display_sleep.h): it needs the background (app_focus.m) and the power source (the 5 s watch) kept,
// as BackgroundFPS and BatteryFPS do; no hook
static void idle_display_sleep_watch(int mode) {
    if (mode != Display_Sleep_Off)
        app_focus_watch(background_changed);
    if (mode == Display_Sleep_On_Battery)
        start_power_watch();
}

void gpu_tweaks_idle_display_sleep_init(int mode) {
    if (mode < 0 || mode > 2)
        mode = 0;
    display_sleep_init(mode);
    idle_display_sleep_watch(mode);
    saver_user(&user_battery.idle_display_sleep, mode);
}

bool gpu_tweaks_set_idle_display_sleep(int mode) {
    if (mode < 0 || mode > 2)
        return false;
    idle_display_sleep_watch(mode);
    saver_user(&user_battery.idle_display_sleep, mode);
    display_sleep_tick();
    printf(mode == 0 ? "display sleep: the game keeps the display on, as it always did\n"
                     : "display sleep: the display may turn off while the game is paused, in the menus or in the "
                       "background%s\n",
           mode == 1 ? ", on battery" : "");
    saver_note("IdleDisplaySleep", mode, saver_on_battery().idle_display_sleep);
    return true;
}

// BatterySaver: what its levels need besides the values: present hooked (the limits, the resolution), the power watch,
// the background watch (BackgroundFPS, IdleDisplaySleep)
static void saver_say(int level) {
    battery_settings_t l = battery_saver_level(level);
    if (level == Battery_Saver_Off)
        printf("battery saver off: the battery settings as you set them\n");
    else
        printf("battery saver %s on battery: %i fps at most, the 3D scene at %i%% at most, %i fps in an untouched menu, "
               "%i behind another app, the display may sleep when idle (where your own settings save more, they "
               "stay; the menu and display parts with ControllerBridge only); plugged in, your own settings (%s now)\n",
               level == Battery_Saver_Saver ? "Saver" : "Longest", l.battery_fps, l.battery_resolution,
               l.idle_menu_fps, l.background_fps, atomic_load(&limits.on_battery) ? "on battery" : "plugged in");
    // the player's own values were said before the level was known (at the start the five settings come first)
    os_unfair_lock_lock(&user_battery_lock);
    battery_settings_t u = user_battery;
    os_unfair_lock_unlock(&user_battery_lock);
    battery_settings_t e = saver_on_battery();
    saver_note("BatteryFPS", u.battery_fps, e.battery_fps);
    saver_note("BatteryResolution", u.battery_resolution, e.battery_resolution);
    saver_note("IdleMenuFPS", u.idle_menu_fps, e.idle_menu_fps);
    saver_note("BackgroundFPS", u.background_fps, e.background_fps);
    saver_note("IdleDisplaySleep", u.idle_display_sleep, e.idle_display_sleep);
}

void gpu_tweaks_battery_saver_init(int level, code_patch_fn checked_code_patch) {
    if (level < Battery_Saver_Saver || level > Battery_Saver_Longest)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no BatterySaver\n");
        return;
    }
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    start_power_watch();
    idle_display_sleep_watch(Display_Sleep_On_Battery); // the background watch and the power watch
    atomic_store(&saver_level, level);
    saver_apply();
    saver_say(level);
}

bool gpu_tweaks_set_battery_saver(int level) {
    if (level < Battery_Saver_Off || level > Battery_Saver_Longest)
        return false;
    if (level != Battery_Saver_Off && !present_live())
        return false;
    if (level != Battery_Saver_Off) {
        sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
        start_power_watch();
        idle_display_sleep_watch(Display_Sleep_On_Battery);
    }
    atomic_store(&saver_level, level);
    saver_apply();
    display_sleep_tick();
    saver_say(level);
    return true;
}

void gpu_tweaks_metal_hud_init(int enabled) {
    const char *env = getenv("BO3MACFIX_METALHUD");
    if (env != NULL)
        enabled = atoi(env);
    // Metal reads this when the game creates its device, which is after we start
    if (enabled) {
        setenv("MTL_HUD_ENABLED", "1", 1);
        printf("Metal performance HUD on\n");
    }
}

#pragma mark - changes from the settings menu

// whether present goes through present_drawable already, or can be made to without writing the game's code (the
// pre-patched executable calls it through a slot)
bool gpu_tweaks_live_possible(void) {
    return present_redirected == 1 || prepatch_active();
}

// present going through present_drawable now, for a change from the menu. While the game runs only the pre-patched
// executable's slot may be set: patching the call in memory now would write code the render thread is running.
static bool present_live(void) {
    if (present_redirected == 1)
        return true;
    if (present_redirected == 0 || !prepatch_redirect_call(ADDR_ASL_Present_PresentDrawableCall, (void *)present_drawable))
        return false;
    present_redirected = 1;
    return true;
}

bool gpu_tweaks_set_pacing(int fps) {
    if (fps != 0 && (fps < 30 || fps > 240))
        return false;
    if (fps != 0 && !present_live())
        return false;
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    atomic_store(&limits.pacing_fps, fps);
    printf(fps != 0 ? "pacing frames at %i fps\n" : "not pacing frames\n", fps);
    return true;
}

bool gpu_tweaks_set_battery_fps(int fps) {
    if (fps != 0 && (fps < 30 || fps > 240))
        return false;
    if (fps != 0 && !present_live())
        return false;
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    if (fps != 0)
        start_power_watch();
    saver_user(&user_battery.battery_fps, fps);
    if (fps != 0)
        printf("limiting the frame rate to %i fps on battery (%s now)\n", fps, atomic_load(&limits.on_battery) ? "on battery" : "plugged in");
    else
        printf("not limiting the frame rate on battery\n");
    saver_note("BatteryFPS", fps, saver_on_battery().battery_fps);
    return true;
}

bool gpu_tweaks_set_battery_resolution(int setting) {
    if (setting != 0 && (setting < 50 || setting > 95))
        return false;
    if (setting != 0 && !present_live())
        return false;
    if (setting != 0)
        start_power_watch();
    saver_user(&user_battery.battery_resolution, setting);
    if (setting != 0)
        printf("the 3D scene at %i%% of ResolutionPercent at most on battery (%s now)\n", setting,
               atomic_load(&limits.on_battery) ? "on battery" : "plugged in");
    else
        printf("the 3D scene at the full Render Resolution on battery too\n");
    saver_note("BatteryResolution", setting, saver_on_battery().battery_resolution);
    return true;
}

bool gpu_tweaks_set_low_power_fps(int fps) {
    if (fps != 0 && (fps < 30 || fps > 240))
        return false;
    if (fps != 0 && !present_live())
        return false;
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    if (fps != 0)
        start_power_watch();
    atomic_store(&limits.low_power_fps, fps);
    if (fps != 0)
        printf("limiting the frame rate to %i fps in Low Power Mode (%s now)\n", fps,
               atomic_load(&limits.low_power) ? "on" : "off");
    else
        printf("not limiting the frame rate in Low Power Mode\n");
    return true;
}

bool gpu_tweaks_set_pause_menu_fps(int fps) {
    if (fps != 0 && (fps < 30 || fps > 240))
        return false;
    if (fps != 0 && !present_live())
        return false;
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    atomic_store(&limits.pause_menu_fps, fps);
    printf(fps != 0 ? "limiting the frame rate to %i fps in the pause menu\n" : "not limiting the frame rate in the pause menu\n", fps);
    return true;
}

bool gpu_tweaks_set_menu_fps(int fps) {
    if (fps != 0 && (fps < 30 || fps > 240))
        return false;
    if (fps != 0 && !present_live())
        return false;
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    atomic_store(&limits.menu_fps, fps);
    printf(fps != 0 ? "limiting the frame rate to %i fps in the menus\n" : "not limiting the frame rate in the menus\n", fps);
    return true;
}

bool gpu_tweaks_set_idle_menu_fps(int fps) {
    if (fps != 0 && (fps < 10 || fps > 60))
        return false;
    if (fps != 0 && !gc_bridge_enabled()) {
        printf("IdleMenuFPS needs ControllerBridge (only through it is a controller's input seen), so it stays off\n");
        return true; // saved; nothing to apply
    }
    if (fps != 0 && !present_live())
        return false;
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    saver_user(&user_battery.idle_menu_fps, fps);
    printf(fps != 0 ? "limiting the frame rate to %i fps in a menu left untouched for 10 s\n"
                    : "not limiting the frame rate in a menu left untouched\n", fps);
    saver_note("IdleMenuFPS", fps, saver_on_battery().idle_menu_fps);
    return true;
}

bool gpu_tweaks_set_background_fps(int fps) {
    if (fps != 0 && (fps < 10 || fps > 240))
        return false;
    if (fps != 0 && !present_live())
        return false;
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    if (fps != 0)
        app_focus_watch(background_changed);
    saver_user(&user_battery.background_fps, fps);
    printf(fps != 0 ? "limiting the frame rate to %i fps in the background\n" : "not limiting the frame rate in the background\n", fps);
    saver_note("BackgroundFPS", fps, saver_on_battery().background_fps);
    return true;
}

bool gpu_tweaks_set_frame_stats(int seconds) {
    if (seconds < 0)
        return false;
    if (seconds > 0 && !present_live())
        return false;
    if (seconds > 0 && frame_stats_seconds == 0)
        atomic_fetch_add(&frame_stats_session, 1);
    frame_stats_seconds = seconds;
    printf(seconds > 0 ? "logging frame stats every %i s\n" : "not logging frame stats\n", seconds);
    return true;
}

bool gpu_tweaks_set_dynamic_resolution(int setting) {
    if (setting != 0 && (setting < 50 || setting > 95))
        return false;
    if (setting != 0 && !present_live())
        return false;
    atomic_store(&dynres_setting, setting);
    // the render thread gives the user's value back and starts again from it with the new floor
    atomic_store(&dynres_reconfigure, true);
    printf(setting != 0 ? "dynamic resolution on, down to %i%% of ResolutionPercent\n" : "dynamic resolution off\n", setting);
    return true;
}
