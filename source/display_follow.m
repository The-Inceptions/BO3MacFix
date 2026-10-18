// DisplayFollow: keeps the game on the display the GameDisplay setting picks, at a resolution and refresh rate that suit
// that display, as displays are plugged in, unplugged or changed. The decisions are in display_policy.c.
//
// Aspyr's main thread runs Cocoa's run loop inside SDL_PumpEvents about 120 times a second (0x1000040de), so the CG
// callback, the AppKit notifications and the timer here all run on the macOS main thread. The engine has its own thread
// and sends its SDL calls to the main thread, so nothing here waits for the engine. A move takes two steps:
// sdl_displays_refresh here (SDL's display list with the game's display first, and the window on it), then
// engine_display_request, which the engine applies on its next frame (its mode list, resolution and refresh rate).
#import <AppKit/AppKit.h>
#import <ColorSync/ColorSync.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "display_follow.h"
#include "display_policy.h"
#include "engine_display.h"
#include "metalfx.h"
#include "offsets.h"
#include "sdl_displays.h"

#define Displays_File "../../../BO3MacFix/displays.ini"

extern uint64_t game_base_address;

static const char *const game_display_names[] = { "Main display", "Built-in", "External", "Chosen display" };
static const char *const option_names[] = { "DisplayFollow", "GameDisplay", "DisplayMemory", "MatchRefreshRate", "ConfineCursor" };

// guards what other threads read or change: the snapshot, the game's display and the store
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static DisplayInfo displays[Display_Max];
static int display_count;
static uint32_t game_id; // the game's display: SDL display 0 once a move went through
static DisplayStore store;
static uint64_t store_version;
static DisplaySlots pick_slots;       // GameDisplay's value for each display this session (Game_Display_Pick + slot)
static int display_slots[Display_Max]; // the slot of displays[i]

// one writer of displays.ini at a time, outside `lock` so a slow disk never holds up the others
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t saved_version;
static int started, follow, game_display, use_memory = 1, match_refresh = 1, confine, debug;
static _Atomic uint64_t expect_until_ns;

// main thread only
static DisplayInfo game_info; // the game's display as last seen, kept while it's unplugged
static int appkit_started, sleeping;
static double woke_at, gone_since;
static DisplayDebounce debounce;
static dispatch_source_t timer, cursor_timer;
static int cursor_timer_on, told_let_through;
static id cursor_monitor;
static CursorEdge edge;
static DisplayDecision pending;
static int pending_rehome, pending_request, pending_tries, pending_generation;

static double now_seconds(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}

// the game's FullScreenMode, including the one AutoRestoreFullscreen brings back while it's alt-tabbed out, and
// fullscreen before the engine knows (early in the launch)
static int game_fullscreen(void) {
    int mode = engine_display_fullscreen_mode();
    return mode < 0 ? 1 : mode;
}

#pragma mark - the displays

static void native_size(CGDirectDisplayID id, int *width, int *height) {
    CFStringRef keys[] = { kCGDisplayShowDuplicateLowResolutionModes };
    CFBooleanRef values[] = { kCFBooleanTrue };
    CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 1,
                                                 &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, options);
    CFRelease(options);
    int widest_w = 0, widest_h = 0;
    *width = *height = 0;
    for (CFIndex i = 0; modes != NULL && i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        int w = (int)CGDisplayModeGetPixelWidth(mode), h = (int)CGDisplayModeGetPixelHeight(mode);
        if (CGDisplayModeGetIOFlags(mode) & kDisplayModeNativeFlag) {
            *width = w;
            *height = h;
        }
        // without a native flag: the widest mode that isn't HiDPI
        if (w == (int)CGDisplayModeGetWidth(mode) && w > widest_w) {
            widest_w = w;
            widest_h = h;
        }
    }
    if (modes != NULL)
        CFRelease(modes);
    if (*width == 0) {
        *width = widest_w;
        *height = widest_h;
    }
}

// the online displays in SDL 2.0.7's order: the main display first, then the rest, leaving out mirror secondaries and
// displays without a mode (SDL_cocoamodes.m:236-262), so the indices match SDL's. AppKit adds the names, the maximum
// refresh rate and the camera housing, on the main thread once NSApp exists.
static void uuid_text(CGDirectDisplayID id, char *buf, size_t n) {
    buf[0] = '\0';
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(id);
    if (uuid != NULL) {
        CFStringRef text = CFUUIDCreateString(NULL, uuid);
        CFStringGetCString(text, buf, (CFIndex)n, kCFStringEncodingASCII);
        CFRelease(text);
        CFRelease(uuid);
    }
}

