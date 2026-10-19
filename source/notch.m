// AvoidNotch: keeps the game's picture below the camera housing of a MacBook's built-in display in fullscreen.
//
// The fullscreen window still covers the whole screen, so nothing shows through beside the housing. Aspyr's own
// letterbox, the one for a display with another aspect ratio, does the rest:
// - Window_UpdateLetterbox (0x1000ee14c) works out where the picture goes: a uniform scale, a pillar width and a
//   letterbox height. Window_ConvertCoords (0x1000ee866) maps the mouse with the same fields.
// - The Metal client rect (0x1000ee6c6) forces the drawable for that and puts the frame in it (the blit rect). The layer
//   stretches the drawable over the window, and present's clear pass leaves the bars black.
// Aspyr only centres the picture. With an inset, these two hooks put it in the part of the window below the housing
// instead (notch_layout.c), and metalfx.m scales that rect when it upscales.
//
// The main thread keeps the game window's screen's safeAreaInsets.top (macOS 12 and later). When that or the option
// changes, or the Metal view turns up after the letterbox was done without it, the engine thread does the letterbox and
// client rect again (engine_display.c's R_CheckVidRestart hook), so moving between the built-in and an external display
// turns it on and off. Off, or on a screen without a housing, both
// hooks leave Aspyr's numbers exactly as they are.
#import <AppKit/AppKit.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dobby.h"
#include "notch.h"
#include "notch_layout.h"
#include "offsets.h"
#include "prepatch.h"

extern uint64_t game_base_address;

// ASL's Window, as Window_UpdateLetterbox, Window_ConvertCoords and the Metal client rect use it
#define Window_SDL 0x00       // SDL_Window *
#define Window_ScaleX 0x08    // double: logical units per point
#define Window_ScaleY 0x10    // double
#define Window_LogicalW 0x18  // int: the size Window_SetMode was given (fullscreen: the desktop's, in points)
#define Window_LogicalH 0x1c  // int
#define Window_Pillar 0x20    // int: points left of the picture
#define Window_Letterbox 0x24 // int: points above it
#define Window_FrameW 0xb0    // int: the swap chain's buffer, 0 = the logical size
#define Window_FrameH 0xb4    // int
#define Window_DrawableW 0xb8 // int: the drawable the client rect forces
#define Window_DrawableH 0xbc // int
#define Window_Blit 0xc0      // int x, y, w, h: where present copies the frame into the drawable
#define Window_Flags 0xd0     // bit 0: fullscreen
_Static_assert(sizeof(NotchRect) == 16, "the blit rect is four ints");

static void (*SDL_GetWindowSize_)(void *window, int *w, int *h);
static void *(*Metal_GetView)(void *sdl_window);
static void (*Metal_GetDrawableSize)(void *view, int *w, int *h);
static void (*Metal_ForceDrawableSize)(void *view, int w, int h); // 0 x 0 unforces it
static void (*orig_UpdateLetterbox)(char *window);
static void (*orig_MetalClientRect)(char *window);

static atomic_int option;           // AvoidNotch
static atomic_int hooked;           // both hooks are in
static atomic_int screen_inset;     // the camera housing of the game window's screen, in points (main thread)
static atomic_bool refresh_pending; // the engine thread should do the letterbox and client rect again
static atomic_uint forced_size;     // width << 16 | height of the drawable we forced, 0 while Aspyr's numbers are in place

// ASL's window mutex is held in both hooks: Window_SetMode and engine_display.c hold it around the letterbox, and
// UpdateClientRect takes it before it calls the Metal client rect
static char *placed_window;         // the window whose letterbox is ours
static NotchLayout placed;          // what we put there
// The window whose last letterbox had everything but its Metal view. ASL makes the view with the swap chain
// (0x1000f2867, from 0x1000e89ab) and then does only the client rect (0x1000e89ef), not the letterbox, so the
// letterbox Window_SetMode did before that stays Aspyr's. The client rect hook has both done again once the view is there.
static char *view_wanted_by;

static int window_int(const char *window, int offset) {
    return *(const int *)(window + offset);
}

#pragma mark - the hooks

// one line each time where the picture goes changes: why it stays where Aspyr puts it, or where it goes instead
static void say(const char *window, const char *why, const NotchLayout *layout, int inset, int w, int h) {
    static const char *last_why;
    static int last[3] = { -1, -1, -1 };
    int now[3] = { inset, layout != NULL ? layout->pillar : 0, layout != NULL ? layout->top : 0 };
    if (why == last_why && memcmp(now, last, sizeof(now)) == 0)
        return;
    last_why = why;
    memcpy(last, now, sizeof(now));
    if (layout == NULL)
        printf("notch: the picture stays where the game puts it (%s)\n", why);
    else
        printf("notch: the picture goes below the %d point camera housing: %.0fx%.0f points at %d,%d of %dx%d\n", inset,
               window_int(window, Window_LogicalW) / layout->scale, window_int(window, Window_LogicalH) / layout->scale,
               layout->pillar, layout->top, w, h);
}

