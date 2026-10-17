// Following display changes, the SDL side. This port has one monitor, SDL display 0: Aspyr's Win32 and DXGI layer
// (ASL) builds its mode table for display 0 only, and every monitor function it emulates counts that table. SDL 2.0.7
// builds its display list once, with macOS's main display first, and never updates it. So the game uses whatever
// display 0 is, and here display 0 is made the display the game should use: at launch by reordering SDL's list before
// ASL builds its table, after a display change by rebuilding SDL's list and moving the window there.
// The addresses and layouts are in wip/display/2-sdl-asl.md.
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CoreVideo.h>
#include "display_follow.h"
#include "dobby.h"
#include "engine_display.h"
#include "offsets.h"
#include "sdl_displays.h"

extern uint64_t game_base_address;

// SDL 2.0.7's structs as Aspyr built them: SDL_DisplayMode has an extra double (the backing scale, which nothing
// reads), so SDL_VideoDisplay is 0x70 bytes, and SDL_Window's fields from opacity on are 8 bytes further along
typedef struct SDLDisplayMode {
    uint32_t format;
    int w, h, refresh_rate;
    void *driverdata; // SDL_DisplayModeData *, { CGDisplayModeRef moderef; }
    double scale;
} SDLDisplayMode;

typedef struct SDLDisplay {
    char *name;
    int max_display_modes, num_display_modes;
    SDLDisplayMode *display_modes;
    SDLDisplayMode desktop_mode, current_mode;
    void *fullscreen_window;
    void *device;
    CGDirectDisplayID *driverdata; // SDL_DisplayData *, { CGDirectDisplayID display; }
} SDLDisplay;

_Static_assert(sizeof(SDLDisplayMode) == 0x20, "SDL_DisplayMode");
_Static_assert(sizeof(SDLDisplay) == 0x70 && offsetof(SDLDisplay, current_mode) == 0x38 &&
    offsetof(SDLDisplay, fullscreen_window) == 0x58 && offsetof(SDLDisplay, driverdata) == 0x68, "SDL_VideoDisplay");

// SDL_VideoDevice
#define DEVICE_NUM_DISPLAYS 0x248
#define DEVICE_DISPLAYS 0x250
#define DEVICE_WINDOWS 0x258
// SDL_Window
#define WINDOW_RECT 0x20 // int x, y, w, h, in points
#define WINDOW_FLAGS 0x40
#define WINDOW_DRIVERDATA 0xc8 // SDL_WindowData *, its NSWindow at +0x8
#define WINDOW_NEXT 0xd8
#define SDL_WINDOW_FULLSCREEN 0x1
#define SDL_WINDOW_SHOWN 0x4

// room in every display array made here, more than SDL's own list can have had
#define MAX_DISPLAYS 16

static char *video(void) {
    return game_base_address != 0 ? *(char **)(game_base_address + ADDR_SDL_VideoDevice) : NULL;
}

// readers, some on the engine thread, load the count and then the array (SDL_GetDesktopDisplayMode and the rest), so
// the array is published first. Each new array has room for at least the old count and old ones are never freed, so a
// reader that has the old count with the new array, or the old array, still reads memory that's there.
static void publish(char *dev, SDLDisplay *displays, int count) {
    __atomic_store_n((SDLDisplay **)(dev + DEVICE_DISPLAYS), displays, __ATOMIC_RELEASE);
    __atomic_store_n((int *)(dev + DEVICE_NUM_DISPLAYS), count, __ATOMIC_RELEASE);
}

static SDLDisplay *new_display_array(int old_count) {
    return calloc(old_count > MAX_DISPLAYS ? old_count : MAX_DISPLAYS, sizeof(SDLDisplay));
}

static SDLDisplay *displays_of(char *dev, int *count) {
    *count = __atomic_load_n((int *)(dev + DEVICE_NUM_DISPLAYS), __ATOMIC_ACQUIRE);
    return __atomic_load_n((SDLDisplay **)(dev + DEVICE_DISPLAYS), __ATOMIC_ACQUIRE);
}

int sdl_displays_count(void) {
    char *dev = video();
    int count = 0;
    if (dev != NULL)
        displays_of(dev, &count);
    return count;
}

uint32_t sdl_displays_id(int index) {
    char *dev = video();
    if (dev == NULL)
        return 0;
    int count;
    SDLDisplay *displays = displays_of(dev, &count);
    return index >= 0 && index < count && displays[index].driverdata != NULL ? *displays[index].driverdata : 0;
}