static int take_snapshot(DisplayInfo *out, int max, MirrorInfo *mirrored, int *mirrored_count) {
    uint32_t count = 0;
    CGDirectDisplayID online[64], ids[64];
    *mirrored_count = 0;
    if (CGGetOnlineDisplayList(64, online, &count) != kCGErrorSuccess)
        return 0;
    for (uint32_t i = 0; i < count && *mirrored_count < max; i++) {
        CGDirectDisplayID primary = CGDisplayMirrorsDisplay(online[i]);
        if (primary != kCGNullDirectDisplay) {
            MirrorInfo *m = &mirrored[(*mirrored_count)++];
            uuid_text(online[i], m->key, sizeof(m->key));
            m->primary = primary;
        }
    }
    int candidates = 0;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < count; i++)
            if ((pass == 0) == (CGDisplayIsMain(online[i]) != 0) && CGDisplayMirrorsDisplay(online[i]) == kCGNullDirectDisplay)
                ids[candidates++] = online[i];
    // more than fit: the game's display takes the last place rather than look unplugged
    pthread_mutex_lock(&lock);
    CGDirectDisplayID game = game_id;
    pthread_mutex_unlock(&lock);
    for (int i = max; game != 0 && i < candidates; i++)
        if (ids[i] == game && max > 1)
            ids[max - 1] = game;
    BOOL appkit = pthread_main_np() && NSApp != nil;
    int n = 0;
    @autoreleasepool {
        NSArray<NSScreen *> *screens = appkit ? [NSScreen screens] : nil;
        for (int i = 0; i < candidates && n < max; i++) {
            CGDirectDisplayID id = ids[i];
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
            if (mode == NULL)
                continue;
            DisplayInfo *d = &out[n++];
            memset(d, 0, sizeof(*d));
            d->id = id;
            uuid_text(id, d->uuid, sizeof(d->uuid));
            d->builtin = CGDisplayIsBuiltin(id) != 0;
            d->main = CGDisplayIsMain(id) != 0;
            d->vendor = CGDisplayVendorNumber(id);
            d->model = CGDisplayModelNumber(id);
            d->serial = CGDisplaySerialNumber(id);
            d->unit = CGDisplayUnitNumber(id);
            CGRect bounds = CGDisplayBounds(id);
            d->x = (int)bounds.origin.x;
            d->y = (int)bounds.origin.y;
            d->w = (int)bounds.size.width;
            d->h = (int)bounds.size.height;
            d->pixel_w = (int)CGDisplayModeGetPixelWidth(mode);
            d->pixel_h = (int)CGDisplayModeGetPixelHeight(mode);
            d->scale = d->w > 0 ? (double)d->pixel_w / d->w : 1.0;
            int mode_refresh = (int)lround(CGDisplayModeGetRefreshRate(mode)), screen_fps = 0;
            CGDisplayModeRelease(mode);
            native_size(id, &d->native_w, &d->native_h);
            snprintf(d->name, sizeof(d->name), d->builtin ? "Built-in display" : "Display %u", id);
            for (NSScreen *screen in screens) {
                if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] != id)
                    continue;
                if (@available(macOS 10.15, *)) {
                    if (screen.localizedName.UTF8String != NULL)
                        snprintf(d->name, sizeof(d->name), "%s", screen.localizedName.UTF8String);
                }
                if (@available(macOS 12.0, *)) {
                    screen_fps = (int)screen.maximumFramesPerSecond;
                    d->variable_refresh = screen.minimumRefreshInterval != screen.maximumRefreshInterval;
                    d->notch = (int)screen.safeAreaInsets.top;
                }
            }
            // a virtual display can report 2500 frames per second
            d->refresh = display_policy_refresh(screen_fps, mode_refresh);
        }
    }
    display_policy_keys(out, n);
    return n;
}

static void describe(const DisplayInfo *d, char *buf, size_t n) {
    snprintf(buf, n, "\"%s\" (%s, %dx%d pixels at %gx, %d Hz)", d->name, d->builtin ? "built-in" : "external", d->pixel_w,
             d->pixel_h, d->scale, d->refresh);
}

static void log_displays(void) {
    for (int i = 0; i < display_count; i++) {
        char text[160];
        describe(&displays[i], text, sizeof(text));
        printf("display: [%d] %s%s\n", i, text, displays[i].main ? ", main" : "");
    }
}

static void log_changes(const DisplayInfo *before, int before_count, const DisplayInfo *after, int after_count) {
    char text[160];
    for (int i = 0; i < after_count; i++) {
        int was = display_policy_find(before, before_count, after[i].id);
        describe(&after[i], text, sizeof(text));
        if (was < 0)
            printf("display: %s connected\n", text);
        else if (display_policy_changed(&before[was], &after[i]))
            printf("display: %s changed\n", text);
    }
    for (int i = 0; i < before_count; i++)
        if (display_policy_find(after, after_count, before[i].id) < 0)
            printf("display: \"%s\" disconnected\n", before[i].name);
}

