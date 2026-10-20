// Following display changes, the engine's side (tier 2 in wip/display/PLAN.md).
//
// The port shows the engine one monitor, SDL display 0, and sdl_displays.c makes that the display the game should be on.
// The engine reads that monitor (its mode list, and the size its fullscreen aspect ratio comes from) once, in
// R_InitHardware. So when display 0 changes, this reads them again on the engine's Main thread, at the point where the
// engine applies video changes itself (R_CheckVidRestart, once a frame), sets an exact mode of the new display, and lets the
// engine's own code apply it. At launch, R_SetWndParms sets the mode for the display the game starts on.
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dobby.h"
#include "offsets.h"
#include "engine_display.h"
#include "notch.h"
#include "sdl_displays.h"

extern uint64_t game_base_address;

// DXGI_MODE_DESC
typedef struct {
    uint32_t width, height;
    uint32_t refresh_numerator, refresh_denominator;
    uint32_t format, scanline_ordering, scaling;
} ModeDesc;

#define Max_Monitors 8

// the engine's monitor table (ADDR_R_MonitorTable), filled by R_EnumDisplayModes
typedef struct {
    uint32_t count;
    uint32_t mode_counts[Max_Monitors];
    ModeDesc *modes[Max_Monitors]; // from new[]
} MonitorTable;
_Static_assert(offsetof(MonitorTable, modes) == 0x28, "the modes are at 0x113c0e20");

// ADDR_R_MonitorSize
typedef struct {
    bool valid;
    int width, height;
} MonitorSize;

#define SwapChain_Window 0xc0 // ASL_DXGISwapChain's ASL window, the one SetFullscreenState moves (0x1000e956f)

// the engine hashes dvar names: h = 0x4b9ace2f, then h = (h ^ tolower(c)) * 0x1000193 for each character and the NUL
#define Hash_r_fullscreen 0xbfdb056b
#define Hash_r_mode 0x236af761
#define Hash_r_refreshRate 0x3727533b
#define Hash_r_monitor 0x5fe4031a
#define Hash_r_monitorCount 0x69cbe7a7
#define Hash_com_maxfps 0x29b732e8

#define Mode_CritSec 0x33 // the critical section R_SetWndParms reads r_mode in (the window thread writes it on WM_SIZE)
#define Highest_Refresh 240 // r_refreshRate's maximum

typedef struct dvar_s dvar_t;
typedef int32_t HRESULT;

static dvar_t *(*Dvar_FindByHash)(uint32_t hash);
static int (*Dvar_GetInt)(const dvar_t *dvar);
static float (*Dvar_GetFloat)(const dvar_t *dvar);
static const char *(*Dvar_GetString)(const dvar_t *dvar);
static void (*Dvar_SetFromString)(dvar_t *dvar, const char *value, int source);
static void (*Dvar_SetInt)(dvar_t *dvar, int value);
static bool (*R_EnumDisplayModes)(void *adapter);
static void (*R_EnterFullscreen)(void);
static void (*R_CheckVideoDvarsModified)(void);
static void (*CritSec_ScopedLock)(void *lock, int critical_section, int mode);
static void (*CritSec_ScopedUnlock)(void *lock);
static void (*ASL_Window_UpdateLetterbox)(void *window);
static void (*ASL_Window_UpdateClientRect)(void *window);

// the game allocates the mode lists with new[]
extern void cxx_delete_array(void *pointer) __asm__("__ZdaPv");

static struct {
    dvar_t *fullscreen, *mode, *refresh, *monitor, *monitor_count, *max_fps;
} dvars;
static atomic_bool dvars_found; // set once, after dvars is filled (engine thread)

// the newest request; the engine thread takes it on its next frame
static os_unfair_lock request_lock = OS_UNFAIR_LOCK_INIT;
static DisplayDecision request;    // under request_lock
static unsigned request_taken = 0; // the generation the engine thread took last, under request_lock
static _Atomic unsigned request_generation = 0;

