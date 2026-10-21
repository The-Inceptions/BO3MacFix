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
// - PauseMenuFPS: paces frames to that rate while the pause menu is open in a match (notices.lua says when), to save
//   power.
// - MenuFPS: the same in the menus outside a match (the main menu, the lobby, the mods menu; notices.lua says when).
// - BackgroundFPS: the same while the game isn't the active app, or is hidden or minimized (app_focus.m says when).
//   The lowest of these limits that applies wins, see frame_limit.c.
// - DynamicResolution: lowers the 3D scene's resolution (ResolutionPercent) in small steps while the GPU can't keep
//   up with the display's refresh rate, and raises it again when it can, see dynres.h.
// - MetalHUD: turns on macOS's Metal performance HUD.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <objc/runtime.h>
#include <objc/message.h>

#include "app_focus.h"
#include "dynres.h"
#include "frame_limit.h"
#include "gpu_tweaks.h"
#include "metalfx.h"
#include "prepatch.h"
#include "offsets.h"
#include "script_errors.h"
#include "gpu_diag.h"

extern uint64_t game_base_address;

static frame_limits_t limits; // the frame rate limits (frame_limit.h) and the situations some apply in
static SEL sel_present_after_minimum_duration;
static int metalfx_active = 0;
static int frame_stats_seconds = 0; // 0 when not logging frame stats
static int frame_stats_detail = 0; // BO3MACFIX_FRAMESTATSDETAIL=1: also the script errors thrown in each window
static atomic_uint frame_stats_session = 0; // goes up each time FrameStats is turned on from the menu

