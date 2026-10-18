// Hooks for the display tests, only with BO3MACFIX_TESTHOOKS=1: Darwin notifications that drive the game without
// Accessibility permission, posted with notifyutil (notifyutil -s NAME VALUE sets a notification's state first):
//   com.bo3macfix.test.state         prints one "display state:" line
//   com.bo3macfix.test.gamedisplay   sets GameDisplay to the state (0/1/2), the same way the settings page does
//   com.bo3macfix.test.activate      brings the game to the front again, after the test switched to another app
//   com.bo3macfix.test.quit          quits through the game's own quit path, which saves config.ini
//   com.bo3macfix.test.gpadscan      asks the game to look for controllers, as a controller connecting does
// They arrive on the main queue, which Aspyr's main thread runs while it pumps events.
#import <AppKit/AppKit.h>
#include <notify.h>
#include <stdio.h>
#include "controllers.h"
#include "display_follow.h"
#include "display_testhooks.h"
#include "offsets.h"

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
    on_notification("com.bo3macfix.test.gpadscan", ^(int token) {
        printf("display tests: controller scan\n");
        controllers_request_gamepad_scan("test");
    });
    printf("display tests: test hooks on (com.bo3macfix.test.*)\n");
}
