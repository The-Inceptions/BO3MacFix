// On-screen notices in the game, see notices.h. What happens outside the game goes into a queue as a line of text:
// - macOS's notifications for controllers connecting and disconnecting, the same ones gc_bridge.m acts on (so only
//   with ControllerBridge, which is when the game uses the GameController framework anyway);
// - a look at those controllers' batteries every minute, warning at 20, 10 and 5 percent (notice_queue.c);
// - the game window moving to another display, which display_follow.m does, or the player dragging it.
// notices.lua, in the game's UI, takes them from the queue four times a second and shows each for a few seconds at
// the top of the screen.
//
// PauseOnDisconnect: when the controller the game reads disconnects and no other takes its place within half a second
// (gc_bridge.m works that out: not an idle second controller, nor a DualSense going from Bluetooth to USB or back),
// notices.lua is asked to pause. It does if the game is in a match that can pause (Engine.CanPauseGame: solo and
// offline games), the pause menu isn't open already and the last input came from a controller. To pause, this posts
// an Escape key press to the game's own event queue, so the engine opens its pause menu and pauses exactly as when
// the player presses Esc.
//
// PauseMenuFPS and MenuFPS: notices.lua also tells gpu_tweaks.c when the pause menu is open in a match and when the
// game is in the menus outside one, which works with Notices and PauseOnDisconnect off too (notices.lua is loaded in
// every UI Lua state either way).
//
// Test notifications, with BO3MACFIX_TESTHOOKS=1 (notifyutil -s NAME VALUE sets the value, notifyutil -p NAME posts):
//   com.bo3macfix.test.notice      shows "Test notice N"
//   com.bo3macfix.test.controller  1: a made-up DualSense connects. 0: it disconnects, asking to pause as if the last
//                                  input came from it
//   com.bo3macfix.test.battery     the made-up DualSense's battery is now VALUE percent (VALUE + 1000: and charging)
//   com.bo3macfix.test.escape      posts Escape, what PauseOnDisconnect does
//   com.bo3macfix.test.cmdq        does what the app menu's Cmd+Q item does, saying first what that is
//   com.bo3macfix.test.modwarning  shows the warning for a mod's Windows DLL (bo3macfix.c), for a made-up Test.dll
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#include <notify.h>
#include <objc/runtime.h>
#include <os/lock.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "gc_bridge.h"
#include "gpu_tweaks.h"
#include "havok.h"
#include "notice_queue.h"
#include "notices.h"
#include "notices_lua.h"

#define Max_Notice_Age 20.0    // seconds a notice waits for the UI (while a map loads, say) before it's dropped
#define Lasting_Show 8         // seconds a lasting notice (a warning) shows, twice the others
#define Max_Pause_Age 5.0      // seconds a pause request waits
#define Startup_Quiet 15.0     // controllers macOS finds while the game starts aren't news
#define Startup_Display_Quiet 30.0 // nor the display the window first goes to
#define Battery_Interval 60    // seconds between battery checks
#define Key_Escape 53         // kVK_Escape

static atomic_bool notices_on = false, pause_on = false;
static bool debug_menus = false;
static double started;
static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
static notice_queue_t queue;       // under lock
static double pause_requested;     // under lock: when a controller went away, 0 for no request
static bool pause_forced;          // under lock: pause whatever the last input was (the test hook)

static double now(void) {
    return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}

__attribute__((format(printf, 2, 0))) static void post(bool lasting, const char *format, va_list args) {
    char text[Notice_Text_Max];
    vsnprintf(text, sizeof(text), format, args);
    bool on = atomic_load(&notices_on);
    printf("BO3MacFix: notice: %s%s\n", text, on ? "" : " (not shown, Notices is off)");
    if (!on)
        return;
    os_unfair_lock_lock(&lock);
    if (lasting)
        notice_queue_push_lasting(&queue, text, now());
    else
        notice_queue_push(&queue, text, now());
    os_unfair_lock_unlock(&lock);
}

void notices_post(const char *format, ...) {
    va_list args;
    va_start(args, format);
    post(false, format, args);
    va_end(args);
}

void notices_post_lasting(const char *format, ...) {
    va_list args;
    va_start(args, format);
    post(true, format, args);
    va_end(args);
}

void notices_mod_warning(const char *file, const char *func) {
    printf("BO3MacFix: Mod Warning: the mod you loaded tried to open a Windows DLL file (%s: %s). The game might work "
           "fine, but it might be unstable.\n", file, func);
    notices_post_lasting("Mod warning: can't load Windows DLL %s, the game might be unstable", file);
}