#pragma mark - COM

#define COM_METHOD(object, slot, type) ((type)((*(void ***)(object))[slot]))

static void com_release(void *object) {
    if (object != NULL)
        COM_METHOD(object, 2, uint32_t (*)(void *))(object);
}

// the adapter the engine's device came from, as R_EnterFullscreen gets it; release it after use
static void *engine_adapter(void) {
    void *device = *(void **)(game_base_address + ADDR_g_d3d11Device);
    void *dxgi_device = NULL, *adapter = NULL;
    if (device == NULL)
        return NULL;
    HRESULT hr = COM_METHOD(device, 0, HRESULT (*)(void *, const void *, void **))(
        device, (void *)(game_base_address + ADDR_IID_IDXGIDevice), &dxgi_device); // QueryInterface
    if (hr < 0 || dxgi_device == NULL)
        return NULL;
    hr = COM_METHOD(dxgi_device, 6, HRESULT (*)(void *, const void *, void **))(
        dxgi_device, (void *)(game_base_address + ADDR_IID_IDXGIAdapter), &adapter); // GetParent
    com_release(dxgi_device);
    return hr < 0 ? NULL : adapter;
}

// Display 0's size in points. That's what R_InitHardware's GetMonitorInfo gives at launch, before there's a fullscreen
// window: once there is one, ASL's display rect (0x10006f536) is the window's logical size, the old resolution.
static bool display_size(int *width, int *height) {
    uint32_t display = sdl_displays_id(0);
    if (display == 0)
        return false;
    CGRect bounds = CGDisplayBounds(display);
    *width = (int)bounds.size.width;
    *height = (int)bounds.size.height;
    return *width > 0 && *height > 0;
}

#pragma mark - dvars

static bool find_dvars(void) {
    if (atomic_load(&dvars_found))
        return true;
    dvars.fullscreen = Dvar_FindByHash(Hash_r_fullscreen);
    dvars.mode = Dvar_FindByHash(Hash_r_mode);
    dvars.refresh = Dvar_FindByHash(Hash_r_refreshRate);
    dvars.monitor = Dvar_FindByHash(Hash_r_monitor);
    dvars.monitor_count = Dvar_FindByHash(Hash_r_monitorCount);
    dvars.max_fps = Dvar_FindByHash(Hash_com_maxfps);
    if (dvars.fullscreen == NULL || dvars.mode == NULL || dvars.refresh == NULL || dvars.monitor == NULL ||
        dvars.monitor_count == NULL || dvars.max_fps == NULL) {
        printf("engine: the game's video dvars aren't there\n");
        return false;
    }
    atomic_store(&dvars_found, true);
    return true;
}

static bool current_mode(int *width, int *height) {
    uint64_t lock[2] = { 0 };
    CritSec_ScopedLock(lock, Mode_CritSec, 0);
    const char *text = Dvar_GetString(dvars.mode);
    bool ok = text != NULL && sscanf(text, "%dx%d", width, height) == 2;
    CritSec_ScopedUnlock(lock);
    return ok;
}

// source 1 is what Engine.ApplyHardwareProfileSettings uses: the video dvars only get a latched value, which the engine
// applies on its next video restart
static void set_mode(int width, int height) {
    char value[32];
    snprintf(value, sizeof(value), "%dx%d", width, height);
    uint64_t lock[2] = { 0 };
    CritSec_ScopedLock(lock, Mode_CritSec, 0);
    Dvar_SetFromString(dvars.mode, value, 1);
    CritSec_ScopedUnlock(lock);
}

static void set_dvar_number(dvar_t *dvar, double value) {
    char text[32];
    snprintf(text, sizeof(text), "%g", value);
    Dvar_SetFromString(dvar, text, 1);
}

#pragma mark - modes