int display_follow_current(DisplayInfo *out) {
    pthread_mutex_lock(&lock);
    int i = display_policy_find(displays, display_count, game_id);
    if (i >= 0)
        *out = displays[i];
    pthread_mutex_unlock(&lock);
    return i >= 0;
}

static void save_store(const DisplayStore *copy, uint64_t version);

// the settings page's GameDisplay choices, logged at launch and whenever they change (the test harness reads the
// 100 + i values from it)
static void log_choices(const int *values, char labels[][64], int n) {
    static char last[2048];
    char line[2048] = "display: game display choices:";
    for (int i = 0; i < n; i++) {
        char item[96];
        snprintf(item, sizeof(item), "%s %d %s", i > 0 ? ";" : "", values[i], labels[i]);
        strlcat(line, item, sizeof(line));
    }
    if (strcmp(line, last) == 0)
        return;
    strlcpy(last, line, sizeof(last));
    printf("%s\n", line);
}

// makes a new snapshot the current one (the old one goes to `before` if it's wanted). A display displays.ini doesn't
// know may be a remembered one on another port, with another UUID: it gets that one's memory.
static void use_snapshot(const DisplayInfo *current, int count, DisplayInfo *before, int *before_count) {
    char told[Display_Max][160];
    int told_count = 0;
    DisplayStore copy;
    uint64_t version = 0;
    pthread_mutex_lock(&lock);
    if (before != NULL) {
        *before_count = display_count;
        memcpy(before, displays, sizeof(displays));
    }
    memcpy(displays, current, sizeof(displays));
    display_count = count;
    display_slots_update(&pick_slots, displays, count, display_slots);
    for (int i = 0; i < count; i++) {
        char from[52];
        if (display_store_adopt(&store, displays, count, i, from, sizeof(from))) {
            snprintf(told[told_count++], sizeof(told[0]), "display: \"%s\" looks like the one remembered as %.8s",
                     displays[i].name, from);
            version = ++store_version;
        }
    }
    if (version != 0)
        copy = store;
    int values[3 + Display_Max];
    char labels[3 + Display_Max][64];
    int choices = display_policy_choices(displays, count, display_slots, values, labels, 3 + Display_Max);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < told_count; i++)
        printf("%s\n", told[i]);
    if (version != 0)
        save_store(&copy, version);
    log_choices(values, labels, choices);
}

#pragma mark - the cursor

static uint8_t *sdl_mouse(void) {
    return game_base_address != 0 ? ((uint8_t *(*)(void))(game_base_address + ADDR_SDL_GetMouse))() : NULL;
}

// the pointer stays on the game's display when the game is fullscreen, in a menu (SDL's relative mode is off), and
// there's another display it could wander onto
static int cursor_confined(void) {
    if (!confine || display_count < 2 || game_fullscreen() == 0)
        return 0;
    uint8_t *mouse = sdl_mouse();
    return mouse != NULL && *(int *)(mouse + 0x88) == 0; // SDL_Mouse.relative_mode
}

static void keep_cursor(void) {
    if (![NSApp isActive] || !cursor_confined())
        return;
    CGRect bounds = CGDisplayBounds(game_id);
    CGEventRef event = CGEventCreate(NULL);
    if (event == NULL || CGRectIsEmpty(bounds)) {
        if (event != NULL)
            CFRelease(event);
        return;
    }
    CGPoint point = CGEventGetLocation(event);
    CFRelease(event);
    // a soft edge: a crossing is pulled back, pushing on (or a second try) lets it through, see display_policy.h
    CursorAction action = cursor_edge_sample(&edge, now_seconds(), !CGRectContainsPoint(bounds, point));
    if (!edge.released) {
        told_let_through = 0;
    } else if (!told_let_through) {
        told_let_through = 1;
        printf("display: letting the pointer off the game's display\n");
    }
    if (action != Cursor_Pull_Back)
        return;
    point.x = fmin(fmax(point.x, CGRectGetMinX(bounds)), CGRectGetMaxX(bounds) - 1);
    point.y = fmin(fmax(point.y, CGRectGetMinY(bounds)), CGRectGetMaxY(bounds) - 1);
    CGWarpMouseCursorPosition(point);
    // a warp holds the mouse still for a moment otherwise
    CGAssociateMouseAndMouseCursorPosition(true);
    if (debug)
        printf("display: pointer brought back onto display %u at %.0f,%.0f\n", game_id, point.x, point.y);
}