static int compare_floats(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
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
    char detail[160] = "";
    if (frame_stats_detail) {
        uint64_t buffers;
        double busy = gpu_diag_take_busy(&buffers);
        int n = snprintf(detail, sizeof(detail), ", GPU busy %.2f ms a frame (%.0f%%), %.1f command buffers a frame",
                         busy * 1000.0 / frames, busy * 100.0 / seconds, (double)buffers / frames);
        uint32_t server_errors, client_errors;
        if (script_errors_take_thrown(&server_errors, &client_errors) && n > 0 && n < (int)sizeof(detail))
            snprintf(detail + n, sizeof(detail) - n, ", script errors %u server, %u client", server_errors, client_errors);
    }
    printf("frame stats: %.1f fps, 1%% low %.1f fps, worst frame %.1f ms (%i frames in %.1f s%s)%s\n",
           frames / seconds, 1000.0f / low_1, frame_ms[stored - 1], frames, seconds, display, detail);
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
    if (fps >= display_fps || display_fps % fps == 0 || present_display_variable_refresh())
        return;
    int refreshes = (display_fps + fps - 1) / fps;
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

// DynamicResolution: 0 off, else the lowest share of ResolutionPercent it goes down to, in percent
static atomic_int dynres_setting = 0;
static dynres_t dynres; // the render thread's
// what DynamicResolution last put into r_sceneResolutionMultiplier, 0 while the dvar holds the user's own value;
// and the user's own value then, for config.ini (gpu_tweaks_dynres_held_base)
static atomic_int dynres_written = 0, dynres_base = 0;
static atomic_bool dynres_reconfigure = false; // the setting changed in the menu: start again from the user's value
// BO3MACFIX_DYNRESTEST="100,85,70" cycles through these every BO3MACFIX_DYNRESTESTSECONDS (8) instead, for testing
static int dynres_test[8], dynres_test_count = 0, dynres_test_seconds = 8;

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

// each frame: learn the sets that appear right after a step, and set the last use of those sizes' sets to the frame
// counter, so the engine's lookup doesn't free them after 11 frames. Only the last use is written, never a size or a
// render target: which sets are valid stays the engine's own (a set is in use while its size isn't 0). Returns how
// many sets appeared; *view_3d says whether the engine's lookup used a set in the last 2 frames (the 3D view renders),
// not counting the last uses this wrote itself.
static int dynres_keep_tick(bool *view_3d) {
    static uint64_t written_use[Dynres_Sets]; // the last use this wrote to each set, 0 for none
    uint32_t sizes[Dynres_Sets];
    uint64_t last_use[Dynres_Sets];
    int appeared = 0;
    *view_3d = false;
    if (!rt_sets_read(sizes, last_use))
        return 0;
    uint64_t frame = *(volatile uint64_t *)(game_base_address + ADDR_rt_frame_counter);
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
    unsigned mask = dynres_keep_mask(&dynres_keep, sizes, last_use, frame);
    volatile uint64_t *use = (volatile uint64_t *)(game_base_address + ADDR_rt_set_last_use);
    for (int i = 1; i < Dynres_Sets; i++) {
        if (mask & (1u << i))
            use[i] = frame;
        written_use[i] = mask & (1u << i) ? frame : 0;
    }
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
    int percent = dynres_prewarm_frame(&dynres_prewarm, dynres.base, dynres.lowest, ready, warmed || appeared > 0);
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

// the frame rate to reach: the display's refresh rate, or a lower limit (ours, or MaxFPS)
static int dynres_target_fps(int frame_limit_fps) {
    static void *maxfps = NULL;
    if (maxfps == NULL)
        maxfps = ((void *(*)(uint32_t))(game_base_address + ADDR_Dvar_FindByHash))(Hash_com_maxfps);
    int target = present_display_max_fps();
    if (frame_limit_fps > 0 && (target <= 0 || frame_limit_fps < target))
        target = frame_limit_fps;
    int max_fps = maxfps != NULL ? ((int (*)(void *))(game_base_address + ADDR_Dvar_GetInt))(maxfps) : 0;
    if (max_fps > 0 && (target <= 0 || max_fps < target))
        target = max_fps;
    return target;
}

// from the render thread, for each frame
static void dynres_tick(int frame_limit_fps) {
    static uint64_t window_start = 0, last_present = 0, test_start = 0;
    static int frames = 0, logging = 0, from = 0, to = 0, test_index = 0;
    static double busy_start = 0, window_fps = 0, window_busy = 0;
    static float after[4];
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    // the frames right after a change show what it cost: the engine allocates render targets for the new size
    if (logging > 0 && last_present != 0) {
        after[4 - logging] = (float)((now - last_present) / 1e6);
        if (--logging == 0)
            printf("dynamic resolution: %i%% -> %i%% (%.1f fps of %i, GPU busy %.0f%%), the next frames took %.1f, %.1f, "
                   "%.1f, %.1f ms\n", from, to, window_fps, dynres_target_fps(frame_limit_fps), window_busy * 100,
                   after[0], after[1], after[2], after[3]);
    }
    last_present = now;
    char *dvar = scene_multiplier_dvar();
    if (dvar == NULL)
        return;
    bool view_3d;
    int appeared = dynres_keep_tick(&view_3d);
    int current = *(volatile int *)(dvar + dvar_Value), latched = *(volatile int *)(dvar + dvar_Latched);
    int written = atomic_load(&dynres_written), setting = atomic_load(&dynres_setting);
    bool reconfigure = atomic_exchange(&dynres_reconfigure, false);
    if (written != 0 && (current != written || latched != written)) {
        // something else set ResolutionPercent (the options): that's the user's value from now on
        printf("dynamic resolution: ResolutionPercent was set to %i, going on from there\n", latched);
        atomic_store(&dynres_written, 0);
        written = 0;
        dynres.base = 0;
    } else if (written != 0 && (reconfigure || setting == 0)) {
        // turned off or changed in the menu: the user's value back first
        int base = atomic_load(&dynres_base);
        scene_multiplier_set(dvar, base);
        atomic_store(&dynres_written, 0);
        printf(setting == 0 ? "dynamic resolution off, back to %i%%\n" : "dynamic resolution: back to %i%% for the new setting\n", base);
        dynres.base = 0;
        return;
    }
    if (reconfigure)
        dynres.base = 0;
    if (written == 0 && (setting == 0 || current != latched)) // off, or a change the engine applies first
        return;
    if (written == 0 && dynres.base != current) {
        dynres_reset(&dynres, current, dynres_lowest_for(current, setting));
        atomic_store(&dynres_base, current);
        window_start = 0;
        printf("dynamic resolution: between %i%% and %i%%\n", dynres.lowest, dynres.base);
    }
    if (dynres_prewarm_tick(dvar, now, view_3d, appeared)) {
        window_start = 0; // the controller's first window starts after it
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
        percent = dynres_window(&dynres, window_fps, window_busy, dynres_target_fps(frame_limit_fps));
    }
    int value = written != 0 ? written : current;
    if (percent == value || logging > 0)
        return;
    from = value;
    to = percent;
    logging = 4;
    dynres_keep_step();
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
    if (frame_stats_detail || atomic_load(&dynres_setting) != 0 || atomic_load(&dynres_written) != 0)
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
    if (atomic_load(&dynres_setting) != 0 || atomic_load(&dynres_written) != 0)
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
    if (seconds <= 0)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no frame stats\n");
        return;
    }
    frame_stats_seconds = seconds;
    const char *detail = getenv("BO3MACFIX_FRAMESTATSDETAIL");
    frame_stats_detail = detail != NULL && atoi(detail) != 0;
    printf("logging frame stats every %i s\n", seconds);
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

// checks the power source now and then every 5 seconds (once, however often it's asked)
static void start_battery_watch(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        atomic_store(&limits.on_battery, running_on_battery());
        dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC);
        dispatch_source_set_event_handler(timer, ^{
            bool battery = running_on_battery();
            if (battery != atomic_exchange(&limits.on_battery, battery) && atomic_load(&limits.battery_fps) > 0)
                printf("%s, %s\n", battery ? "running on battery" : "plugged in", battery ? "limiting the frame rate" : "frame rate unlimited");
        });
        dispatch_resume(timer);
    });
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
    atomic_store(&limits.battery_fps, fps);
    start_battery_watch();
    printf("limiting the frame rate to %i fps on battery (%s now)\n", fps, atomic_load(&limits.on_battery) ? "on battery" : "plugged in");
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

