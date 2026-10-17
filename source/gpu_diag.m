// GpuDiagnostics = 1: logs how the game's frames get to the screen (its window, the display mode and the Metal layer)
// whenever that changes, for example whether Fullscreen switched the display's mode.
//
// FullScreenMode = 1 (Fullscreen) is SDL 2.0.7's real fullscreen: it captures the displays, puts the window at the
// shielding level and switches the display to a mode of the game's resolution (Cocoa_SetDisplayMode at 0x100147770,
// Cocoa_SetWindowFullscreen at 0x10014e5e0). FullScreenMode = 2 (Fullscreen Window) is a fullscreen Space at the
// desktop's mode (toggleFullScreen: at 0x10014a261).
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpu_diag.h"

static NSString *last_state = nil;
static CAMetalLayer *find_metal_layer(CALayer *layer) {
    if ([layer isKindOfClass:[CAMetalLayer class]])
        return (CAMetalLayer *)layer;
    for (CALayer *sublayer in layer.sublayers) {
        CAMetalLayer *found = find_metal_layer(sublayer);
        if (found != nil)
            return found;
    }
    return nil;
}

static void log_state(void) {
    @autoreleasepool {
        NSMutableString *state = [NSMutableString string];
        for (NSWindow *window in [NSApp windows]) {
            CAMetalLayer *layer = window.visible ? find_metal_layer(window.contentView.layer) : nil;
            if (layer == nil)
                continue;
            NSScreen *screen = window.screen;
            CGDirectDisplayID display = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
            NSString *mode_text = mode == NULL ? @"?" :
                [NSString stringWithFormat:@"%zux%zu (%zux%zu pixels)", CGDisplayModeGetWidth(mode), CGDisplayModeGetHeight(mode),
                    CGDisplayModeGetPixelWidth(mode), CGDisplayModeGetPixelHeight(mode)];
            if (mode != NULL)
                CGDisplayModeRelease(mode);
            NSString *level = window.level == CGShieldingWindowLevel() ? @"shielding (displays captured)" :
                [NSString stringWithFormat:@"%ld", (long)window.level];
            [state appendFormat:@"window %.0fx%.0f, level %@%@, screen %.0fx%.0f at %.0fx, display mode %@",
                window.frame.size.width, window.frame.size.height, level,
                (window.styleMask & NSWindowStyleMaskFullScreen) ? @", fullscreen Space" : @"",
                screen.frame.size.width, screen.frame.size.height, screen.backingScaleFactor, mode_text];
            if (@available(macOS 12.0, *)) {
                if (screen.safeAreaInsets.top > 0)
                    [state appendFormat:@" (camera housing %.0f points)", screen.safeAreaInsets.top];
            }
            unsigned long drawables = 3;
            if (@available(macOS 10.13.2, *))
                drawables = layer.maximumDrawableCount;
            [state appendFormat:@"; Metal layer %.0fx%.0f at %.0fx, drawable %.0fx%.0f, %@, %@, pixel format %lu, vsync %@, %lu drawables",
                layer.bounds.size.width, layer.bounds.size.height, layer.contentsScale,
                layer.drawableSize.width, layer.drawableSize.height, layer.opaque ? @"opaque" : @"transparent",
                layer.framebufferOnly ? @"framebuffer only" : @"not framebuffer only", (unsigned long)layer.pixelFormat,
                layer.displaySyncEnabled ? @"on" : @"off", drawables];
        }
        if (state.length > 0 && ![state isEqualToString:last_state]) {
            printf("GPU: %s\n", state.UTF8String);
            [last_state release];
            last_state = [state copy];
        }
    }
}

void gpu_diag_init(int enabled) {
    const char *env = getenv("BO3MACFIX_GPUDIAGNOSTICS");
    if (env != NULL)
        enabled = atoi(env);
    if (!enabled)
        return;
    // checked every 2 seconds on the main thread, which is where AppKit wants to be asked
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 4);
    dispatch_source_set_event_handler(timer, ^{
        log_state();
    });
    dispatch_resume(timer);
    printf("GPU diagnostics on, logging how frames get to the screen\n");
}