static void request_pause(bool forced) {
    if (!atomic_load(&pause_on))
        return;
    os_unfair_lock_lock(&lock);
    pause_requested = now();
    pause_forced = forced;
    os_unfair_lock_unlock(&lock);
}

void notices_set_enabled(int on) {
    atomic_store(&notices_on, on != 0);
    if (on == 0) {
        os_unfair_lock_lock(&lock);
        queue.count = 0;
        os_unfair_lock_unlock(&lock);
    }
}

bool notices_enabled(void) {
    return atomic_load(&notices_on);
}

void notices_set_pause_on_disconnect(int on) {
    atomic_store(&pause_on, on != 0);
}

#pragma mark - controllers

static const void *Battery_Warned_Key = &Battery_Warned_Key;

static NSString *controller_name(GCController *controller) {
    NSString *name = nil;
    if (@available(macOS 10.15, *))
        name = controller.productCategory;
    if (name.length == 0)
        name = controller.vendorName;
    return name.length > 0 ? name : @"Controller";
}

// the battery level in percent, -1 if macOS doesn't know it. A DualSense over Bluetooth gives its level with an
// unknown state, so charging is only known over USB.
static int battery_percent(GCController *controller, bool *charging) {
    *charging = false;
    if (@available(macOS 11.0, *)) {
        GCDeviceBattery *battery = controller.battery;
        if (battery == nil || battery.batteryLevel <= 0.0f)
            return -1;
        *charging = battery.batteryState == GCDeviceBatteryStateCharging || battery.batteryState == GCDeviceBatteryStateFull;
        return (int)(battery.batteryLevel * 100.0f + 0.5f);
    }
    return -1;
}

// whether to warn about this battery level now; what's been warned about is kept on the controller (or the test's
// stand-in for one)
static bool battery_low(id owner, int percent, bool charging) {
    int warned = [objc_getAssociatedObject(owner, Battery_Warned_Key) intValue];
    int warn = battery_warning(&warned, percent, charging);
    objc_setAssociatedObject(owner, Battery_Warned_Key, @(warned), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return warn != 0;
}

static void check_battery(id owner, NSString *name, int percent, bool charging) {
    if (battery_low(owner, percent, charging))
        notices_post("%s battery low: %d%%", name.UTF8String, percent);
}

static void check_batteries(void) {
    if (!atomic_load(&notices_on))
        return;
    for (GCController *controller in GCController.controllers) {
        if (controller.extendedGamepad == nil)
            continue;
        bool charging;
        int percent = battery_percent(controller, &charging);
        check_battery(controller, controller_name(controller), percent, charging);
    }
}

static void controller_connected(NSString *name, int percent, bool charging, id battery_owner) {
    if (now() - started < Startup_Quiet) {
        printf("BO3MacFix: %s was there at startup, no notice\n", name.UTF8String);
        return;
    }
    if (percent < 0)
        notices_post("%s connected", name.UTF8String);
    else if (battery_low(battery_owner, percent, charging))
        notices_post("%s connected, battery low: %d%%", name.UTF8String, percent);
    else
        notices_post("%s connected, battery %d%%%s", name.UTF8String, percent, charging ? " and charging" : "");
}

// forced: the test hook's made-up DualSense, which pauses whatever the last input was. A real controller only pauses
// when it was the one the game reads, through notices_controller_lost
static void controller_disconnected(NSString *name, bool forced) {
    notices_post("%s disconnected", name.UTF8String);
    if (forced)
        request_pause(true);
}

void notices_controller_lost(void) {
    request_pause(false);
}

#pragma mark - displays

static CGDirectDisplayID shown_display; // main thread: the display the game was last seen on, 0 until it's known
                                        // (the window becoming key, or its first move)
static unsigned screen_changes;         // main thread

static CGDirectDisplayID display_of(NSScreen *screen) {
    return [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}

static bool is_game_window(id object) {
    Class sdl_window = NSClassFromString(@"SDLWindow");
    return sdl_window != nil && [object isKindOfClass:sdl_window];
}

// Moving to another display takes a few steps (leaving fullscreen, moving, fullscreen again, maybe a resolution
// change), so say where the window is once it has stayed put for a moment
static void game_window_moved(NSWindow *window) {
    unsigned change = ++screen_changes;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        NSScreen *screen = window.screen;
        if (change != screen_changes || screen == nil || display_of(screen) == shown_display)
            return;
        shown_display = display_of(screen);
        // where the window first goes isn't news
        if (now() - started < Startup_Display_Quiet)
            return;
        NSString *name = nil;
        if (@available(macOS 10.15, *))
            name = screen.localizedName;
        int hz = 0;
        if (@available(macOS 12.0, *))
            hz = (int)screen.maximumFramesPerSecond;
        if (hz > 0)
            notices_post("Game moved to %s, %d Hz", name.UTF8String ?: "another display", hz);
        else
            notices_post("Game moved to %s", name.UTF8String ?: "another display");
    });
}