static float mode_refresh(const ModeDesc *mode) {
    return mode->refresh_denominator != 0 ? (float)mode->refresh_numerator / (float)mode->refresh_denominator : 0.0f;
}

// width, height, then refresh rate, like the engine's R_ModeCompare (0xdcf36b)
static int compare_modes(const void *a, const void *b) {
    const ModeDesc *x = a, *y = b;
    if (x->width != y->width)
        return x->width < y->width ? -1 : 1;
    if (x->height != y->height)
        return x->height < y->height ? -1 : 1;
    float rx = mode_refresh(x), ry = mode_refresh(y);
    return rx < ry ? -1 : rx > ry ? 1 : 0;
}

// What R_InitHardware does to each list after R_EnumDisplayModes (0x100dcd89f-0x100dcdb35): drop the modes smaller than
// 800x600, sort them, and keep one of each size and refresh rate (preferring unspecified scaling)
static uint32_t filter_modes(ModeDesc *modes, uint32_t count) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++)
        if (modes[i].width >= 800 && modes[i].height >= 600)
            modes[kept++] = modes[i];
    qsort(modes, kept, sizeof(ModeDesc), compare_modes);
    uint32_t unique = 0;
    for (uint32_t i = 0; i < kept; i++) {
        ModeDesc *last = unique > 0 ? &modes[unique - 1] : NULL;
        if (last != NULL && last->width == modes[i].width && last->height == modes[i].height &&
            mode_refresh(last) == mode_refresh(&modes[i])) {
            if (last->scaling != 0 && modes[i].scaling == 0)
                *last = modes[i];
            continue;
        }
        modes[unique++] = modes[i];
    }
    return unique;
}

static void free_lists(ModeDesc **lists, uint32_t from) {
    for (uint32_t i = from; i < Max_Monitors; i++) {
        if (lists[i] != NULL)
            cxx_delete_array(lists[i]);
        lists[i] = NULL;
    }
}

// Fills the engine's monitor table again from display 0, as R_InitHardware does at launch. The old table stays if the new
// one has no modes.
static bool rebuild_monitor_table(void *adapter) {
    MonitorTable *table = (MonitorTable *)(game_base_address + ADDR_R_MonitorTable);
    MonitorTable old = *table;
    memset(table, 0, sizeof(*table));
    R_EnumDisplayModes(adapter);
    uint32_t count = table->count < Max_Monitors ? table->count : Max_Monitors;
    // like R_InitHardware, the monitors end at the first one without modes
    for (uint32_t i = 0; i < count; i++) {
        table->mode_counts[i] = table->modes[i] != NULL ? filter_modes(table->modes[i], table->mode_counts[i]) : 0;
        if (table->mode_counts[i] == 0) {
            count = i;
            break;
        }
    }
    if (count == 0) {
        free_lists(table->modes, 0);
        *table = old;
        return false;
    }
    free_lists(table->modes, count);
    for (uint32_t i = count; i < Max_Monitors; i++)
        table->mode_counts[i] = 0;
    table->count = count;
    free_lists(old.modes, 0);
    Dvar_SetInt(dvars.monitor_count, (int)count);
    return true;
}

static const ModeDesc *exact_size(const MonitorTable *table, int width, int height) {
    for (uint32_t i = 0; i < table->mode_counts[0]; i++)
        if ((int)table->modes[0][i].width == width && (int)table->modes[0][i].height == height)
            return &table->modes[0][i];
    return NULL;
}

// The size of display 0 nearest to width x height: that size if the display has it, else the nearest at its aspect ratio
// (among all if none is). An exact match comes first because ASL adds 16:9 sizes (1280x720, 1344x756) to every list.
static const ModeDesc *nearest_size(const MonitorTable *table, int width, int height, double aspect) {
    const ModeDesc *best = exact_size(table, width, height);
    long best_distance = 0;
    for (int pass = 0; pass < 2 && best == NULL; pass++) {
        for (uint32_t i = 0; i < table->mode_counts[0]; i++) {
            const ModeDesc *mode = &table->modes[0][i];
            if (pass == 0 && (aspect <= 0 || fabs((double)mode->width / mode->height - aspect) > aspect * 0.01))
                continue;
            long distance = labs((long)mode->width - width) + labs((long)mode->height - height);
            if (best == NULL || distance < best_distance) {
                best = mode;
                best_distance = distance;
            }
        }
    }
    return best;
}