// a look every 50 ms while it could matter (ConfineCursor on, the game active, more than one display), so a pointer that
// something else moved (which sends the game no mouse events) counts too. Nothing runs with one display.
static void update_cursor_timer(void) {
    if (!appkit_started)
        return;
    int want = confine && display_count > 1 && [NSApp isActive];
    if (want == cursor_timer_on)
        return;
    if (cursor_timer == NULL) {
        cursor_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(cursor_timer, ^{
            keep_cursor();
        });
        dispatch_source_set_timer(cursor_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
        dispatch_resume(cursor_timer);
    }
    cursor_timer_on = want;
    if (want)
        dispatch_source_set_timer(cursor_timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 20), NSEC_PER_SEC / 20,
                                  NSEC_PER_SEC / 100);
    else
        dispatch_source_set_timer(cursor_timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
}

// ConfineCursor: the event monitor catches the pointer as the mouse moves it, the timer whatever else moves it
static void apply_confine(void) {
    if (confine && cursor_monitor == nil) {
        NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged;
        cursor_monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent *(NSEvent *event) {
            keep_cursor();
            return event;
        }] retain];
    } else if (!confine && cursor_monitor != nil) {
        [NSEvent removeMonitor:cursor_monitor];
        [cursor_monitor release];
        cursor_monitor = nil;
    }
    update_cursor_timer();
}

#pragma mark - moving the game

// a copy of the store taken under `lock` (with ++store_version); an older copy than the last one written is skipped
static void save_store(const DisplayStore *copy, uint64_t version) {
    static int told_read_only;
    pthread_mutex_lock(&save_lock);
    if (version > saved_version) {
        int saved = display_store_save(copy, Displays_File);
        if (saved > 0)
            saved_version = version;
        else if (saved < 0 && !told_read_only)
            printf("display: " Displays_File " is read-only, so what the game uses on each display isn't saved there\n");
        else if (saved == 0)
            printf("display: couldn't save " Displays_File "\n");
        told_read_only = told_read_only || saved < 0;
    }
    pthread_mutex_unlock(&save_lock);
}

static void print_state_later(const char *why, double seconds) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        display_follow_print_state(why);
    });
}

// tier 1, then tier 2. SDL's list can't be rebuilt while ASL holds its window mutex (the engine is changing the window
// or its mode table), and right after a hot-plug SDL may have no mode for the new display yet and put another one first,
// so both are tried again every 250 ms rather than waited for. Only once display 0 really is the target does it count.
static void run_move(int generation) {
    if (generation != pending_generation)
        return;
    if (pending_rehome || pending_request)
        display_follow_expect_changes(1.5);
    int result = sdl_displays_refresh(pending.display, pending_rehome);
    if (result < 0) {
        printf("display: couldn't rebuild SDL's display list for display %u\n", pending.display);
        return;
    }
    uint32_t first = sdl_displays_id(0);
    if (result == 0 || first != pending.display) {
        if (pending_tries++ == 0) {
            if (result == 0)
                printf("display: SDL's display list is busy, retrying\n");
            else
                printf("display: SDL put display %u first, not %u, retrying\n", first, pending.display);
        }
        if (pending_tries < 40) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4), dispatch_get_main_queue(), ^{
                run_move(generation);
            });
            return;
        }
        // the game stays wherever SDL's display 0 is
        pthread_mutex_lock(&lock);
        if (first != 0) {
            game_id = first;
            int i = display_policy_find(displays, display_count, first);
            if (i >= 0)
                game_info = displays[i];
        }
        pthread_mutex_unlock(&lock);
        printf("display: gave up moving the game to display %u, it's on display %u\n", pending.display, first);
        return;
    }
    DisplayStore copy;
    uint64_t version = 0;
    pthread_mutex_lock(&lock);
    game_id = pending.display;
    int i = display_policy_find(displays, display_count, game_id);
    if (i >= 0)
        game_info = displays[i];
    if (i >= 0 && !displays[i].builtin && strcmp(store.last_external, displays[i].key) != 0) {
        strlcpy(store.last_external, displays[i].key, sizeof(store.last_external));
        display_store_add(&store, &displays[i]);
        copy = store;
        version = ++store_version;
    }
    pthread_mutex_unlock(&lock);
    if (version != 0)
        save_store(&copy, version);
    if (pending_request) {
        engine_display_request(&pending);
        if (pending.width > 0 || pending.refresh > 0 || pending.max_fps >= 0) {
            char size[32] = "the same size", refresh[32] = "", cap[32] = "";
            if (pending.width > 0)
                snprintf(size, sizeof(size), "%dx%d", pending.width, pending.height);
            if (pending.refresh > 0)
                snprintf(refresh, sizeof(refresh), " at %d Hz", pending.refresh);
            if (pending.max_fps >= 0)
                snprintf(cap, sizeof(cap), ", MaxFPS %d", pending.max_fps);
            printf("display: asking the game for %s%s%s (%s)\n", size, refresh, cap, pending.why);
        }
    }
    print_state_later("settled", 2.0);
}