void gpu_tweaks_pause_menu_open(bool open) {
    frame_limit_set_pause_menu(&limits, open, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
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
}

void gpu_tweaks_ui_gone(void) {
    frame_limit_ui_gone(&limits);
    atomic_store(&dynres_prewarm_arm, true); // a map loads: warm DynamicResolution's sizes up when its match starts
}

// from the main thread (app_focus.m)
static void background_changed(bool background) {
    atomic_store(&limits.in_background, background);
}

void gpu_tweaks_background_fps_init(int fps, code_patch_fn checked_code_patch) {
    if (fps < 10 || fps > 240)
        return;
    if (!redirect_present(checked_code_patch)) {
        printf("couldn't hook Aspyr's present, no BackgroundFPS\n");
        return;
    }
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    atomic_store(&limits.background_fps, fps);
    app_focus_watch(background_changed);
    printf("limiting the frame rate to %i fps in the background\n", fps);
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
        start_battery_watch();
    atomic_store(&limits.battery_fps, fps);
    if (fps != 0)
        printf("limiting the frame rate to %i fps on battery (%s now)\n", fps, atomic_load(&limits.on_battery) ? "on battery" : "plugged in");
    else
        printf("not limiting the frame rate on battery\n");
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

bool gpu_tweaks_set_background_fps(int fps) {
    if (fps != 0 && (fps < 10 || fps > 240))
        return false;
    if (fps != 0 && !present_live())
        return false;
    sel_present_after_minimum_duration = sel_registerName("presentDrawable:afterMinimumDuration:");
    if (fps != 0)
        app_focus_watch(background_changed);
    atomic_store(&limits.background_fps, fps);
    printf(fps != 0 ? "limiting the frame rate to %i fps in the background\n" : "not limiting the frame rate in the background\n", fps);
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