// the refresh rate of display 0 nearest to refresh at the mode's size
static const ModeDesc *nearest_refresh(const MonitorTable *table, const ModeDesc *size, int refresh) {
    const ModeDesc *best = NULL;
    float best_distance = 0;
    for (uint32_t i = 0; i < table->mode_counts[0]; i++) {
        const ModeDesc *mode = &table->modes[0][i];
        float hz = mode_refresh(mode);
        if (mode->width != size->width || mode->height != size->height || hz < 1 || hz > Highest_Refresh + 0.5f)
            continue;
        float distance = fabsf(hz - (float)refresh);
        if (best == NULL || distance < best_distance) {
            best = mode;
            best_distance = distance;
        }
    }
    return best;
}

// Sets r_mode (an exact entry of display 0's list), r_refreshRate and com_maxfps for a decision. They're applied by the
// engine's next video restart (launch: R_SetWndParms, which makes latched values current). A window keeps the size the
// user gave it. Returns whether r_mode changed.
static bool set_video(const char *when, const DisplayDecision *d, double aspect) {
    const MonitorTable *table = (const MonitorTable *)(game_base_address + ADDR_R_MonitorTable);
    if (table->count == 0 || table->mode_counts[0] == 0 || table->modes[0] == NULL)
        return false;
    int old_width = 0, old_height = 0;
    current_mode(&old_width, &old_height);
    float old_refresh = Dvar_GetFloat(dvars.refresh);
    int old_max_fps = Dvar_GetInt(dvars.max_fps);
    bool windowed = Dvar_GetInt(dvars.fullscreen) == 0;

    const ModeDesc *size;
    if (windowed)
        size = exact_size(table, old_width, old_height); // only for its refresh rates
    else if (d->width > 0 && d->height > 0)
        size = nearest_size(table, d->width, d->height, aspect);
    else
        size = nearest_size(table, old_width, old_height, aspect);
    bool mode_changed = !windowed && size != NULL && ((int)size->width != old_width || (int)size->height != old_height);
    if (mode_changed)
        set_mode((int)size->width, (int)size->height);

    float refresh = old_refresh;
    const ModeDesc *rate = d->refresh > 0 && size != NULL ? nearest_refresh(table, size, d->refresh) : NULL;
    if (rate != NULL && fabsf(mode_refresh(rate) - old_refresh) > 0.01f) {
        refresh = mode_refresh(rate);
        set_dvar_number(dvars.refresh, refresh);
    }
    if (d->max_fps >= 0 && d->max_fps != old_max_fps)
        set_dvar_number(dvars.max_fps, d->max_fps);
    printf("engine: %s: WindowSize %dx%d -> %dx%d%s, RefreshRate %g -> %g, MaxFPS %d -> %d (Monitor %d)\n", when,
           old_width, old_height, mode_changed ? (int)size->width : old_width, mode_changed ? (int)size->height : old_height,
           windowed ? " (windowed)" : "", old_refresh, refresh, old_max_fps, d->max_fps >= 0 ? d->max_fps : old_max_fps,
           Dvar_GetInt(dvars.monitor));
    return mode_changed;
}

#pragma mark - the ASL window