static void move_game(const DisplayDecision *decision, int rehome, int request) {
    pending = *decision;
    pending_rehome = rehome;
    pending_request = request;
    pending_tries = 0;
    run_move(++pending_generation);
}

static void arm(double seconds) {
    if (timer != NULL)
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(fmax(seconds, 0.0) * NSEC_PER_SEC)),
                                  DISPATCH_TIME_FOREVER, NSEC_PER_SEC / 50);
}

// after a settled change (or a setting change): which display the game should be on, and what it needs there
static void evaluate(void) {
    double now = now_seconds(), expected = atomic_load(&expect_until_ns) / 1e9;
    if (now < expected) {
        // our own move or restart is still causing events: look once it's done
        arm(expected - now + 0.05);
        return;
    }
    DisplayInfo current[Display_Max], before[Display_Max];
    MirrorInfo mirrored[Display_Max];
    int mirrored_count;
    int count = take_snapshot(current, Display_Max, mirrored, &mirrored_count);
    if (count <= 0)
        return;
    uint32_t sdl_first = sdl_displays_id(0);
    int before_count = 0;
    use_snapshot(current, count, before, &before_count);
    pthread_mutex_lock(&lock);
    if (game_id == 0)
        game_id = sdl_first != 0 ? sdl_first : CGMainDisplayID();
    uint32_t game = game_id;
    pthread_mutex_unlock(&lock);
    log_changes(before, before_count, current, count);
    update_cursor_timer();

    int fullscreen = game_fullscreen();
    uint32_t window_display = sdl_game_window_display();
    if (!follow) {
        // nothing moves, but the present path still asks which display the game is on
        pthread_mutex_lock(&lock);
        if (window_display != 0)
            game_id = window_display;
        else if (display_policy_find(current, count, game_id) < 0)
            game_id = CGMainDisplayID();
        pthread_mutex_unlock(&lock);
        return;
    }
    // windowed: no forced moves, SDL's display 0 just follows the window once it's settled on a screen
    int target;
    if (fullscreen == 0 && window_display != 0) {
        target = display_policy_find(current, count, window_display);
    } else {
        pthread_mutex_lock(&lock);
        target = display_policy_target_mirrored(current, count, game_display, &store, mirrored, mirrored_count);
        pthread_mutex_unlock(&lock);
    }
    int here = display_policy_find(current, count, game);
    if (game_info.id != game) {
        int old = display_policy_find(before, before_count, game);
        if (old >= 0)
            game_info = before[old];
    }
    int known = game_info.id == game;
    if (debug)
        printf("display: game on %u, target %u, fullscreen %d, window on %u\n", game, target >= 0 ? current[target].id : 0,
               fullscreen, window_display);
    if (target < 0)
        return;
    // the game's display went away: give it a moment to come back (a cable, an input switch, waking up)
    if (here < 0 && fullscreen != 0) {
        double grace = woke_at > 0 && now - woke_at < 10.0 ? 5.0 : 2.0;
        if (gone_since == 0)
            gone_since = now;
        if (now - gone_since < grace) {
            arm(grace - (now - gone_since));
            return;
        }
    }
    gone_since = 0;

    int moved = current[target].id != game;
    int changed = !moved && known && display_policy_changed(&game_info, &current[target]);
    int others = count != before_count;
    for (int i = 0; !others && i < count; i++)
        others = display_policy_find(before, before_count, current[i].id) < 0;
    if (!moved && !changed && !others && sdl_first == current[target].id)
        return;

    DisplayDecision decision;
    memset(&decision, 0, sizeof(decision));
    decision.display = current[target].id;
    decision.max_fps = -1;
    snprintf(decision.why, sizeof(decision.why), "displays changed");
    if (moved || changed) {
        RememberedDisplay from, memory;
        memset(&from, 0, sizeof(from));
        if (known)
            display_policy_shape(&game_info, &from);
        char text[160] = "";
        GameValues values;
        engine_display_describe(text, sizeof(text));
        display_policy_parse_game(text, &values);
        DisplayStore copy;
        uint64_t version = 0;
        pthread_mutex_lock(&lock);
        // leaving a display in fullscreen: remember what the game used there
        if (moved && use_memory && known && fullscreen != 0 && values.width > 0) {
            RememberedDisplay *left = display_store_add(&store, &game_info);
            if (left != NULL) {
                left->width = values.width;
                left->height = values.height;
                left->refresh = values.refresh;
                copy = store;
                version = ++store_version;
            }
        }
        RememberedDisplay *entry = use_memory ? display_store_find(&store, current[target].key) : NULL;
        if (entry != NULL)
            memory = *entry;
        pthread_mutex_unlock(&lock);
        if (version != 0)
            save_store(&copy, version);
        display_policy_decide(&from, &values, &current[target], entry != NULL ? &memory : NULL, match_refresh, &decision);
        if (fullscreen == 0) {
            // a window keeps its size and stays where it is
            decision.width = decision.height = 0;
            decision.moved = 0;
        }
        if (moved) {
            char text2[160];
            describe(&current[target], text2, sizeof(text2));
            printf("display: %s the game to %s (%s)\n", fullscreen != 0 ? "moving" : "following", text2,
                   here < 0 ? "its display was unplugged" : fullscreen == 0 ? "the window moved" :
                   game_display_names[game_display]);
        }
    }
    move_game(&decision, fullscreen != 0 && (moved || changed), moved || changed);
}

