// Hooks for the display tests, only with BO3MACFIX_TESTHOOKS=1: Darwin notifications that drive the game without
// Accessibility permission, posted with notifyutil (notifyutil -s NAME VALUE sets a notification's state first):
//   com.bo3macfix.test.state         prints one "display state:" line
//   com.bo3macfix.test.gamedisplay   sets GameDisplay to the state (0/1/2), the same way the settings page does
//   com.bo3macfix.test.activate      brings the game to the front again, after the test switched to another app
//   com.bo3macfix.test.quit          quits through the game's own quit path, which saves config.ini
//   com.bo3macfix.test.key           presses the key with the state's macOS virtual key code (kVK_*)
//   com.bo3macfix.test.click         clicks the left mouse button in the middle of the game's window (fires a weapon:
//                                    the console's +attack doesn't, and AAE takes some free keys for its own binds)
//   com.bo3macfix.test.restart       restarts the game through Steam, as the settings page's restart does
//   com.bo3macfix.test.gpadscan      asks the game to look for controllers, as a controller connecting does
//   com.bo3macfix.test.command       runs the console commands in the file BO3MACFIX_TESTTEXT names (read when posted)
//   com.bo3macfix.test.screenshot    writes the game's next frame to the PNG that file names (metalfx.h): the picture
//                                    from inside the game, for when the test's own app can't record the screen. At
//                                    most 1600 pixels wide, unless its name ends in .full.png
//   com.bo3macfix.test.chat          sends that file's text as a chat message to everyone, through the chat box's own
//                                    send function, so mods that watch the chat (All-around Enhancement's /dep, /bal
//                                    and /save) see it as typed. Sent on the game's next client frame: that function
//                                    needs the game thread (its va() buffers), and crashes on Aspyr's main thread.
//   com.bo3macfix.test.pad           gives player 1 a controller in the state that file's text names, seven numbers:
//                                    buttons (XInput's bits, 0x1000 is A) left-x left-y right-x right-y (-32768 to
//                                    32767) left-trigger right-trigger (0 to 255). It stays so until the next one.
//                                    From the first one on SDL's keyboard focus is kept on the game's window, which
//                                    is what the game takes for being in front: otherwise it doesn't read
//                                    controllers in the menus, e.g. while the Mac's screen is locked. Needs
//                                    ControllerBridge.
//   com.bo3macfix.test.cheats        turns sv_cheats on, for setviewpos, god and notarget: a devmap start with
//                                    All-around Enhancement 4 leaves it off
//   com.bo3macfix.test.lua           runs that file's text as Lua in the UI's Lua VM, with two arguments: a function
//                                    that writes its string to the log as "test lua: ...", and one that releases the
//                                    registry reference with its number through the game's luaL_unref. Run from the
//                                    notices' poll (notices.lua), so within a quarter of a second, and only while that
//                                    runs.
// They arrive on the main queue, which Aspyr's main thread runs while it pumps events.
#import <AppKit/AppKit.h>
#include <notify.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "controllers.h"
#include "display_follow.h"
#include "display_testhooks.h"
#include "gc_bridge.h"
#include "gsc_obj.h"
#include "havok.h"
#include "metalfx.h"
#include "offsets.h"
#include "sdl_displays.h"
#include "settings.h"

static uint64_t base_address;
static void (*Cbuf_AddText)(int localClientNum, const char *text);
static void (*ChatClient_Send)(int localClientNum, const char *text, int channel);
static void (*hksi_luaL_unref)(lua_State *s, int index, int ref);
// a chat message for the game thread to send; the main thread fills it in while chat_pending is false
static char pending_chat[256];
static atomic_bool chat_pending;
// a Lua chunk for the UI's Lua VM to run, handed over the same way
static char pending_lua[16384];
static atomic_bool lua_pending;

static void on_notification(const char *name, void (^handler)(int token)) {
    int token;
    if (notify_register_dispatch(name, &token, dispatch_get_main_queue(), handler) != NOTIFY_STATUS_OK)
        printf("display tests: couldn't listen for %s\n", name);
}

// the text of the file BO3MACFIX_TESTTEXT names, without a trailing newline; false if there's none
static bool read_test_text(char *text, size_t size) {
    const char *path = getenv("BO3MACFIX_TESTTEXT");
    FILE *file = path != NULL ? fopen(path, "r") : NULL;
    if (file == NULL) {
        printf("display tests: no text file (BO3MACFIX_TESTTEXT)\n");
        return false;
    }
    // room for a newline the caller may add
    size_t length = fread(text, 1, size - 2, file);
    fclose(file);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r'))
        length--;
    text[length] = '\0';
    return length > 0;
}