// the game's ASL window: the swap chain's, if it's in ASL's window list (SetFullscreenState makes the same check)
static void *game_asl_window(void) {
    char *swap_chain = *(char **)(game_base_address + ADDR_R_SwapChain);
    void *window = swap_chain != NULL ? *(void **)(swap_chain + SwapChain_Window) : NULL;
    void **list = *(void ***)(game_base_address + ADDR_ASL_WindowList);
    void **end = *(void ***)(game_base_address + ADDR_ASL_WindowList + 8);
    for (void **entry = list; window != NULL && entry != NULL && entry < end; entry++)
        if (*entry == window)
            return window;
    return NULL;
}

// ASL's Window_SetMode returns early when the fullscreen state, the size and the display index are all unchanged, so a
// move that keeps the resolution doesn't redo what it ends with (0x1000edc58): the letterbox (which the menu mouse mapping
// uses) and the client rect (and the forced drawable). Do those two, like it does, with ASL's window mutex held. AvoidNotch
// (notch.m) has them done again when the camera housing it keeps the picture below changes. False if there's no window.
static bool update_asl_window(void) {
    pthread_mutex_t *mutex = (pthread_mutex_t *)(game_base_address + ADDR_ASL_WindowMutex); // a std::recursive_mutex
    pthread_mutex_lock(mutex);
    void *window = game_asl_window();
    if (window != NULL) {
        ASL_Window_UpdateLetterbox(window);
        ASL_Window_UpdateClientRect(window);
    }
    pthread_mutex_unlock(mutex);
    return window != NULL;
}

#pragma mark - hooks

static bool take_request(DisplayDecision *out) {
    if (atomic_load_explicit(&request_generation, memory_order_acquire) == request_taken)
        return false;
    os_unfair_lock_lock(&request_lock);
    *out = request;
    request_taken = atomic_load(&request_generation);
    os_unfair_lock_unlock(&request_lock);
    return true;
}

// Tier 2: display 0 changed (sdl_displays_refresh ran on the main thread), so read the monitor again, set the game's
// resolution for it and move the fullscreen window. R_CheckVidRestart applies the rest in this frame.
static bool apply_request(const DisplayDecision *d) {
    printf("engine: display change (%s)\n", d->why);
    if (!find_dvars())
        return false;
    if (d->display != 0 && sdl_displays_id(0) != d->display) {
        printf("engine: display 0 is %u, not %u, ignoring the change\n", sdl_displays_id(0), d->display);
        return false;
    }
    display_follow_expect_changes(1.5);

    bool table_ok = false;
    double aspect = 0;
    void *adapter = NULL;
    if (sdl_displays_rebuild_mode_table() != 1) {
        printf("engine: ASL's mode table wasn't rebuilt, keeping the monitor table and the resolution\n");
    } else if ((adapter = engine_adapter()) == NULL) {
        printf("engine: no DXGI adapter, keeping the monitor table and the resolution\n");
    } else if (!(table_ok = rebuild_monitor_table(adapter))) {
        printf("engine: the new display has no modes the game can use, keeping the old monitor table and the resolution\n");
    } else {
        int width, height;
        MonitorSize *size = (MonitorSize *)(game_base_address + ADDR_R_MonitorSize);
        const MonitorTable *table = (const MonitorTable *)(game_base_address + ADDR_R_MonitorTable);
        if (display_size(&width, &height)) {
            size->width = width;
            size->height = height;
            size->valid = true;
            aspect = (double)width / height;
            printf("engine: display 0 has %u modes, it's %dx%d points\n", table->mode_counts[0], width, height);
        } else {
            printf("engine: display 0 has %u modes, its size is unknown, the aspect ratio may be off\n", table->mode_counts[0]);
        }
    }
    com_release(adapter);

    bool mode_changed = false;
    if (table_ok) {
        mode_changed = set_video("display change", d, aspect);
    } else if (d->max_fps >= 0) {
        set_dvar_number(dvars.max_fps, d->max_fps);
    }

    // R_EnterFullscreen only sends SetFullscreenState(TRUE, display 0's output) to the window thread, where ASL's
    // Window_SetMode re-homes the window. Unlike R_CheckVidRestart's monitor change, it doesn't leave fullscreen first (no
    // windowed frame) or Sleep(1000). A new resolution reaches Window_SetMode again through the restart's ResizeBuffers.
    if (d->moved && Dvar_GetInt(dvars.fullscreen) != 0 && *(void **)(game_base_address + ADDR_R_SwapChain) != NULL) {
        printf("engine: entering fullscreen again on the new display\n");
        R_EnterFullscreen();
        if (!mode_changed && !update_asl_window())
            printf("engine: no ASL window to update for the new display\n");
    }

    R_CheckVideoDvarsModified();
    return true;
}