static void timer_fired(void) {
    if (sleeping)
        return;
    double now = now_seconds(), settle = woke_at > 0 && now - woke_at < 10.0 ? 3.0 : 0.75;
    double wait = display_debounce_wait(&debounce, now, settle, 5.0);
    if (wait > 0) {
        arm(wait);
        return;
    }
    display_debounce_clear(&debounce);
    evaluate();
}

static void poke(int begin) {
    double now = now_seconds(), settle = woke_at > 0 && now - woke_at < 10.0 ? 3.0 : 0.75;
    display_debounce_poke(&debounce, now, begin);
    if (!sleeping)
        arm(display_debounce_wait(&debounce, now, settle, 5.0));
}

static void reconfigured(CGDirectDisplayID display, CGDisplayChangeSummaryFlags flags, void *info) {
    if (debug)
        printf("display: CG display %u flags 0x%x main=%d\n", display, flags, pthread_main_np());
    int begin = (flags & kCGDisplayBeginConfigurationFlag) != 0;
    if (pthread_main_np())
        poke(begin);
    else
        dispatch_async(dispatch_get_main_queue(), ^{
            poke(begin);
        });
}

static void woke(void) {
    sleeping = 0;
    woke_at = now_seconds();
    poke(0);
}

// once the game window is up: a first look (SDL's list may need the game's display first), and the state line
static void wait_for_window(int tries) {
    if (sdl_game_window_display() == 0 && tries < 120) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            wait_for_window(tries + 1);
        });
        return;
    }
    evaluate();
    print_state_later("launch", 2.0);
}

static void start_appkit(void) {
    if (appkit_started)
        return;
    if (NSApp == nil) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
            start_appkit();
        });
        return;
    }
    appkit_started = 1;
    // AppKit's names and refresh rates, without treating them as a change
    DisplayInfo current[Display_Max];
    MirrorInfo mirrored[Display_Max];
    int mirrored_count;
    int count = take_snapshot(current, Display_Max, mirrored, &mirrored_count);
    if (count > 0)
        use_snapshot(current, count, NULL, NULL);
    pthread_mutex_lock(&lock);
    if (game_id == 0)
        game_id = CGMainDisplayID();
    int i = display_policy_find(displays, display_count, game_id);
    if (i >= 0)
        game_info = displays[i];
    pthread_mutex_unlock(&lock);
    log_displays();

    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(timer, ^{
        timer_fired();
    });
    dispatch_source_set_timer(timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
    dispatch_resume(timer);
    CGDisplayRegisterReconfigurationCallback(reconfigured, NULL);
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:nil
                    usingBlock:^(NSNotification *note) {
                        poke(0);
                    }];
    [center addObserverForName:NSWindowDidChangeScreenNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        poke(0);
    }];
    NSNotificationCenter *workspace = [[NSWorkspace sharedWorkspace] notificationCenter];
    [workspace addObserverForName:NSWorkspaceWillSleepNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        sleeping = 1;
    }];
    [workspace addObserverForName:NSWorkspaceDidWakeNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        woke();
    }];
    [workspace addObserverForName:NSWorkspaceScreensDidWakeNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        woke();
    }];
    [center addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        cursor_edge_reset(&edge);
        update_cursor_timer();
    }];
    [center addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        update_cursor_timer();
    }];
    apply_confine();
    wait_for_window(0);
}

#pragma mark - the API

static void save_at_exit(void) {
    display_follow_save();
}