// the game's window: SDL's first shown one (ASL makes hidden ones while it starts)
static char *game_window(char *dev) {
    for (char *window = *(char **)(dev + DEVICE_WINDOWS); window != NULL; window = *(char **)(window + WINDOW_NEXT))
        if (*(uint32_t *)(window + WINDOW_FLAGS) & SDL_WINDOW_SHOWN)
            return window;
    return NULL;
}

static void *nswindow_of(char *window) {
    char *data = *(char **)(window + WINDOW_DRIVERDATA);
    return data != NULL ? *(void **)(data + 0x8) : NULL;
}

// Fullscreen is SDL's desktop fullscreen, which is all ASL asks SDL for. Fullscreen Window is a plain window that
// ASL sizes to its display, so that one comes from the engine's mode: right after the game's display is unplugged,
// AppKit has already moved the window onto another screen at its old size. Until the engine's mode is known, a
// window that exactly covers its display counts as one.
static int window_mode(char *window) {
    if (*(uint32_t *)(window + WINDOW_FLAGS) & SDL_WINDOW_FULLSCREEN)
        return 1;
    int engine_mode = engine_display_fullscreen_mode();
    if (engine_mode >= 0)
        return engine_mode == 2 ? 2 : 0;
    int *rect = (int *)(window + WINDOW_RECT);
    CGDirectDisplayID display;
    uint32_t count = 0;
    CGGetDisplaysWithPoint(CGPointMake(rect[0] + rect[2] / 2.0, rect[1] + rect[3] / 2.0), 1, &display, &count);
    if (count == 0)
        return 0;
    CGRect bounds = CGDisplayBounds(display);
    return rect[0] == (int)bounds.origin.x && rect[1] == (int)bounds.origin.y &&
        rect[2] == (int)bounds.size.width && rect[3] == (int)bounds.size.height ? 2 : 0;
}

int sdl_game_fullscreen_mode(void) {
    char *dev = video();
    char *window = dev != NULL ? game_window(dev) : NULL;
    return window != NULL ? window_mode(window) : 0;
}

uint32_t sdl_game_window_display(void) {
    char *dev = video();
    char *window = dev != NULL ? game_window(dev) : NULL;
    void *nswindow = window != NULL ? nswindow_of(window) : NULL;
    return nswindow != NULL ? sdl_cocoa_window_display(nswindow) : 0;
}

void sdl_displays_describe(char *buf, size_t n) {
    char *dev = video();
    char *window = dev != NULL ? game_window(dev) : NULL;
    void *nswindow = window != NULL ? nswindow_of(window) : NULL;
    if (nswindow == NULL) {
        snprintf(buf, n, "window=none");
        return;
    }
    char frames[96];
    sdl_cocoa_describe(nswindow, frames, sizeof(frames));
    int *rect = (int *)(window + WINDOW_RECT);
    snprintf(buf, n, "window=%dx%d fullscreen=%d %s", rect[2], rect[3], window_mode(window), frames);
}

static void print_displays(const char *why) {
    char line[512];
    int count, used = snprintf(line, sizeof(line), "display: %s, SDL's displays:", why);
    SDLDisplay *displays = displays_of(video(), &count);
    for (int i = 0; i < count && used < (int)sizeof(line); i++) {
        SDLDisplayMode *mode = &displays[i].desktop_mode;
        used += snprintf(line + used, sizeof(line) - used, "%s [%d] %u %s %dx%d@%d %gx", i > 0 ? "," : "", i,
            displays[i].driverdata != NULL ? *displays[i].driverdata : 0, displays[i].name != NULL ? displays[i].name : "?",
            mode->w, mode->h, mode->refresh_rate, mode->scale);
    }
    printf("%s\n", line);
}

// moves the display with this id to the front of the list, keeping the others in order; returns where it was
static int move_to_front(SDLDisplay *displays, int count, CGDirectDisplayID id) {
    for (int i = 0; i < count; i++) {
        if (displays[i].driverdata == NULL || *displays[i].driverdata != id)
            continue;
        if (i > 0) {
            SDLDisplay display = displays[i];
            memmove(&displays[1], &displays[0], i * sizeof(SDLDisplay));
            displays[0] = display;
        }
        return i;
    }
    return -1;
}