#pragma mark - pausing

static void post_escape(bool down) {
    NSWindow *window = NSApp.keyWindow ?: NSApp.mainWindow;
    NSEvent *event = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint
                                 modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                                  windowNumber:window.windowNumber context:nil characters:@"\x1b"
                   charactersIgnoringModifiers:@"\x1b" isARepeat:NO keyCode:Key_Escape];
    [NSApp postEvent:event atStart:NO];
}

// SDL reads the key from the event queue on the main thread like a real one; the engine acts on the key going down
static void press_escape(const char *why) {
    dispatch_async(dispatch_get_main_queue(), ^{
        printf("BO3MacFix: %s, pressing Escape for the game's pause menu\n", why);
        post_escape(true);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            post_escape(false);
        });
    });
}

#pragma mark - Lua

// TakeNotice() -> the next notice's text and, for a lasting one (a warning), how many seconds to show it; or nil
static int lua_take_notice(lua_State *s) {
    char text[Notice_Text_Max];
    bool lasting = false;
    os_unfair_lock_lock(&lock);
    bool taken = notice_queue_pop(&queue, now(), Max_Notice_Age, text, sizeof(text), &lasting);
    os_unfair_lock_unlock(&lock);
    if (!taken) {
        hks_push_nil(s);
        return 1;
    }
    hks_push_string(s, text);
    if (!lasting)
        return 1;
    hks_push_number(s, Lasting_Show);
    return 2;
}

// TakePauseRequest() -> nil, or whether to pause whatever the last input was
static int lua_take_pause(lua_State *s) {
    os_unfair_lock_lock(&lock);
    double requested = pause_requested;
    bool forced = pause_forced;
    pause_requested = 0;
    pause_forced = false;
    os_unfair_lock_unlock(&lock);
    if (requested > 0 && now() - requested <= Max_Pause_Age)
        hks_push_boolean(s, forced);
    else
        hks_push_nil(s);
    return 1;
}

static int lua_pause_game(lua_State *s) {
    press_escape("a controller disconnected mid-match (PauseOnDisconnect)");
    return 0;
}

// PauseMenuOpen(1 or 0): whether the pause menu is open in a match, for PauseMenuFPS
static int lua_pause_menu_open(lua_State *s) {
    float open = 0;
    hks_number_arg(s, 1, &open);
    gpu_tweaks_pause_menu_open(open != 0);
    return 0;
}

// MenusOpen(1 or 0): whether the game is in the menus, outside a match, for MenuFPS, on every poll (BackgroundFPS
// waits for the polls)
static int lua_menus_open(lua_State *s) {
    float open = 0;
    hks_number_arg(s, 1, &open);
    gpu_tweaks_menus_open(open != 0);
    return 0;
}

// NoticeClock() -> seconds since the game started, for how long a toast has shown (LUI's timers run late under load)
static int lua_clock(lua_State *s) {
    hks_push_number(s, (float)(now() - started));
    return 1;
}

static int lua_log(lua_State *s) {
    const char *text = hks_check_string(s, 1);
    printf("BO3MacFix: %s\n", text != NULL ? text : "");
    return 0;
}

// runs notices.lua, which gets whether to log every menu made (BO3MACFIX_NOTICESDEBUG=1) and MenusOpen. MenusOpen is
// handed over here, when the first menu is made, rather than in start_lua: compiling or running any more Lua while the
// UI state is set up, as All-around Enhancement's scripts load, makes AAE quit the game at a map's load (see
// settings_menu.c). One more line in start_lua did.
static int lua_load_ui(lua_State *s) {
    ptrdiff_t top = HKS_TOP(s) - HKS_BASE(s);
    if (hks_load_source(s, (const char *)notices_lua, notices_lua_len, "=BO3MacFix notices")) {
        hks_push_boolean(s, debug_menus);
        hks_push_cfunction(s, lua_menus_open, "NoticesMenusOpen");
        hks_pcall(s, 2, 0);
    }
    HKS_TOP(s) = HKS_BASE(s) + top;
    return 0;
}