// GameDisplay, DisplayMemory, MatchRefreshRate and ConfineCursor arrive through display_follow_set_option before this
void display_follow_init(int follow_setting) {
    const char *env = getenv("BO3MACFIX_DISPLAYFOLLOW");
    follow = (env != NULL ? atoi(env) : follow_setting) != 0;
    env = getenv("BO3MACFIX_DISPLAYDEBUG");
    debug = env != NULL && atoi(env) != 0;
    display_store_load(&store, Displays_File);
    started = 1;
    // a plain exit() runs atexit handlers; ExitFix's _exit on the game thread calls display_follow_save itself
    atexit(save_at_exit);
    if (follow && game_display == Game_Display_Chosen && store.chosen_name[0] != '\0')
        printf("display: following display changes, GameDisplay = %s (\"%s\")\n", game_display_names[game_display],
               store.chosen_name);
    else if (follow)
        printf("display: following display changes, GameDisplay = %s\n", game_display_names[game_display]);
    else
        printf("display: not following display changes (DisplayFollow = 0)\n");
    // AppKit, the CG callback and the timer once Cocoa's run loop first runs, which is after SDL has set up NSApp
    dispatch_async(dispatch_get_main_queue(), ^{
        start_appkit();
    });
}

uint32_t display_follow_launch_target(void) {
    if (!started)
        return 0;
    DisplayInfo current[Display_Max];
    MirrorInfo mirrored[Display_Max];
    int mirrored_count;
    int count = take_snapshot(current, Display_Max, mirrored, &mirrored_count);
    if (count > 0)
        use_snapshot(current, count, NULL, NULL);
    pthread_mutex_lock(&lock);
    int target = follow ? display_policy_target_mirrored(current, count, game_display, &store, mirrored, mirrored_count) : -1;
    game_id = target >= 0 ? current[target].id : CGMainDisplayID();
    int i = display_policy_find(current, count, game_id);
    if (i >= 0)
        game_info = current[i];
    pthread_mutex_unlock(&lock);
    if (target < 0)
        return 0;
    char text[160];
    describe(&current[target], text, sizeof(text));
    printf("display: starting on %s (GameDisplay = %s)\n", text, game_display_names[game_display]);
    return current[target].id;
}

// the caller passes config.ini's values in *out (the engine's vidConfig isn't set up yet, so engine_display_describe
// can't tell them here)
int display_follow_launch_choice(DisplayDecision *out) {
    GameValues values = { out->width, out->height, out->refresh, out->max_fps };
    int apply = 0;
    pthread_mutex_lock(&lock);
    uint32_t display = game_id;
    int i = display_policy_find(displays, display_count, display);
    if (started && follow && i >= 0 && values.width > 0 && values.height > 0)
        apply = display_policy_launch(&store, displays, display_count, &displays[i], &values, use_memory, match_refresh, out);
    pthread_mutex_unlock(&lock);
    if (!apply) {
        memset(out, 0, sizeof(*out));
        out->display = display;
        out->max_fps = -1;
        return 0;
    }
    out->moved = 0; // the window doesn't exist yet: it's created on this display
    char cap[32] = "";
    if (out->max_fps >= 0)
        snprintf(cap, sizeof(cap), ", MaxFPS %d", out->max_fps);
    printf("display: starting at %dx%d, %d Hz%s (config.ini has %dx%d, %d Hz: %s)\n",
           out->width > 0 ? out->width : values.width, out->width > 0 ? out->height : values.height,
           out->refresh > 0 ? out->refresh : values.refresh, cap, values.width, values.height, values.refresh, out->why);
    return 1;
}

void display_follow_note_choice(int width, int height, int refresh) {
    if (!started || !use_memory || width <= 0 || height <= 0)
        return;
    DisplayStore copy;
    uint64_t version = 0;
    RememberedDisplay remembered;
    pthread_mutex_lock(&lock);
    int i = display_policy_find(displays, display_count, game_id);
    RememberedDisplay *entry = i >= 0 ? display_store_add(&store, &displays[i]) : NULL;
    if (entry != NULL) {
        entry->width = width;
        entry->height = height;
        if (refresh > 0)
            entry->refresh = refresh;
        remembered = *entry;
        copy = store;
        version = ++store_version;
    }
    pthread_mutex_unlock(&lock);
    if (version != 0) {
        save_store(&copy, version);
        printf("display: remembering %dx%d at %d Hz for \"%s\"\n", width, height, remembered.refresh, remembered.name);
    }
}

void display_follow_expect_changes(double seconds) {
    uint64_t until = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (uint64_t)(seconds * 1e9);
    uint64_t old = atomic_load(&expect_until_ns);
    while (old < until && !atomic_compare_exchange_weak(&expect_until_ns, &old, until)) {
    }
}

int display_follow_owns(const char *name) {
    for (size_t i = 0; i < sizeof(option_names) / sizeof(option_names[0]); i++)
        if (strcmp(name, option_names[i]) == 0)
            return 1;
    return 0;
}