// After Aspyr's: the same letterbox in the window below the housing, when the window is fullscreen with Metal on a
// screen that has one
static void UpdateLetterbox_hook(char *window) {
    orig_UpdateLetterbox(window);
    if (window == NULL)
        return;
    if (window == placed_window)
        placed_window = NULL;
    void *sdl = *(void **)(window + Window_SDL);
    int inset = atomic_load(&screen_inset), w = 0, h = 0;
    NotchLayout layout;
    static const char no_view[] = "no Metal view yet";
    const char *why = !atomic_load(&option) ? "AvoidNotch is off" : inset <= 0 ? "no camera housing on this screen"
                    : sdl == NULL || (*(const uint8_t *)(window + Window_Flags) & 1) == 0 ? "not fullscreen"
                    : Metal_GetView(sdl) == NULL ? no_view : NULL;
    view_wanted_by = why == no_view ? window : NULL;
    if (why == NULL) {
        SDL_GetWindowSize_(sdl, &w, &h);
        if (!notch_layout(window_int(window, Window_LogicalW), window_int(window, Window_LogicalH), w, h, inset, &layout))
            why = "the window's size doesn't fit one";
    }
    if (why != NULL) {
        say(window, why, NULL, 0, 0, 0);
        return;
    }
    *(double *)(window + Window_ScaleX) = layout.scale;
    *(double *)(window + Window_ScaleY) = layout.scale;
    *(int *)(window + Window_Pillar) = layout.pillar;
    *(int *)(window + Window_Letterbox) = layout.top;
    placed_window = window;
    placed = layout;
    say(window, "", &layout, inset, w, h);
}

static bool still_placed(const char *window) {
    return window != NULL && window == placed_window && *(const double *)(window + Window_ScaleX) == placed.scale &&
           *(const double *)(window + Window_ScaleY) == placed.scale && window_int(window, Window_Pillar) == placed.pillar &&
           window_int(window, Window_Letterbox) == placed.top;
}

// the drawable to force for the window's frame in our letterbox, and where the frame goes in it
static bool placed_rect(const char *window, void *sdl, int *drawable_w, int *drawable_h, NotchRect *blit) {
    int logical_w = window_int(window, Window_LogicalW), logical_h = window_int(window, Window_LogicalH);
    int frame_w = window_int(window, Window_FrameW) != 0 ? window_int(window, Window_FrameW) : logical_w;
    int frame_h = window_int(window, Window_FrameH) != 0 ? window_int(window, Window_FrameH) : logical_h;
    int w = 0, h = 0;
    SDL_GetWindowSize_(sdl, &w, &h);
    return notch_drawable(&placed, logical_w, logical_h, w, h, frame_w, frame_h, drawable_w, drawable_h, blit);
}

// Aspyr's 0x1000ee6c6, with the frame where our letterbox has the picture instead of centred
static void MetalClientRect_hook(char *window) {
    void *sdl = window != NULL ? *(void **)(window + Window_SDL) : NULL;
    // the view the letterbox waited for: this frame is Aspyr's, the engine thread does ours next frame
    if (sdl != NULL && window == view_wanted_by && Metal_GetView(sdl) != NULL) {
        view_wanted_by = NULL;
        atomic_store(&refresh_pending, true);
        printf("notch: the Metal view is there now, the letterbox is done again\n");
    }
    void *view = sdl != NULL && still_placed(window) ? Metal_GetView(sdl) : NULL;
    int drawable_w = 0, drawable_h = 0;
    NotchRect blit;
    if (view == NULL || !placed_rect(window, sdl, &drawable_w, &drawable_h, &blit)) {
        atomic_store(&forced_size, 0);
        orig_MetalClientRect(window);
        return;
    }
    // like Aspyr: unforce, read the drawable back, and force it unless it already has that size
    memset(window + Window_Blit, 0, 16);
    Metal_ForceDrawableSize(view, 0, 0);
    int current_w = 0, current_h = 0;
    Metal_GetDrawableSize(view, &current_w, &current_h);
    *(int *)(window + Window_DrawableW) = drawable_w;
    *(int *)(window + Window_DrawableH) = drawable_h;
    memcpy(window + Window_Blit, &blit, sizeof(blit));
    atomic_store(&forced_size, (unsigned)drawable_w << 16 | (unsigned)drawable_h);
    if (current_w != drawable_w || current_h != drawable_h)
        Metal_ForceDrawableSize(view, drawable_w, drawable_h);

    static int last[4];
    int now[4] = { drawable_w, drawable_h, blit.x, blit.y };
    if (memcmp(now, last, sizeof(now)) != 0)
        printf("notch: the %dx%d frame goes at %d,%d in a %dx%d drawable\n", blit.w, blit.h, blit.x, blit.y, drawable_w,
               drawable_h);
    memcpy(last, now, sizeof(now));
}

