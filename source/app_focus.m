// See app_focus.h. AppKit posts a notification for each way the game can go to the background or come back, and this
// looks at all of them again each time, on the main thread.
#import <AppKit/AppKit.h>
#include <stdio.h>
#include "app_focus.h"

static void (*on_change)(bool background);
static int last = -1; // what on_change was last told, -1 before the first time

static void evaluate(void) {
    bool active = [NSApp isActive], hidden = [NSApp isHidden];
    bool visible = ([NSApp occlusionState] & NSApplicationOcclusionStateVisible) != 0;
    bool background = !active || hidden || !visible;
    if ((int)background == last)
        return;
    last = background;
    printf("BO3MacFix: the game is %s (active %i, hidden %i, a window visible %i)\n",
           background ? "in the background" : "in front", active, hidden, visible);
    on_change(background);
}

static void start(void) {
    if (NSApp == nil) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
            start();
        });
        return;
    }
    NSNotificationName names[] = {
        NSApplicationDidBecomeActiveNotification, NSApplicationDidResignActiveNotification,
        NSApplicationDidHideNotification,         NSApplicationDidUnhideNotification,
        NSApplicationDidChangeOcclusionStateNotification,
        NSWindowDidMiniaturizeNotification,       NSWindowDidDeminiaturizeNotification,
    };
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        [center addObserverForName:names[i] object:nil queue:[NSOperationQueue mainQueue]
                        usingBlock:^(NSNotification *note) {
                            evaluate();
                        }];
    }
    evaluate();
}

void app_focus_watch(void (*changed)(bool background)) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        on_change = changed;
        dispatch_async(dispatch_get_main_queue(), ^{
            start();
        });
    });
}