// The window size config.ini gave and the smaller one the launch made it to fit its display. The engine saves config.ini
// at quit from the dvars' current values, so the fitted size would stay, also on a bigger display later.
static int chosen_width, chosen_height, fitted_width, fitted_height;
static atomic_bool fit_kept; // the window has the fitted size, as far as we know: no restart from the options since

// Alt-tabbed out of fullscreen the engine is windowed (r_fullscreen 0, with the restore flag set) until it's active
// again, when the restore sets r_fullscreen 1 (latched, so it still reads 0) and restarts at r_mode. A display change
// applied meanwhile is applied as to a window: no new resolution, no fullscreen on the new display, and the old
// display's resolution comes back there. So it waits until the engine is fullscreen again, or windowed with nothing
// pending (the restore is off).
static bool wait_for_fullscreen(void) {
    return Dvar_GetInt(dvars.fullscreen) == 0 && (*(volatile bool *)(game_base_address + ADDR_R_RestoreFullscreen) ||
                                                  *(volatile bool *)(game_base_address + ADDR_R_VidRestartPending));
}

// Once a frame on the engine's Main thread: applies pending video changes (the options' vid_restart, alt-tab)
install_hook_name(R_CheckVidRestart, void, void) {
    DisplayDecision decision;
    int width, height;
    static bool told_waiting;
    if (notch_take_refresh())
        update_asl_window();
    bool pending = atomic_load_explicit(&request_generation, memory_order_acquire) != request_taken;
    bool waiting = pending && find_dvars() && wait_for_fullscreen();
    if (waiting && !told_waiting)
        printf("engine: alt-tabbed out of fullscreen, the display change waits until the game is fullscreen again\n");
    told_waiting = waiting;
    if (!waiting && take_request(&decision)) {
        bool applied = apply_request(&decision);
        orig_R_CheckVidRestart();
        if (applied && current_mode(&width, &height))
            printf("engine: the game runs at %dx%d, %g Hz\n", width, height, Dvar_GetFloat(dvars.refresh));
        return;
    }
    // someone else's restart, usually the game's options: remember what it applied for this display. Not alt-tab's (it
    // leaves fullscreen and comes back, and a window's WM_SIZE can rewrite r_mode meanwhile).
    bool note = *(volatile bool *)(game_base_address + ADDR_R_VidRestartPending) &&
        !*(volatile bool *)(game_base_address + ADDR_R_RestoreFullscreen) && find_dvars();
    int fullscreen = note ? Dvar_GetInt(dvars.fullscreen) : 0, mode_width = 0, mode_height = 0;
    bool mode_known = note && current_mode(&mode_width, &mode_height);
    orig_R_CheckVidRestart();
    // a window size or fullscreen the options applied is the user's. Their other video settings (anti-aliasing, the
    // refresh rate, ...) restart too, but leave the window the size the launch fitted it to.
    if (note && (!mode_known || !current_mode(&width, &height) || width != mode_width || height != mode_height ||
                 Dvar_GetInt(dvars.fullscreen) != fullscreen))
        atomic_store(&fit_kept, false);
    if (note && fullscreen != 0 && Dvar_GetInt(dvars.fullscreen) == fullscreen && current_mode(&width, &height))
        display_follow_note_choice(width, height, (int)lroundf(Dvar_GetFloat(dvars.refresh)));
}