// what the game's input frame asks before it reads the controllers: SDL's keyboard focus is the game's window
static void keep_game_in_front(void) {
    void *window = sdl_game_window();
    if (window != NULL)
        *(void *volatile *)(base_address + ADDR_SDL_KeyboardFocus) = window;
}

void display_testhooks_init(uint64_t game_base_address) {
    base_address = game_base_address;
    Cbuf_AddText = (void *)(game_base_address + ADDR_Cbuf_AddText);
    ChatClient_Send = (void *)(game_base_address + ADDR_ChatClient_Send);
    hksi_luaL_unref = (void *)(game_base_address + ADDR_hksi_luaL_unref);
    on_notification("com.bo3macfix.test.state", ^(int token) {
        display_follow_print_state("test");
    });
    on_notification("com.bo3macfix.test.gamedisplay", ^(int token) {
        uint64_t value = 0;
        notify_get_state(token, &value);
        printf("display tests: GameDisplay = %llu\n", value);
        display_follow_set_option("GameDisplay", (int)value);
    });
    on_notification("com.bo3macfix.test.activate", ^(int token) {
        [NSApp activateIgnoringOtherApps:YES];
    });
    on_notification("com.bo3macfix.test.quit", ^(int token) {
        printf("display tests: quitting\n");
        Cbuf_AddText(0, "quit\n");
    });
    // presses the key whose macOS virtual key code (kVK_*) is the state, for driving menus such as the settings page:
    // SDL reads key events as its event pump gets them, so posting them to the app is enough
    on_notification("com.bo3macfix.test.key", ^(int token) {
        uint64_t key = 0;
        notify_get_state(token, &key);
        NSWindow *window = [NSApp keyWindow] ?: [NSApp mainWindow];
        NSEvent *(^key_event)(NSEventType) = ^(NSEventType type) {
            return [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0
                                   timestamp:[[NSProcessInfo processInfo] systemUptime]
                                windowNumber:window.windowNumber context:nil characters:@""
                 charactersIgnoringModifiers:@"" isARepeat:NO keyCode:(unsigned short)key];
        };
        printf("display tests: key %llu\n", key);
        [NSApp postEvent:key_event(NSEventTypeKeyDown) atStart:NO];
        // released a few frames later, so a game that reads the key's state each frame sees it down
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            [NSApp postEvent:key_event(NSEventTypeKeyUp) atStart:NO];
        });
    });
    // the same for the left mouse button, at the middle of the window's content: SDL's window listener takes the
    // press as a click in the window, as it does a real one
    on_notification("com.bo3macfix.test.click", ^(int token) {
        NSWindow *window = [NSApp keyWindow] ?: [NSApp mainWindow];
        NSRect content = window.contentView.frame;
        NSPoint middle = NSMakePoint(NSMidX(content), NSMidY(content));
        NSEvent *(^mouse_event)(NSEventType) = ^(NSEventType type) {
            return [NSEvent mouseEventWithType:type location:middle modifierFlags:0
                                     timestamp:[[NSProcessInfo processInfo] systemUptime]
                                  windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:1
                                      pressure:(type == NSEventTypeLeftMouseDown ? 1 : 0)];
        };
        printf("display tests: click\n");
        [NSApp postEvent:mouse_event(NSEventTypeLeftMouseDown) atStart:NO];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            [NSApp postEvent:mouse_event(NSEventTypeLeftMouseUp) atStart:NO];
        });
    });
    on_notification("com.bo3macfix.test.restart", ^(int token) {
        printf("display tests: restarting\n");
        settings_restart_game();
    });
    on_notification("com.bo3macfix.test.gpadscan", ^(int token) {
        printf("display tests: controller scan\n");
        controllers_request_gamepad_scan("test");
    });
    on_notification("com.bo3macfix.test.command", ^(int token) {
        char text[1024];
        if (!read_test_text(text, sizeof(text)))
            return;
        printf("display tests: commands %s\n", text);
        // ending in a newline, so the last command runs
        strlcat(text, "\n", sizeof(text));
        Cbuf_AddText(0, text);
    });
    on_notification("com.bo3macfix.test.chat", ^(int token) {
        if (atomic_load_explicit(&chat_pending, memory_order_acquire)) {
            printf("display tests: the last chat message isn't sent yet\n");
            return;
        }
        if (read_test_text(pending_chat, sizeof(pending_chat)))
            atomic_store_explicit(&chat_pending, true, memory_order_release);
    });
    on_notification("com.bo3macfix.test.pad", ^(int token) {
        char text[128];
        unsigned buttons = 0;
        int left_x = 0, left_y = 0, right_x = 0, right_y = 0, left_trigger = 0, right_trigger = 0;
        if (!read_test_text(text, sizeof(text)) ||
            sscanf(text, "%i %d %d %d %d %d %d", (int *)&buttons, &left_x, &left_y, &right_x, &right_y, &left_trigger,
                   &right_trigger) < 1)
            return;
        static bool scanned = false;
        printf("display tests: pad %s\n", text);
        gc_bridge_test_pad((uint16_t)buttons, (int16_t)left_x, (int16_t)left_y, (int16_t)right_x, (int16_t)right_y,
                           (uint8_t)left_trigger, (uint8_t)right_trigger);
        if (!scanned) {
            scanned = true;
            controllers_request_gamepad_scan("test");
            // SDL takes the focus away again whenever macOS tells it so
            static dispatch_source_t timer;
            timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
            dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(timer, ^{
                keep_game_in_front();
            });
            dispatch_resume(timer);
        }
        keep_game_in_front();
    });
    on_notification("com.bo3macfix.test.cheats", ^(int token) {
        void *(*find)(uint32_t hash) = (void *)(base_address + ADDR_Dvar_FindByHash);
        void (*set_bool)(void *dvar, bool value) = (void *)(base_address + ADDR_Dvar_SetBool);
        void *cheats = find(gsc_hash("sv_cheats"));
        printf("display tests: sv_cheats %s\n", cheats != NULL ? "on" : "not found");
        if (cheats != NULL)
            set_bool(cheats, true);
    });
    on_notification("com.bo3macfix.test.lua", ^(int token) {
        if (atomic_load_explicit(&lua_pending, memory_order_acquire)) {
            printf("display tests: the last Lua chunk hasn't run yet\n");
            return;
        }
        if (read_test_text(pending_lua, sizeof(pending_lua)))
            atomic_store_explicit(&lua_pending, true, memory_order_release);
    });
    on_notification("com.bo3macfix.test.screenshot", ^(int token) {
        char path[1024];
        if (!read_test_text(path, sizeof(path)))
            return;
        printf("display tests: screenshot to %s\n", path);
        metalfx_request_screenshot(path);
    });
    printf("display tests: test hooks on (com.bo3macfix.test.*)\n");
}

