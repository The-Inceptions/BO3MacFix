// The AppKit half of sdl_displays.c: screens by display, the game window's frame, and the fix for Aspyr's
// -[SDLWindow constrainFrameRect:toScreen:]. Main thread only.
#import <AppKit/AppKit.h>
#include <objc/runtime.h>
#include <stdio.h>
#include "sdl_displays.h"

static NSScreen *screen_for_display(uint32_t display) {
    for (NSScreen *screen in [NSScreen screens])
        if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == display)
            return screen;
    return nil;
}

double sdl_cocoa_screen(uint32_t display, char *name, size_t n) {
    @autoreleasepool {
        NSScreen *screen = screen_for_display(display);
        NSString *text = nil;
        if (@available(macOS 10.15, *))
            text = screen.localizedName;
        snprintf(name, n, "%s", text != nil ? text.UTF8String : "");
        if (name[0] == 0)
            snprintf(name, n, "display %u", display);
        return screen != nil ? screen.backingScaleFactor : 1.0;
    }
}

void sdl_cocoa_move_to_screen(void *nswindow, uint32_t display) {
    @autoreleasepool {
        NSWindow *window = (NSWindow *)nswindow;
        NSScreen *screen = screen_for_display(display);
        if (screen != nil && window.screen != screen)
            [window setFrame:screen.frame display:NO];
    }
}

uint32_t sdl_cocoa_window_display(void *nswindow) {
    @autoreleasepool {
        NSScreen *screen = ((NSWindow *)nswindow).screen;
        return screen != nil ? [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] : 0;
    }
}

void sdl_cocoa_describe(void *nswindow, char *buf, size_t n) {
    @autoreleasepool {
        NSWindow *window = (NSWindow *)nswindow;
        NSRect frame = window.frame, screen = window.screen != nil ? window.screen.frame : NSZeroRect;
        snprintf(buf, n, "frame=%.0f,%.0f,%.0f,%.0f screen=%.0f,%.0f,%.0f,%.0f", frame.origin.x, frame.origin.y,
            frame.size.width, frame.size.height, screen.origin.x, screen.origin.y, screen.size.width, screen.size.height);
    }
}

typedef NSRect (*constrain_fn)(id, SEL, NSRect, NSScreen *);
static constrain_fn aspyr_constrain, window_constrain;

// Aspyr's version keeps the window on SDL display 0. It finds display 0's screen by comparing display 0's bounds (CG
// coordinates, from the top left of the main display) with each screen's frame (Cocoa coordinates, from the bottom
// left), which only matches for the main display; for any other display 0 it clamps in the wrong coordinate space and
// moves the window there 0.2 s later. So for those, clamp onto display 0's screen here, in Cocoa coordinates.
static NSRect constrain_frame(id self, SEL sel, NSRect frame, NSScreen *screen) {
    uint32_t display = sdl_displays_id(0);
    if (display == 0 || CGDisplayIsMain(display))
        return aspyr_constrain(self, sel, frame, screen);
    NSRect rect = window_constrain(self, sel, frame, screen);
    NSScreen *target = screen_for_display(display);
    if (target == nil || screen == target)
        return rect;
    NSRect bounds = target.frame;
    rect.origin.x = MAX(bounds.origin.x, MIN(rect.origin.x, NSMaxX(bounds) - rect.size.width));
    rect.origin.y = MAX(bounds.origin.y, MIN(rect.origin.y, NSMaxY(bounds) - rect.size.height));
    return rect;
}

void sdl_cocoa_init(void) {
    Class sdl_window = objc_getClass("SDLWindow");
    SEL sel = @selector(constrainFrameRect:toScreen:);
    Method method = sdl_window != Nil ? class_getInstanceMethod(sdl_window, sel) : NULL;
    IMP inherited = method_getImplementation(class_getInstanceMethod([NSWindow class], sel));
    // only SDLWindow's own version; class_getInstanceMethod falls back to NSWindow's, which mustn't be replaced
    if (method == NULL || method_getImplementation(method) == inherited) {
        printf("display: SDLWindow has no constrainFrameRect:toScreen: of its own, not fixing it\n");
        return;
    }
    window_constrain = (constrain_fn)inherited;
    aspyr_constrain = (constrain_fn)method_setImplementation(method, (IMP)constrain_frame);
}
