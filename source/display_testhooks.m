// Hooks for the display tests, only with BO3MACFIX_TESTHOOKS=1: Darwin notifications that drive the game without
// Accessibility permission, posted with notifyutil (notifyutil -s NAME VALUE sets a notification's state first):
//   com.bo3macfix.test.state         prints one "display state:" line
//   com.bo3macfix.test.gamedisplay   sets GameDisplay to the state (0/1/2), the same way the settings page does
//   com.bo3macfix.test.activate      brings the game to the front again, after the test switched to another app
//   com.bo3macfix.test.quit          quits through the game's own quit path, which saves config.ini
//   com.bo3macfix.test.key           presses the key with the state's macOS virtual key code (kVK_*)
//   com.bo3macfix.test.restart       restarts the game through Steam, as the settings page's restart does
// They arrive on the main queue, which Aspyr's main thread runs while it pumps events.
#import <AppKit/AppKit.h>
#include <notify.h>
#include <stdio.h>
#include "display_follow.h"
#include "display_testhooks.h"
#include "offsets.h"
#include "settings.h"

static void (*Cbuf_AddText)(int localClientNum, const char *text);

static void on_notification(const char *name, void (^handler)(int token)) {
    int token;
    if (notify_register_dispatch(name, &token, dispatch_get_main_queue(), handler) != NOTIFY_STATUS_OK)
        printf("display tests: couldn't listen for %s\n", name);
}

void display_testhooks_init(uint64_t game_base_address) {
    Cbuf_AddText = (void *)(game_base_address + ADDR_Cbuf_AddText);
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
    printf("display tests: test hooks on (com.bo3macfix.test.*)\n");
}