// void ASL_BuildModeTable(void), once on the main thread at startup before the engine starts. With the display the
// game should start on first in SDL's list, ASL's one monitor is that display. No windows exist yet.
install_hook_name(ASL_BuildModeTable, void, void) {
    char *dev = video();
    CGDirectDisplayID target = display_follow_launch_target();
    if (dev != NULL && target != 0) {
        int count;
        SDLDisplay *displays = displays_of(dev, &count);
        int was = move_to_front(displays, count, target);
        if (was < 0)
            printf("display: display %u isn't in SDL's list, starting on SDL's display 0\n", target);
        else if (was > 0)
            print_displays("starting on another display");
    }
    orig_ASL_BuildModeTable();
}

// ASL's mode table (a std::vector of one std::vector of modes, for display 0) is only built at startup. With the outer
// vector emptied, the original builds the entry again in the same storage, and nothing is freed, so a reader that
// still has the old entry's pointers reads memory that's still there.
int sdl_displays_rebuild_mode_table(void) {
    if (game_base_address == 0 || orig_ASL_BuildModeTable == NULL)
        return 0;
    char **table = (char **)(game_base_address + ADDR_ASL_ModeTable); // begin, end, capacity
    if (table[0] == NULL)
        return 0;
    // The original gets display 0's mode count through a call it sends to the main thread and then reads the modes
    // here, so sdl_displays_refresh mustn't swap SDL's list in between; with ASL's window mutex held, its try fails.
    // Waiting for the mutex is fine off the main thread: its users are the engine, window and render threads,
    // sdl_displays_refresh only tries it, and the call sent to the main thread doesn't take it.
    pthread_mutex_t *mutex = (pthread_mutex_t *)(game_base_address + ADDR_ASL_WindowMutex);
    if ((pthread_main_np() ? pthread_mutex_trylock(mutex) : pthread_mutex_lock(mutex)) != 0)
        return 0;
    char *storage = table[0];
    __atomic_store_n(&table[1], table[0], __ATOMIC_RELEASE);
    orig_ASL_BuildModeTable();
    pthread_mutex_unlock(mutex);
    char **modes = (char **)table[0]; // begin, end, capacity of 0x18-byte modes
    printf("display: rebuilt the game's mode list for display %u, %d modes (storage %p, was %p)\n", sdl_displays_id(0),
        table[1] > table[0] ? (int)((modes[1] - modes[0]) / 0x18) : 0, (void *)table[0], (void *)storage);
    return 1;
}

// the online displays the way Cocoa_InitModes lists them (no mirror secondaries), with first at the front, then the
// main display, then the rest
static int online_displays(CGDirectDisplayID *ids, int max, CGDirectDisplayID first) {
    CGDirectDisplayID online[32];
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(32, online, &count) != kCGErrorSuccess)
        return 0;
    int n = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (uint32_t i = 0; i < count && n < max; i++) {
            CGDirectDisplayID id = online[i];
            int wanted = pass == 0 ? id == first : id != first && (pass == 1) == (CGDisplayIsMain(id) != 0);
            if (wanted && CGDisplayMirrorsDisplay(id) == kCGNullDirectDisplay)
                ids[n++] = id;
        }
    }
    return n;
}

// one entry of SDL's display list, made the way Cocoa_InitModes makes it. Its mode list stays empty, and SDL fills it
// (Cocoa_GetDisplayModes) the next time something asks.
static int make_display(SDLDisplay *display, CGDirectDisplayID id, char *dev) {
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
    if (mode == NULL)
        return 0;
    int (*get_display_mode)(CGDisplayModeRef, CFArrayRef, CVDisplayLinkRef, SDLDisplayMode *) =
        (void *)(game_base_address + ADDR_Cocoa_GetDisplayMode);
    CVDisplayLinkRef link = NULL;
    CVDisplayLinkCreateWithCGDisplay(id, &link);
    int made = get_display_mode(mode, NULL, link, &display->desktop_mode); // keeps the mode reference when it works
    if (link != NULL)
        CVDisplayLinkRelease(link);
    if (!made) {
        CGDisplayModeRelease(mode);
        return 0;
    }
    char name[64];
    display->desktop_mode.scale = sdl_cocoa_screen(id, name, sizeof(name));
    display->current_mode = display->desktop_mode;
    display->name = strdup(name);
    display->device = dev;
    display->driverdata = malloc(sizeof(CGDirectDisplayID));
    *display->driverdata = id;
    return 1;
}