static bool launch_checked = false;

// At launch R_InitHardware calls this again once the monitor table is filled, before it makes the window. Set the mode
// for the display the game starts on there, in memory: the original makes the latched values current.
install_hook_name(R_SetWndParms, void, void *parms) {
    const MonitorTable *table = (const MonitorTable *)(game_base_address + ADDR_R_MonitorTable);
    if (!launch_checked && table->count > 0 && find_dvars()) {
        launch_checked = true;
        // the current values go in (vidConfig is still empty here), the decision comes back in place of them
        DisplayDecision d = { .refresh = (int)lroundf(Dvar_GetFloat(dvars.refresh)), .max_fps = Dvar_GetInt(dvars.max_fps) };
        current_mode(&d.width, &d.height);
        if (display_follow_launch_choice(&d)) {
            int width = 0, height = 0;
            set_video("launch", &d, display_size(&width, &height) ? (double)width / height : 0);
        } else {
            printf("engine: launch: keeping config.ini's video settings (Monitor %d)\n", Dvar_GetInt(dvars.monitor));
        }
        int width = 0, height = 0;
        if (Dvar_GetInt(dvars.fullscreen) == 0 && current_mode(&chosen_width, &chosen_height)) {
            width = chosen_width;
            height = chosen_height;
            if (display_follow_launch_window(&width, &height)) {
                set_mode(width, height);
                fitted_width = width;
                fitted_height = height;
                atomic_store(&fit_kept, true);
            }
        }
    }
    orig_R_SetWndParms(parms);
}

#define HWProfile_Count 58
#define HWProfile_Value_Size 0x40

typedef struct {
    const char *key;
    uint32_t dvar_hash;
    const char *comment;
} HWProfileKey;
_Static_assert(sizeof(HWProfileKey) == 0x18, "HWProfileKey must match the game's layout");

// At quit the engine copies the dvars into the hardware profile here, then writes it to config.ini. If the window still
// has the size the launch fitted it to (not one the user resized it to or picked in the options), save the size
// config.ini had instead. Only at quit: the options read the profile too, and must see the window's real size. Only
// windowed, as the launch fit was: fullscreen, WindowSize is the resolution the game runs at.
install_hook_name(SyncHardwareProfileWithDvars, void, bool make_latched_current) {
    orig_SyncHardwareProfileWithDvars(make_latched_current);
    int width = 0, height = 0;
    if (__builtin_return_address(0) != (void *)(game_base_address + ADDR_HWProfile_QuitSyncReturn) ||
        !atomic_load(&fit_kept) || !find_dvars() || Dvar_GetInt(dvars.fullscreen) != 0 ||
        !current_mode(&width, &height) || width != fitted_width || height != fitted_height)
        return;
    const HWProfileKey *keys = (const HWProfileKey *)(game_base_address + ADDR_HWProfile_Keys);
    for (int i = 0; i < HWProfile_Count; i++) {
        if (keys[i].dvar_hash != Hash_r_mode)
            continue;
        char *value = (char *)(game_base_address + ADDR_HWProfile_Values) + (size_t)i * HWProfile_Value_Size;
        snprintf(value, HWProfile_Value_Size, "%dx%d", chosen_width, chosen_height);
        printf("engine: quit: config.ini keeps WindowSize %s, not the %dx%d the window was fitted to at launch\n", value,
               width, height);
        return;
    }
}

#pragma mark - API

void engine_display_request(const DisplayDecision *d) {
    if (d == NULL)
        return;
    os_unfair_lock_lock(&request_lock);
    unsigned generation = atomic_load(&request_generation);
    // a request the engine thread hasn't taken yet may still have to move the window
    int owed_move = generation != request_taken && request.moved;
    request = *d;
    request.moved = request.moved || owed_move;
    atomic_store_explicit(&request_generation, generation + 1, memory_order_release);
    os_unfair_lock_unlock(&request_lock);
}