// Runs in each UI Lua state right after settings.c's table: adds our functions to the BO3MacFix table, and has the
// first menu made in the state load notices.lua, in a coroutine like settings_menu.c does with its page, and pass it
// every menu made after that (for the pause menu). It does nothing in a state that already has them.
static const char start_lua[] =
    "local log, take, take_pause, pause_game, clock, load_ui, pause_menu_open = ...\n"
    "local bo3macfix = rawget(_G, 'BO3MacFix')\n"
    "if bo3macfix == nil or rawget(bo3macfix, 'TakeNotice') ~= nil then return end\n"
    "if CoD == nil or CoD.Menu == nil or CoD.Menu.NewForUIEditor == nil then return end\n"
    "bo3macfix.TakeNotice, bo3macfix.TakePauseRequest, bo3macfix.PauseGame = take, take_pause, pause_game\n"
    "bo3macfix.NoticeLog, bo3macfix.NoticeClock, bo3macfix.PauseMenuOpen = log, clock, pause_menu_open\n"
    "local new_menu, loaded = CoD.Menu.NewForUIEditor, false\n"
    "CoD.Menu.NewForUIEditor = function(name, ...)\n"
    "  if not loaded then\n"
    "    loaded = true\n"
    "    local ok, err\n"
    "    if coroutine ~= nil then\n"
    "      ok, err = coroutine.resume(coroutine.create(function() load_ui() end))\n"
    "    else\n"
    "      ok, err = pcall(load_ui)\n"
    "    end\n"
    "    if not ok then log('notices error: ' .. tostring(err)) end\n"
    "  end\n"
    "  local menu = new_menu(name, ...)\n"
    "  local created = bo3macfix.NoticesMenuCreated\n"
    "  if created ~= nil then\n"
    "    local ok, err = pcall(created, name, menu)\n"
    "    if not ok then log('notices error: ' .. tostring(err)) end\n"
    "  end\n"
    "  return menu\n"
    "end\n";

void notices_lua_install(lua_State *s) {
    // settings_menu.c calls this once for each new UI Lua state: the old state's pause menu went with it, maybe
    // without its close running, and a map loading from the menus starts one
    gpu_tweaks_pause_menu_open(false);
    gpu_tweaks_ui_gone();
    // the stack can be reallocated while our Lua runs, so remember the top as an index
    ptrdiff_t top = HKS_TOP(s) - HKS_BASE(s);
    if (hks_load_source(s, start_lua, sizeof(start_lua) - 1, "=BO3MacFix notices start")) {
        hks_push_cfunction(s, lua_log, "NoticesLog");
        hks_push_cfunction(s, lua_take_notice, "NoticesTake");
        hks_push_cfunction(s, lua_take_pause, "NoticesTakePause");
        hks_push_cfunction(s, lua_pause_game, "NoticesPause");
        hks_push_cfunction(s, lua_clock, "NoticesClock");
        hks_push_cfunction(s, lua_load_ui, "NoticesLoadUI");
        hks_push_cfunction(s, lua_pause_menu_open, "NoticesPauseMenuOpen");
        hks_pcall(s, 7, 0);
    }
    HKS_TOP(s) = HKS_BASE(s) + top;
}

#pragma mark - setup

static NSMenuItem *find_key_equivalent(NSMenu *menu, NSString *key) {
    for (NSMenuItem *item in menu.itemArray) {
        if ([item.keyEquivalent isEqualToString:key] &&
            (item.keyEquivalentModifierMask & (NSEventModifierFlagCommand | NSEventModifierFlagShift |
                                               NSEventModifierFlagOption | NSEventModifierFlagControl)) == NSEventModifierFlagCommand)
            return item;
        NSMenuItem *found = item.hasSubmenu ? find_key_equivalent(item.submenu, key) : nil;
        if (found != nil)
            return found;
    }
    return nil;
}

static void on_test(const char *name, void (^handler)(uint64_t value)) {
    int token;
    uint32_t status = notify_register_dispatch(name, &token, dispatch_get_main_queue(), ^(int t) {
        uint64_t value = 0;
        notify_get_state(t, &value);
        handler(value);
    });
    if (status != NOTIFY_STATUS_OK)
        printf("notices tests: couldn't listen for %s\n", name);
}