// puts the game window on display 0
static void rehome(char *dev, char *window, int mode, SDLDisplay *display) {
    CGRect bounds = CGDisplayBounds(*display->driverdata);
    int x = (int)bounds.origin.x, y = (int)bounds.origin.y, w = (int)bounds.size.width, h = (int)bounds.size.height;
    void (*set_position)(void *, int, int) = (void *)(game_base_address + ADDR_SDL_SetWindowPosition);
    void (*set_size)(void *, int, int) = (void *)(game_base_address + ADDR_SDL_SetWindowSize);
    int *rect = (int *)(window + WINDOW_RECT);
    if (mode == 1) {
        // Cocoa_SetWindowFullscreen covers the display's bounds, but corrects the origin by the height of the screen
        // the window is on now (its fix for Mac OS X 10.4), so the window goes onto the right screen first
        void (*set_fullscreen)(void *, void *, void *, int) = (void *)(game_base_address + ADDR_Cocoa_SetWindowFullscreen);
        // SDL's windowDidMove: puts the window back where it was if SDL went fullscreen less than 500 ms ago
        uint32_t went_fullscreen = *(volatile uint32_t *)(game_base_address + ADDR_SDL_s_moveHack);
        uint32_t since = ((uint32_t (*)(void))(game_base_address + ADDR_SDL_GetTicks))() - went_fullscreen;
        if (went_fullscreen != 0 && since < 500)
            printf("display: SDL went fullscreen %u ms ago, so it may undo moving the window to the new screen first\n", since);
        sdl_cocoa_move_to_screen(nswindow_of(window), *display->driverdata);
        set_fullscreen(dev, window, display, 1);
    } else if (mode == 2) {
        set_position(window, x, y);
        set_size(window, w, h);
    } else {
        // windowed: only if it ended up off every display, then centred on display 0, smaller if it doesn't fit
        uint32_t count = 0;
        CGGetDisplaysWithRect(CGRectMake(rect[0], rect[1], rect[2], rect[3]), 0, NULL, &count);
        if (count > 0)
            return;
        int window_w = rect[2] < w ? rect[2] : w, window_h = rect[3] < h ? rect[3] : h;
        set_position(window, x + (w - window_w) / 2, y + (h - window_h) / 2);
        set_size(window, window_w, window_h);
    }
    printf("display: moved the window to display %u (%s) at %d,%d %dx%d\n", *display->driverdata,
        mode == 1 ? "fullscreen" : mode == 2 ? "fullscreen window" : "windowed", rect[0], rect[1], rect[2], rect[3]);
}

int sdl_displays_refresh(uint32_t target, int rehome_window) {
    char *dev = video();
    if (!pthread_main_np() || dev == NULL)
        return -1;
    // Window_SetMode holds ASL's window mutex while it waits for the main thread, so only try it. Present holds it for
    // most of each frame too, so keep trying for up to 30 ms to catch it between two frames.
    pthread_mutex_t *mutex = (pthread_mutex_t *)(game_base_address + ADDR_ASL_WindowMutex);
    int locked = pthread_mutex_trylock(mutex) == 0;
    for (int i = 0; !locked && i < 60; i++) {
        usleep(500);
        locked = pthread_mutex_trylock(mutex) == 0;
    }
    if (!locked)
        return 0;
    if (target == 0)
        target = sdl_displays_id(0);
    CGDirectDisplayID ids[MAX_DISPLAYS];
    int old_count, count = 0, online = online_displays(ids, MAX_DISPLAYS, target);
    displays_of(dev, &old_count);
    SDLDisplay *displays = new_display_array(old_count);
    for (int i = 0; i < online; i++)
        count += make_display(&displays[count], ids[i], dev);
    if (count == 0) {
        free(displays);
        pthread_mutex_unlock(mutex);
        printf("display: no displays to give SDL, keeping its list\n");
        return -1;
    }
    if (*displays[0].driverdata != target)
        printf("display: display %u isn't online, the game's display is now %u\n", target, *displays[0].driverdata);
    char *window = game_window(dev);
    int mode = window != NULL ? window_mode(window) : 0;
    if (mode == 1)
        displays[0].fullscreen_window = window;
    publish(dev, displays, count);
    print_displays("updated");
    if (rehome_window && window != NULL && nswindow_of(window) != NULL)
        rehome(dev, window, mode, &displays[0]);
    pthread_mutex_unlock(mutex);
    return 1;
}

void sdl_displays_init(void) {
    if (game_base_address == 0)
        return;
    install_hook_ASL_BuildModeTable((void *)(game_base_address + ADDR_ASL_BuildModeTable));
    sdl_cocoa_init();
}