// The applied resolution and the two dvars are single words the engine thread writes, fine to read for a log line. Before
// the renderer fills vidConfig (CL_InitRenderer), the resolution comes from r_mode, read without its critical section.
void engine_display_describe(char *buf, size_t n) {
    if (buf == NULL || n == 0)
        return;
    int width = 0, height = 0, refresh = 0, max_fps = 0;
    if (game_base_address != 0) {
        const int *size = (const int *)(game_base_address + ADDR_vidConfig_DisplaySize);
        width = size[0];
        height = size[1];
    }
    if (atomic_load(&dvars_found)) {
        const char *mode = width == 0 || height == 0 ? Dvar_GetString(dvars.mode) : NULL;
        if (mode != NULL && sscanf(mode, "%dx%d", &width, &height) != 2)
            width = height = 0;
        refresh = (int)lroundf(Dvar_GetFloat(dvars.refresh));
        max_fps = Dvar_GetInt(dvars.max_fps);
    }
    snprintf(buf, n, "game=%dx%d refresh=%d maxfps=%d", width, height, refresh, max_fps);
}

// Alt-tab out of fullscreen sets r_fullscreen to 0 and the restore flag; R_CheckVidRestart stores the mode it left in
// r_fullscreen's "last" dvar. (The engine itself comes back in mode 1.)
int engine_display_fullscreen_mode(void) {
    if (game_base_address == 0 || Dvar_GetInt == NULL)
        return -1;
    const dvar_t *fullscreen = *(dvar_t *volatile *)(game_base_address + ADDR_dvar_r_fullscreen);
    if (fullscreen == NULL)
        return -1;
    int mode = Dvar_GetInt(fullscreen);
    if (mode == 0 && *(volatile bool *)(game_base_address + ADDR_R_RestoreFullscreen)) {
        const dvar_t *last = *(dvar_t *volatile *)(game_base_address + ADDR_dvar_r_fullscreenLast);
        mode = last != NULL ? Dvar_GetInt(last) : 0;
        if (mode != 1 && mode != 2)
            mode = 1;
    }
    return mode;
}

void engine_display_init(void) {
    uint64_t base = game_base_address;
    if (base == 0)
        return;
    Dvar_FindByHash = (void *)(base + ADDR_Dvar_FindByHash);
    Dvar_GetInt = (void *)(base + ADDR_Dvar_GetInt);
    Dvar_GetFloat = (void *)(base + ADDR_Dvar_GetFloat);
    Dvar_GetString = (void *)(base + ADDR_Dvar_GetString);
    Dvar_SetFromString = (void *)(base + ADDR_Dvar_SetFromString);
    Dvar_SetInt = (void *)(base + ADDR_Dvar_SetInt);
    R_EnumDisplayModes = (void *)(base + ADDR_R_EnumDisplayModes);
    R_EnterFullscreen = (void *)(base + ADDR_R_EnterFullscreen);
    R_CheckVideoDvarsModified = (void *)(base + ADDR_R_CheckVideoDvarsModified);
    CritSec_ScopedLock = (void *)(base + ADDR_CritSec_ScopedLock);
    CritSec_ScopedUnlock = (void *)(base + ADDR_CritSec_ScopedUnlock);
    ASL_Window_UpdateLetterbox = (void *)(base + ADDR_ASL_Window_UpdateLetterbox);
    ASL_Window_UpdateClientRect = (void *)(base + ADDR_ASL_Window_UpdateClientRect);
    install_hook_R_CheckVidRestart((void *)(base + ADDR_R_CheckVidRestart));
    install_hook_R_SetWndParms((void *)(base + ADDR_R_SetWndParms));
    install_hook_SyncHardwareProfileWithDvars((void *)(base + ADDR_SyncHardwareProfileWithDvars));
    printf("engine: the game's resolution and refresh rate follow its display\n");
}