static bool install_hooks(void) {
    if (atomic_load(&hooked))
        return true;
    uint64_t base = game_base_address;
    if (DobbyHook((void *)(base + ADDR_ASL_Window_MetalClientRect), (void *)MetalClientRect_hook,
                  (void **)&orig_MetalClientRect) != 0)
        return false;
    // the client rect hook passes everything through until the letterbox is ours, so it can go in first on its own
    if (DobbyHook((void *)(base + ADDR_ASL_Window_UpdateLetterbox), (void *)UpdateLetterbox_hook,
                  (void **)&orig_UpdateLetterbox) != 0)
        return false;
    atomic_store(&hooked, 1);
    return true;
}

#pragma mark - the screen

static NSWindow *game_window(void) {
    Class sdl_window = NSClassFromString(@"SDLWindow");
    for (NSWindow *window in NSApp.windows)
        if (sdl_window != nil && [window isKindOfClass:sdl_window])
            return window;
    return nil;
}

// main thread: the housing of the screen the game window is on now
static void check_screen(NSWindow *window) {
    int inset = 0;
    if (@available(macOS 12.0, *)) {
        NSScreen *screen = window.screen;
        if (screen != nil)
            inset = (int)lround(screen.safeAreaInsets.top);
    }
    if (atomic_exchange(&screen_inset, inset) != inset && atomic_load(&option))
        atomic_store(&refresh_pending, true);
}

// main thread: watches the game window move to other screens, and screens change (a built-in display's housing is
// another number of points at another resolution)
static void watch_screens(void) {
    static bool watching;
    if (watching)
        return;
    watching = true;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    NSOperationQueue *main_queue = NSOperationQueue.mainQueue;
    void (^window_changed)(NSNotification *) = ^(NSNotification *note) {
        Class sdl_window = NSClassFromString(@"SDLWindow");
        if (sdl_window != nil && [note.object isKindOfClass:sdl_window])
            check_screen(note.object);
    };
    [center addObserverForName:NSWindowDidChangeScreenNotification object:nil queue:main_queue usingBlock:window_changed];
    [center addObserverForName:NSWindowDidResizeNotification object:nil queue:main_queue usingBlock:window_changed];
    [center addObserverForName:NSWindowDidBecomeKeyNotification object:nil queue:main_queue usingBlock:window_changed];
    [center addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:main_queue
                    usingBlock:^(NSNotification *note) {
        check_screen(game_window());
    }];
    check_screen(game_window());
}

#pragma mark - API

void notch_init(int enabled) {
    uint64_t base = game_base_address;
    if (base == 0)
        return;
    SDL_GetWindowSize_ = (void *)(base + ADDR_SDL_GetWindowSize);
    Metal_GetView = (void *)(base + ADDR_ASL_Metal_GetView);
    Metal_GetDrawableSize = (void *)(base + ADDR_ASL_Metal_GetDrawableSize);
    Metal_ForceDrawableSize = (void *)(base + ADDR_ASL_Metal_ForceDrawableSize);
    if (!enabled)
        return;
    atomic_store(&option, 1);
    if (!install_hooks()) {
        atomic_store(&option, 0);
        printf("notch: couldn't hook ASL's letterbox, AvoidNotch is off\n");
        return;
    }
    // NSApp doesn't exist yet; the block runs once the game's main thread runs its loop
    dispatch_async(dispatch_get_main_queue(), ^{
        watch_screens();
    });
    printf("notch: in fullscreen the picture stays below a camera housing (AvoidNotch)\n");
}

bool notch_live_possible(void) {
    return atomic_load(&hooked) || (prepatch_has_hook(ADDR_ASL_Window_UpdateLetterbox) &&
                                    prepatch_has_hook(ADDR_ASL_Window_MetalClientRect));
}

bool notch_set_enabled(int enabled) {
    if (game_base_address == 0 || SDL_GetWindowSize_ == NULL)
        return false;
    // while the game runs, only the pre-patched copy's slots may be set: patching in memory would write code the
    // window and engine threads may be running
    if (enabled && !atomic_load(&hooked) && (!notch_live_possible() || !install_hooks()))
        return false;
    int was = atomic_exchange(&option, enabled != 0);
    if (enabled)
        dispatch_async(dispatch_get_main_queue(), ^{
            watch_screens();
            check_screen(game_window());
        });
    // the letterbox again with the new value; without the hooks there's nothing to undo
    if (was != (enabled != 0) && atomic_load(&hooked))
        atomic_store(&refresh_pending, true);
    return true;
}

bool notch_take_refresh(void) {
    if (!atomic_load_explicit(&refresh_pending, memory_order_relaxed))
        return false;
    return atomic_exchange(&refresh_pending, false);
}

bool notch_forced_drawable(int *width, int *height) {
    unsigned size = atomic_load(&forced_size);
    if (size == 0)
        return false;
    *width = (int)(size >> 16);
    *height = (int)(size & 0xFFFF);
    return true;
}