static void setup_tests(void) {
    static NSObject *test_pad;
    test_pad = [NSObject new];
    on_test("com.bo3macfix.test.notice", ^(uint64_t value) {
        static int count = 0;
        notices_post("Test notice %d", ++count);
    });
    on_test("com.bo3macfix.test.controller", ^(uint64_t value) {
        if (value != 0)
            controller_connected(@"DualSense", 80, false, test_pad);
        else
            controller_disconnected(@"DualSense", true);
    });
    on_test("com.bo3macfix.test.battery", ^(uint64_t value) {
        check_battery(test_pad, @"DualSense", (int)(value % 1000), value >= 1000);
    });
    on_test("com.bo3macfix.test.escape", ^(uint64_t value) {
        press_escape("test");
    });
    on_test("com.bo3macfix.test.cmdq", ^(uint64_t value) {
        NSMenuItem *quit = find_key_equivalent(NSApp.mainMenu, @"q");
        printf("notices tests: Cmd+Q is \"%s\", action %s, target %s; NSApp is %s, its delegate %s\n",
               quit.title.UTF8String ?: "(no menu item)", quit.action != NULL ? sel_getName(quit.action) : "none",
               quit.target != nil ? object_getClassName(quit.target) : "the first responder", object_getClassName(NSApp),
               NSApp.delegate != nil ? object_getClassName(NSApp.delegate) : "none");
        if (quit != nil && quit.action != NULL)
            [NSApp sendAction:quit.action to:quit.target from:quit];
        else
            [NSApp terminate:nil];
    });
    on_test("com.bo3macfix.test.modwarning", ^(uint64_t value) {
        notices_mod_warning("Test.dll", "init");
    });
    printf("notices tests: test hooks on (com.bo3macfix.test.notice, controller, battery, escape, cmdq, modwarning)\n");
}

static void setup(bool tests) {
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    NSOperationQueue *main_queue = NSOperationQueue.mainQueue;
    [center addObserverForName:NSWindowDidChangeScreenNotification object:nil queue:main_queue usingBlock:^(NSNotification *note) {
        if (is_game_window(note.object))
            game_window_moved(note.object);
    }];
    [center addObserverForName:NSWindowDidBecomeKeyNotification object:nil queue:main_queue usingBlock:^(NSNotification *note) {
        NSWindow *window = note.object;
        if (shown_display == 0 && is_game_window(window) && window.screen != nil)
            shown_display = display_of(window.screen);
    }];

    // the GameController framework only when the bridge uses it, so SDL keeps controllers to itself otherwise
    if (gc_bridge_enabled()) {
        [center addObserverForName:GCControllerDidConnectNotification object:nil queue:main_queue usingBlock:^(NSNotification *note) {
            GCController *controller = note.object;
            if (controller.extendedGamepad == nil)
                return;
            bool charging;
            int percent = battery_percent(controller, &charging);
            controller_connected(controller_name(controller), percent, charging, controller);
        }];
        [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:main_queue usingBlock:^(NSNotification *note) {
            GCController *controller = note.object;
            if (controller.extendedGamepad != nil)
                controller_disconnected(controller_name(controller), false);
        }];
        static dispatch_source_t battery_timer;
        battery_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(battery_timer, dispatch_time(DISPATCH_TIME_NOW, Battery_Interval * NSEC_PER_SEC),
                                  Battery_Interval * NSEC_PER_SEC, 10 * NSEC_PER_SEC);
        dispatch_source_set_event_handler(battery_timer, ^{
            @autoreleasepool {
                check_batteries();
            }
        });
        dispatch_resume(battery_timer);
    }
    if (tests)
        setup_tests();
}

void notices_init(int notices, int pause_on_disconnect) {
    started = now();
    atomic_store(&notices_on, notices != 0);
    atomic_store(&pause_on, pause_on_disconnect != 0);
    const char *env = getenv("BO3MACFIX_NOTICESDEBUG");
    debug_menus = env != NULL && atoi(env) != 0;
    env = getenv("BO3MACFIX_TESTHOOKS");
    bool tests = env != NULL && atoi(env) != 0;
    // after gc_bridge_init's own setup, on the main thread once the game runs its event loop
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            setup(tests);
        }
    });
    printf("BO3MacFix: on-screen notices %s, pausing when the controller disconnects %s%s\n", notices ? "on" : "off",
           pause_on_disconnect ? "on" : "off", gc_bridge_enabled() ? "" : " (controller notices need ControllerBridge)");
}