// each client frame, on the game thread (CL_SetCGameTime's hook): sends a chat message com.bo3macfix.test.chat queued
void display_testhooks_frame(int localClientNum) {
    if (!atomic_load_explicit(&chat_pending, memory_order_acquire))
        return;
    printf("display tests: chat %s\n", pending_chat);
    ChatClient_Send(localClientNum, pending_chat, 0);
    atomic_store_explicit(&chat_pending, false, memory_order_release);
}

static int lua_test_log(lua_State *s) {
    printf("test lua: %s\n", hks_check_string(s, 1));
    return 0;
}

#define Lua_Registry (-10000)

static int lua_test_unref(lua_State *s) {
    float ref = -1;
    if (hks_number_arg(s, 1, &ref) && hksi_luaL_unref != NULL) {
        printf("test lua: releasing reference %d\n", (int)ref);
        hksi_luaL_unref(s, Lua_Registry, (int)ref);
    }
    return 0;
}

// from the UI's Lua VM, on every poll of the notices: runs the chunk com.bo3macfix.test.lua queued
void display_testhooks_lua(lua_State *s) {
    if (!atomic_load_explicit(&lua_pending, memory_order_acquire))
        return;
    printf("display tests: lua, %zu bytes\n", strlen(pending_lua));
    if (hks_load_source(s, pending_lua, strlen(pending_lua), "=test")) {
        hks_push_cfunction(s, lua_test_log, "TestLog");
        hks_push_cfunction(s, lua_test_unref, "TestUnref");
        hks_pcall(s, 2, 0);
    }
    atomic_store_explicit(&lua_pending, false, memory_order_release);
}
