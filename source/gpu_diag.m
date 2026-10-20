// GpuDiagnostics = 1: logs how the game's frames get to the screen (its window, the display and its mode, the Metal layer
// and MetalFX) whenever that changes, for example when the game window moves to another display.
//
// In this port both FullScreenModes keep the display at the desktop's mode: Aspyr sets SDL_ALLOW_TOPMOST and
// SDL_VIDEO_MAC_FULLSCREEN_SPACES to 0 (0x100100dea) and only asks SDL for FULLSCREEN_DESKTOP (0x1000edc4e), so SDL never
// captures the displays, switches their mode or uses a fullscreen Space.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <math.h>
#include <objc/runtime.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "gpu_diag.h"
#include "metalfx.h"

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
            double mode_refresh = mode != NULL ? CGDisplayModeGetRefreshRate(mode) : 0;
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
            int fps = (int)lround(mode_refresh), variable = 0;
            if (@available(macOS 12.0, *)) {
                fps = (int)screen.maximumFramesPerSecond;
                variable = screen.minimumRefreshInterval < screen.maximumRefreshInterval;
            }
            char present[96];
            present_describe(present, sizeof(present));
            [state appendFormat:@"; display %u, %i Hz%@, %s", display, fps, variable ? @" (variable)" : @"", present];
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
    // and straight away when the game window moves to another screen or gets another backing scale, or a screen changes,
    // once whatever else is told about it (MetalFX resizing the drawable) has run
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    for (NSString *name in @[NSWindowDidChangeScreenNotification, NSWindowDidChangeBackingPropertiesNotification,
                             NSApplicationDidChangeScreenParametersNotification]) {
        [center addObserverForName:name object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *notification) {
            dispatch_async(dispatch_get_main_queue(), ^{
                log_state();
            });
        }];
    }
    printf("GPU diagnostics on, logging how frames get to the screen\n");
}

// FrameStats' detail (BO3MACFIX_FRAMESTATSDETAIL=1): how long the GPU was busy with the game's command buffers. Every
// committed command buffer gets a completed handler that adds its GPU time, less any overlap with the ones before it,
// so the total is the time the GPU spent on at least one of the game's command buffers.
static os_unfair_lock busy_lock = OS_UNFAIR_LOCK_INIT;
static double busy_seconds = 0, busy_last_end = 0, busy_total = 0; // busy_total is never reset (DynamicResolution)
static uint64_t busy_buffers = 0;
static IMP orig_commit = NULL;

// GPUStartTime and GPUEndTime need macOS 10.15, gpu_diag_watch_command_buffers checks before it swizzles
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
static void commit_new(id self, SEL sel) {
    [(id<MTLCommandBuffer>)self addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        CFTimeInterval start = buffer.GPUStartTime, end = buffer.GPUEndTime;
        if (start <= 0 || end <= start)
            return;
        os_unfair_lock_lock(&busy_lock);
        if (start < busy_last_end)
            start = busy_last_end;
        if (end > start) {
            busy_seconds += end - start;
            busy_total += end - start;
        }
        if (end > busy_last_end)
            busy_last_end = end;
        busy_buffers++;
        os_unfair_lock_unlock(&busy_lock);
    }];
    ((void (*)(id, SEL))orig_commit)(self, sel);
}
#pragma clang diagnostic pop

void gpu_diag_watch_command_buffers(void *command_buffer) {
    static atomic_flag watching = ATOMIC_FLAG_INIT;
    if (command_buffer == NULL || atomic_flag_test_and_set(&watching))
        return;
    if (!@available(macOS 10.15, *))
        return;
    Method commit = class_getInstanceMethod(object_getClass((id)command_buffer), @selector(commit));
    if (commit != NULL)
        orig_commit = method_setImplementation(commit, (IMP)commit_new);
    printf("frame stats: timing the GPU's work (%s)\n", commit != NULL ? class_getName(object_getClass((id)command_buffer)) : "not found");
}

double gpu_diag_take_busy(uint64_t *buffers) {
    os_unfair_lock_lock(&busy_lock);
    double seconds = busy_seconds;
    *buffers = busy_buffers;
    busy_seconds = 0;
    busy_buffers = 0;
    os_unfair_lock_unlock(&busy_lock);
    return seconds;
}

double gpu_diag_busy_total(void) {
    os_unfair_lock_lock(&busy_lock);
    double seconds = busy_total;
    os_unfair_lock_unlock(&busy_lock);
    return seconds;
}