// before display_follow_init this only stores the startup value
int display_follow_set_option(const char *name, int value) {
    if (!display_follow_owns(name))
        return 0;
    int look_again = 0;
    DisplayStore copy;
    uint64_t version = 0;
    char told[256] = "";
    pthread_mutex_lock(&lock);
    if (strcmp(name, "DisplayFollow") == 0) {
        look_again = !follow && value != 0;
        follow = value != 0;
    } else if (strcmp(name, "GameDisplay") == 0) {
        int rule = value < 0 || value > Game_Display_Chosen ? Game_Display_Main : value;
        if (value >= Game_Display_Pick) {
            // a display picked by name on the settings page becomes the chosen display. The value is the display's
            // for the whole session, so a page that listed the displays before one came or went still gets the one it
            // showed; one unplugged since is chosen all the same (the External rule applies until it's back).
            const DisplayInfo *slot = display_slots_pick(&pick_slots, value);
            int i = slot != NULL ? display_policy_find_key(displays, display_count, slot->key) : -1;
            DisplayInfo picked = { 0 };
            if (slot != NULL)
                picked = i >= 0 ? displays[i] : *slot;
            rule = game_display;
            if (slot != NULL && display_store_add(&store, &picked) != NULL) {
                strlcpy(store.chosen, picked.key, sizeof(store.chosen));
                strlcpy(store.chosen_name, picked.name, sizeof(store.chosen_name));
                copy = store;
                version = ++store_version;
                rule = Game_Display_Chosen;
                snprintf(told, sizeof(told), "display: the game's display is \"%s\" now%s", picked.name,
                         i < 0 ? " (not connected, so it goes by the External rule until it's back)" : "");
            }
        }
        look_again = game_display != rule || version != 0;
        game_display = rule;
    } else if (strcmp(name, "DisplayMemory") == 0) {
        use_memory = value != 0;
    } else if (strcmp(name, "MatchRefreshRate") == 0) {
        match_refresh = value != 0;
    } else {
        confine = value != 0;
    }
    pthread_mutex_unlock(&lock);
    if (version != 0)
        save_store(&copy, version);
    if (told[0] != '\0')
        printf("%s\n", told);
    if (!started)
        return 1;
    // the settings page runs on the engine thread: move (or change the cursor) from the main thread, without waiting
    int cursor = strcmp(name, "ConfineCursor") == 0;
    if (look_again || cursor)
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!appkit_started)
                return;
            if (cursor)
                apply_confine();
            else
                evaluate();
        });
    return 1;
}

// once per quit: ExitFix's path and the atexit handler can both get here
void display_follow_save(void) {
    static atomic_int saved;
    if (!started || atomic_exchange(&saved, 1))
        return;
    char text[160] = "";
    GameValues values;
    engine_display_describe(text, sizeof(text));
    int have_values = display_policy_parse_game(text, &values);
    DisplayStore copy;
    uint64_t version = 0;
    pthread_mutex_lock(&lock);
    int i = display_policy_find(displays, display_count, game_id);
    RememberedDisplay *entry = i >= 0 ? display_store_add(&store, &displays[i]) : NULL;
    if (entry != NULL) {
        // config.ini (which the game saved just before quitting) now has this display's values
        strlcpy(store.last_display, entry->key, sizeof(store.last_display));
        if (!displays[i].builtin)
            strlcpy(store.last_external, entry->key, sizeof(store.last_external));
        if (use_memory && have_values) {
            entry->width = values.width;
            entry->height = values.height;
            entry->refresh = values.refresh;
        }
        copy = store;
        version = ++store_version;
    }
    pthread_mutex_unlock(&lock);
    if (version != 0)
        save_store(&copy, version);
}

void display_follow_print_state(const char *why) {
    DisplayInfo d;
    if (!display_follow_current(&d))
        memset(&d, 0, sizeof(d));
    char sdl[256] = "", engine[160] = "", present[160] = "";
    sdl_displays_describe(sdl, sizeof(sdl));
    engine_display_describe(engine, sizeof(engine));
    present_describe(present, sizeof(present));
    printf("display state: display=%u uuid=%.8s builtin=%d pixels=%dx%d scale=%g hz=%d %s %s %s cursor=%s why=%s\n", d.id,
           d.uuid, d.builtin, d.pixel_w, d.pixel_h, d.scale, d.refresh, sdl, engine, present,
           cursor_confined() && !edge.released ? "confined" : "free", why);
}

int display_follow_game_display_choices(int *values, char labels[][64], int max) {
    pthread_mutex_lock(&lock);
    int n = display_policy_choices(displays, display_count, display_slots, values, labels, max);
    pthread_mutex_unlock(&lock);
    return n;
}

int display_follow_game_display_current(void) {
    pthread_mutex_lock(&lock);
    int value = display_policy_current(game_display, displays, display_count, display_slots, &store);
    pthread_mutex_unlock(&lock);
    return value;
}

int display_follow_persisted_value(const char *name, int value) {
    return display_policy_persisted(name, value);
}
