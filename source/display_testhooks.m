// Hooks for the display tests, only with BO3MACFIX_TESTHOOKS=1: Darwin notifications that drive the game without
// Accessibility permission, posted with notifyutil (notifyutil -s NAME VALUE sets a notification's state first):
//   com.bo3macfix.test.state         prints one "display state:" line
//   com.bo3macfix.test.gamedisplay   sets GameDisplay to the state (0/1/2), the same way the settings page does
//   com.bo3macfix.test.activate      brings the game to the front again, after the test switched to another app
//   com.bo3macfix.test.quit          quits through the game's own quit path, which saves config.ini
//   com.bo3macfix.test.key           presses the key with the state's macOS virtual key code (kVK_*)
//   com.bo3macfix.test.restart       restarts the game through Steam, as the settings page's restart does
//   com.bo3macfix.test.gpadscan      asks the game to look for controllers, as a controller connecting does
//   com.bo3macfix.test.command       runs the console commands in the file BO3MACFIX_TESTTEXT names (read when posted)
//   com.bo3macfix.test.chat          sends that file's text as a chat message to everyone, through the chat box's own
//                                    send function, so mods that watch the chat (All-around Enhancement's /dep, /bal
//                                    and /save) see it as typed. Sent on the game's next client frame: that function
//                                    needs the game thread (its va() buffers), and crashes on Aspyr's main thread.
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
#include "offsets.h"
#include "settings.h"

static void (*Cbuf_AddText)(int localClientNum, const char *text);
static void (*ChatClient_Send)(int localClientNum, const char *text, int channel);
// a chat message for the game thread to send; the main thread fills it in while chat_pending is false
static char pending_chat[256];
static atomic_bool chat_pending;

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

void display_testhooks_init(uint64_t game_base_address) {
    Cbuf_AddText = (void *)(game_base_address + ADDR_Cbuf_AddText);
    ChatClient_Send = (void *)(game_base_address + ADDR_ChatClient_Send);
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
