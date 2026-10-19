// MetalFX spatial upscaling of the whole frame in Aspyr's Metal presenter (its D3D Present is at 0x1000e8b78).
//
// Normally Aspyr sizes the drawable to the game's resolution (ASL_MetalView forceDrawableSizeWidth:height:) and
// Core Animation stretches it to the screen. With this on, the drawable stays at the screen's native size instead,
// Aspyr's 1:1 copy of the backbuffer into the drawable copies into our own texture, and right before the drawable is
// presented MetalFX upscales that into the drawable. If the game already renders at the screen's size nothing changes.
//
// Aspyr's view only resizes the drawable when its own size changes, so this also does it when the game window moves to
// another screen or gets another backing scale (a Retina screen and a 1x one can be the same size in points).
//
// MetalFXUpscale = 2 is for scaled HiDPI modes, like looks like 2560x1440 on a 3840x2160 panel: the window then has
// 5120x2880 pixels, more than the panel shows, so MetalFX upscales to the panel's 3840x2160 instead and Core Animation
// and the display scale that the rest of the way, as they would any drawable smaller than the window.
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display_follow.h"
#include "metalfx.h"
#include "notch.h"
#include "notch_layout.h"

// MetalFX needs macOS 13 and is weak-linked, metalfx_init checks it's there before anything here uses it
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

typedef void (*copy_texture_fn)(id, SEL, id, NSUInteger, NSUInteger, MTLOrigin, MTLSize, id, NSUInteger, NSUInteger, MTLOrigin);

static int metalfx_ready = 0; // MetalFX is there and the drawable size hook is installed
static int metalfx_mode = 1; // MetalFXUpscale: 1 upscales to the window's pixels, 2 to the panel's in scaled modes

// ASL_MetalView
static IMP orig_updateDrawableSize = NULL, nsview_viewDidChangeBackingProperties = NULL;
static ptrdiff_t view_size_offset, forced_width_offset, forced_height_offset;
static SEL sel_metalLayer, sel_updateDrawableSize;
static id game_view = nil; // a weak reference (objc_storeWeak), the view whose drawable we size

// the game window's screen as the main thread last saw it. Aspyr also sizes the drawable from its render thread, where
// AppKit can't be asked, so that goes by the scale from here.
static _Atomic double screen_scale = 0; // 0 until the main thread has seen the view in a window
static atomic_int screen_max_fps = 0, screen_variable_refresh = 0;
static atomic_uint screen_display = 0; // its CGDirectDisplayID
static atomic_uint screen_native = 0; // width << 16 | height of its panel's own resolution, 0 if not known

// sizes packed 16 bits each (pack_sizes): the game's size and drawable size MetalFX couldn't do, which keep the drawable
// at the game's size until either changes, the last ones we sized the drawable for, and the last frame's upscale for
// present_describe (0 when it wasn't upscaled)
static _Atomic uint64_t failed_sizes = 0, resized_sizes = 0, frame_sizes = 0;
static atomic_uint frame_drawable_size = 0; // width << 16 | height of the last frame's drawable
static atomic_uint view_points = 0; // width << 16 | height of the view in points

// only touched on Aspyr's render thread, in its present function
static id<MTLFXSpatialScaler> scaler = nil;
static id<MTLTexture> input_texture = nil, output_texture = nil;
static NSUInteger input_width, input_height, output_width, output_height;
static MTLPixelFormat input_format, output_format;
static NSMutableArray *retired = nil; // the scaler and textures from before a resize, until the GPU is done with them
static id<MTLTexture> pending_input = nil, pending_drawable_texture = nil; // set by the copy, used by the present
static MTLOrigin pending_origin; // where the upscaled frame goes in the drawable

static uint64_t pack_sizes(NSUInteger width1, NSUInteger height1, NSUInteger width2, NSUInteger height2) {
    return (uint64_t)(width1 & 0xFFFF) << 48 | (uint64_t)(height1 & 0xFFFF) << 32 | (uint64_t)(width2 & 0xFFFF) << 16 |
           (uint64_t)(height2 & 0xFFFF);
}

// a display's panel resolution, width << 16 | height: the mode macOS marks as native, else the widest one that isn't
// HiDPI. For when display_follow doesn't have the display.
static unsigned int panel_pixels(CGDirectDisplayID display) {
    CFStringRef keys[] = { kCGDisplayShowDuplicateLowResolutionModes };
    CFBooleanRef values[] = { kCFBooleanTrue };
    CFDictionaryRef options = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 1,
                                                 &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, options);
    CFRelease(options);
    size_t width = 0, height = 0, widest_width = 0, widest_height = 0;
    for (CFIndex i = 0; modes != NULL && i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        size_t w = CGDisplayModeGetPixelWidth(mode), h = CGDisplayModeGetPixelHeight(mode);
        if (CGDisplayModeGetIOFlags(mode) & kDisplayModeNativeFlag) {
            width = w;
            height = h;
        }
        if (w == CGDisplayModeGetWidth(mode) && w > widest_width) {
            widest_width = w;
            widest_height = h;
        }
    }
    if (modes != NULL)
        CFRelease(modes);
    if (width == 0) {
        width = widest_width;
        height = widest_height;
    }
    return (unsigned int)(width & 0xFFFF) << 16 | (unsigned int)(height & 0xFFFF);
}

// the panel resolution of the game window's screen: display_follow's when it's describing that display, else ours
static unsigned int panel_native(void) {
    DisplayInfo display;
    unsigned int id = atomic_load(&screen_display);
    if (display_follow_current(&display) == 1 && display.native_w > 0 && display.native_h > 0 && (id == 0 || display.id == id))
        return (unsigned int)(display.native_w & 0xFFFF) << 16 | (unsigned int)(display.native_h & 0xFFFF);
    return atomic_load(&screen_native);
}

// what MetalFX upscales to for a window with this many pixels: all of them, or with MetalFXUpscale = 2 in a scaled
// mode the panel's resolution (the window's size shrunk to fit in it)
static void output_size(int backing_width, int backing_height, int *width, int *height) {
    *width = backing_width;
    *height = backing_height;
    if (metalfx_mode != 2)
        return;
    unsigned int panel = panel_native();
    int panel_width = (int)(panel >> 16), panel_height = (int)(panel & 0xFFFF);
    if (panel_width <= 0 || panel_height <= 0 || backing_width <= panel_width || backing_height <= panel_height)
        return;
    double scale = fmin((double)panel_width / backing_width, (double)panel_height / backing_height);
    *width = (int)lround(backing_width * scale);
    *height = (int)lround(backing_height * scale);
}

// what the main thread knows about the game window's screen, with a line in the log whenever that changes
static void remember_screen(NSView *view) {
    NSWindow *window = view.window;
    NSScreen *screen = window.screen;
    if (screen == nil) // between screens, keep what we had
        return;
    atomic_store(&screen_scale, window.backingScaleFactor);
    CGDirectDisplayID display = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    int fps = 0, variable = 0;
    if (@available(macOS 12.0, *)) {
        fps = (int)screen.maximumFramesPerSecond;
        variable = screen.minimumRefreshInterval < screen.maximumRefreshInterval;
    }
    if (fps <= 0) {
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
        if (mode != NULL) {
            fps = (int)lround(CGDisplayModeGetRefreshRate(mode));
            CGDisplayModeRelease(mode);
        }
    }
    atomic_store(&screen_max_fps, fps > 0 ? fps : 60);
    atomic_store(&screen_variable_refresh, variable);

    static char last[128];
    char line[128];
    snprintf(line, sizeof(line), "present: the game window is on display %u, %.0fx%.0f pixels at %.0fx, %i Hz%s", display,
             view.bounds.size.width * window.backingScaleFactor, view.bounds.size.height * window.backingScaleFactor,
             window.backingScaleFactor, fps > 0 ? fps : 60, variable ? " (variable)" : "");
    if (strcmp(line, last) != 0) {
        printf("%s\n", line);
        strcpy(last, line);
    }

    // a scaled HiDPI mode gives the window more pixels than the panel has, and the display scales them down to it
    if (display != atomic_exchange(&screen_display, display))
        atomic_store(&screen_native, panel_pixels(display));
    unsigned int panel = panel_native();
    int panel_width = (int)(panel >> 16), panel_height = (int)(panel & 0xFFFF);
    int backing_width = (int)lround(view.bounds.size.width * window.backingScaleFactor);
    int backing_height = (int)lround(view.bounds.size.height * window.backingScaleFactor);
    static char last_scaled[256];
    char scaled[256] = "";
    if (panel_width > 0 && backing_width > panel_width && backing_height > panel_height) {
        char name[64] = "the display";
        DisplayInfo info;
        if (display_follow_current(&info) == 1 && info.id == display && info.name[0] != 0)
            snprintf(name, sizeof(name), "%s", info.name);
        else if (@available(macOS 10.15, *))
            snprintf(name, sizeof(name), "%s", screen.localizedName.UTF8String ?: "the display");
        int width, height;
        output_size(backing_width, backing_height, &width, &height);
        if (metalfx_mode == 2)
            snprintf(scaled, sizeof(scaled), "present: %s is in a scaled mode (%ix%i for a %ix%i panel), MetalFX outputs %ix%i",
                     name, backing_width, backing_height, panel_width, panel_height, width, height);
        else
            snprintf(scaled, sizeof(scaled), "present: %s is in a scaled mode (%ix%i for a %ix%i panel), so MetalFX makes "
                     "more pixels than it shows. MetalFXUpscale = 2 upscales to the panel's resolution instead, or a mode "
                     "that looks like %ix%i fits the panel exactly", name, backing_width, backing_height, panel_width,
                     panel_height, panel_width / 2, panel_height / 2);
    }
    if (scaled[0] != 0 && strcmp(scaled, last_scaled) != 0)
        printf("%s\n", scaled);
    strcpy(last_scaled, scaled);
}

// the view's size in pixels. Aspyr's _viewSize and contentScaleFactor are in points (1728x1117 with a scale of 1 on a
// 3456x2234 MacBook Pro), so go by its window's backing scale. AppKit is asked on the main thread; Aspyr's render thread
// also sizes the drawable (when the game changes resolution) and goes by the scale the main thread saw last.
static void view_pixels(NSView *view, CGSize view_size, int *width, int *height) {
    double scale = 1; // not in a window yet
    if (view.window != nil) {
        scale = pthread_main_np() ? view.window.backingScaleFactor : atomic_load(&screen_scale);
        if (scale <= 0) // the main thread hasn't seen the window on a screen yet
            scale = view.window.backingScaleFactor;
    }
    *width = (int)lround(view_size.width * scale);
    *height = (int)lround(view_size.height * scale);
}

// Aspyr's updateDrawableSize sets the drawable to the forced size (the game's resolution) when there is one, and to
// the view's size in points otherwise. Keep the forced size for everything else that reads it, but give the layer the
// view's native pixels (or the panel's, output_size) when they're bigger, so MetalFX can upscale into them. Unforced
// drawables are left alone here: Aspyr unforces, reads the drawable back and forces it again unless it's the game's size
// (enlarge_unforced_drawable).
static void updateDrawableSize_hook(id self, SEL sel) {
    ((void (*)(id, SEL))orig_updateDrawableSize)(self, sel);
    objc_storeWeak(&game_view, self);
    if (pthread_main_np() && ((NSView *)self).window != nil)
        remember_screen(self);
    CGSize view_size = *(CGSize *)((char *)self + view_size_offset);
    atomic_store(&view_points, (unsigned int)lround(view_size.width) << 16 | (unsigned int)lround(view_size.height));
    int forced_width = *(int *)((char *)self + forced_width_offset);
    int forced_height = *(int *)((char *)self + forced_height_offset);
    if (forced_width <= 0 || forced_height <= 0)
        return;
    int backing_width, backing_height, native_width, native_height;
    view_pixels(self, view_size, &backing_width, &backing_height);
    output_size(backing_width, backing_height, &native_width, &native_height);

    static int last_forced_width = -1, last_forced_height = -1, last_native_width = -1, last_native_height = -1;
    int changed = forced_width != last_forced_width || forced_height != last_forced_height ||
                  native_width != last_native_width || native_height != last_native_height;
    last_forced_width = forced_width; last_forced_height = forced_height;
    last_native_width = native_width; last_native_height = native_height;

    if (native_width < forced_width || native_height < forced_height ||
        (native_width == forced_width && native_height == forced_height)) {
        if (changed)
            printf("MetalFX: the game's resolution %ix%i already fills the %ix%i screen, nothing to upscale\n",
                   forced_width, forced_height, native_width, native_height);
        return;
    }
    uint64_t sizes = pack_sizes(forced_width, forced_height, native_width, native_height);
    if (sizes == atomic_load(&failed_sizes))
        return;
    atomic_store(&resized_sizes, sizes);
    CAMetalLayer *layer = ((id (*)(id, SEL))objc_msgSend)(self, sel_metalLayer);
    layer.drawableSize = CGSizeMake(native_width, native_height);
}

// AppKit calls this on the main thread when the view's window gets another backing scale. Its size in points can stay
// the same, and then Aspyr's view wouldn't resize the drawable.
static void viewDidChangeBackingProperties_new(id self, SEL sel) {
    ((void (*)(id, SEL))nsview_viewDidChangeBackingProperties)(self, sel);
    ((void (*)(id, SEL))objc_msgSend)(self, sel_updateDrawableSize);
}

// resizes the drawable when the game window moves to another screen, or a screen changes (resolution, scale or refresh)
static void watch_screens(void) {
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserverForName:NSWindowDidChangeScreenNotification object:nil queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *notification) {
        NSView *view = objc_loadWeak(&game_view);
        if (view != nil && notification.object == view.window)
            ((void (*)(id, SEL))objc_msgSend)(view, sel_updateDrawableSize);
    }];
    [center addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil
                         queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *notification) {
        NSView *view = objc_loadWeak(&game_view);
        if (view != nil)
            ((void (*)(id, SEL))objc_msgSend)(view, sel_updateDrawableSize);
    }];
}

// (re)makes the scaler and its textures for these sizes and formats, returns 0 if MetalFX can't do it
static int ensure_resources(id<MTLDevice> device, NSUInteger in_width, NSUInteger in_height, MTLPixelFormat in_format,
                            NSUInteger out_width, NSUInteger out_height, MTLPixelFormat out_format) {
    if (scaler != nil && in_width == input_width && in_height == input_height && in_format == input_format &&
        out_width == output_width && out_height == output_height && out_format == output_format)
        return 1;
    // frames still on the GPU can be using these, so they go once a later frame is done (metalfx_before_present)
    if (retired == nil)
        retired = [[NSMutableArray alloc] init];
    if (scaler != nil)
        [retired addObject:scaler];
    if (input_texture != nil)
        [retired addObject:input_texture];
    if (output_texture != nil)
        [retired addObject:output_texture];
    [scaler release];
    [input_texture release];
    [output_texture release];
    scaler = nil;
    input_texture = output_texture = nil;

    MTLFXSpatialScalerDescriptor *desc = [[NSClassFromString(@"MTLFXSpatialScalerDescriptor") alloc] init];
    desc.inputWidth = in_width;
    desc.inputHeight = in_height;
    desc.outputWidth = out_width;
    desc.outputHeight = out_height;
    desc.colorTextureFormat = in_format;
    desc.outputTextureFormat = out_format;
    // the game's BGRA8 backbuffer holds gamma-encoded colour, an _sRGB format would hand MetalFX linear values
    BOOL srgb = in_format == MTLPixelFormatBGRA8Unorm_sRGB || in_format == MTLPixelFormatRGBA8Unorm_sRGB;
    desc.colorProcessingMode = srgb ? MTLFXSpatialScalerColorProcessingModeLinear : MTLFXSpatialScalerColorProcessingModePerceptual;
    scaler = [desc newSpatialScalerWithDevice:device];
    [desc release];
    if (scaler == nil)
        return 0;

    MTLTextureDescriptor *texture_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:in_format
                                                                                            width:in_width
                                                                                           height:in_height
                                                                                        mipmapped:NO];
    texture_desc.storageMode = MTLStorageModePrivate;
    texture_desc.usage = scaler.colorTextureUsage;
    input_texture = [device newTextureWithDescriptor:texture_desc];
    texture_desc.pixelFormat = out_format;
    texture_desc.width = out_width;
    texture_desc.height = out_height;
    texture_desc.usage = scaler.outputTextureUsage;
    output_texture = [device newTextureWithDescriptor:texture_desc];
    if (input_texture == nil || output_texture == nil) {
        [scaler release];
        scaler = nil;
        return 0;
    }
    input_width = in_width;
    input_height = in_height;
    input_format = in_format;
    output_width = out_width;
    output_height = out_height;
    output_format = out_format;
    printf("MetalFX upscaling %lux%lu to %lux%lu\n", (unsigned long)in_width, (unsigned long)in_height,
           (unsigned long)out_width, (unsigned long)out_height);
    return 1;
}

// Aspyr leaves the drawable unforced, at the view's size in points, when that's the game's resolution (1920x1080 on a 4K
// screen that looks like 1080p). Once a frame of that size has gone into it, give it the view's native pixels for the
// next frames, after Aspyr has decided not to force it.
static void enlarge_unforced_drawable(NSUInteger width, NSUInteger height) {
    double scale = atomic_load(&screen_scale);
    unsigned int points = atomic_load(&view_points);
    if (scale <= 1 || points != ((unsigned int)(width & 0xFFFF) << 16 | (unsigned int)(height & 0xFFFF)))
        return;
    id view = objc_loadWeak(&game_view);
    if (view == nil || *(int *)((char *)view + forced_width_offset) > 0 || *(int *)((char *)view + forced_height_offset) > 0)
        return;
    int native_width, native_height;
    output_size((int)lround(width * scale), (int)lround(height * scale), &native_width, &native_height);
    if (native_width <= (int)width || native_height <= (int)height)
        return;
    uint64_t sizes = pack_sizes(width, height, (NSUInteger)native_width, (NSUInteger)native_height);
    if (sizes == atomic_load(&failed_sizes))
        return;
    atomic_store(&resized_sizes, sizes);
    CAMetalLayer *layer = ((id (*)(id, SEL))objc_msgSend)(view, sel_metalLayer);
    layer.drawableSize = CGSizeMake(native_width, native_height);
}

// called instead of objc_msgSend for Aspyr's [blitEncoder copyFromTexture:backbuffer ... toTexture:drawable.texture ...]
static void copy_to_drawable(id<MTLBlitCommandEncoder> encoder, SEL sel, id<MTLTexture> source, NSUInteger source_slice,
                             NSUInteger source_level, MTLOrigin source_origin, MTLSize size, id<MTLTexture> drawable_texture,
                             NSUInteger destination_slice, NSUInteger destination_level, MTLOrigin destination_origin) {
    pending_input = pending_drawable_texture = nil;
    NSUInteger drawable_width = drawable_texture.width, drawable_height = drawable_texture.height;
    atomic_store(&frame_drawable_size, (unsigned int)(drawable_width & 0xFFFF) << 16 | (unsigned int)(drawable_height & 0xFFFF));
    // when Aspyr letterboxes, its drawable is the frame with a black bar on each side and the frame goes in the middle.
    // The drawable is bigger than that now, so the frame and the bars are scaled up with it.
    NSUInteger out_width = drawable_width, out_height = drawable_height;
    MTLOrigin origin = MTLOriginMake(0, 0, 0);
    int forced_width, forced_height;
    NotchRect placed;
    if ((destination_origin.x != 0 || destination_origin.y != 0) && notch_forced_drawable(&forced_width, &forced_height) &&
        notch_scale_rect(forced_width, forced_height, (int)drawable_width, (int)drawable_height,
                         (NotchRect){ (int)destination_origin.x, (int)destination_origin.y, (int)size.width,
                                      (int)size.height }, &placed)) {
        // AvoidNotch puts the frame below the camera housing, not in the middle: scale by the drawable it forced
        out_width = (NSUInteger)placed.w;
        out_height = (NSUInteger)placed.h;
        origin = MTLOriginMake((NSUInteger)placed.x, (NSUInteger)placed.y, 0);
    } else if (destination_origin.x != 0 || destination_origin.y != 0) {
        double scale_x = (double)drawable_width / (size.width + 2 * destination_origin.x);
        double scale_y = (double)drawable_height / (size.height + 2 * destination_origin.y);
        out_width = (NSUInteger)lround(size.width * scale_x);
        out_height = (NSUInteger)lround(size.height * scale_y);
        origin = MTLOriginMake((NSUInteger)lround(destination_origin.x * scale_x),
                               (NSUInteger)lround(destination_origin.y * scale_y), 0);
    }
    int upscale = metalfx_ready && size.width <= out_width && size.height <= out_height &&
                  (size.width < out_width || size.height < out_height) && origin.x + out_width <= drawable_width &&
                  origin.y + out_height <= drawable_height && source_slice == 0 && source_level == 0 &&
                  source_origin.x == 0 && source_origin.y == 0;
    if (!upscale) {
        atomic_store(&frame_sizes, 0);
        ((copy_texture_fn)objc_msgSend)(encoder, sel, source, source_slice, source_level, source_origin, size, drawable_texture,
                                        destination_slice, destination_level, destination_origin);
        if (metalfx_ready && size.width == drawable_width && size.height == drawable_height)
            enlarge_unforced_drawable(drawable_width, drawable_height);
        return;
    }
    if (!ensure_resources(encoder.device, size.width, size.height, source.pixelFormat, out_width, out_height,
                          drawable_texture.pixelFormat)) {
        printf("MetalFX couldn't upscale %lux%lu to %lux%lu, not upscaling at these sizes\n", (unsigned long)size.width,
               (unsigned long)size.height, (unsigned long)out_width, (unsigned long)out_height);
        // this frame goes out un-scaled in the corner, put the drawable back to the game's size until the sizes change
        atomic_store(&frame_sizes, 0);
        atomic_store(&failed_sizes, atomic_load(&resized_sizes));
        id view = objc_loadWeak(&game_view);
        if (view != nil)
            ((void (*)(id, SEL))orig_updateDrawableSize)(view, sel_updateDrawableSize);
        ((copy_texture_fn)objc_msgSend)(encoder, sel, source, source_slice, source_level, source_origin, size, drawable_texture,
                                        destination_slice, destination_level, destination_origin);
        return;
    }
    // MetalFX can read the backbuffer itself if it was made readable by shaders and is exactly the frame's size
    MTLTextureUsage needed = scaler.colorTextureUsage;
    if ((source.usage & needed) == needed && source.width == size.width && source.height == size.height &&
        source.textureType == MTLTextureType2D) {
        pending_input = source;
    } else {
        [encoder copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:source_origin sourceSize:size
                       toTexture:input_texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        pending_input = input_texture;
    }
    pending_drawable_texture = drawable_texture;
    pending_origin = origin;
    atomic_store(&frame_sizes, pack_sizes(size.width, size.height, out_width, out_height));
}

void *metalfx_copy_function(void) {
    return (void *)copy_to_drawable;
}

// Aspyr has ended its blit encoder by now, so MetalFX and our copy into the drawable can go in the command buffer
void metalfx_before_present(void *command_buffer) {
    id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)command_buffer;
    // the scaler and textures from before a resize go when this frame is done, and the frames before it are done by then
    if (retired.count > 0) {
        NSArray *objects = [retired copy];
        [retired removeAllObjects];
        [cb addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
            [objects release];
        }];
    }
    id<MTLTexture> input = pending_input, drawable_texture = pending_drawable_texture;
    pending_input = pending_drawable_texture = nil;
    if (input == nil || drawable_texture == nil || scaler == nil)
        return;
    scaler.colorTexture = input;
    scaler.outputTexture = output_texture;
    scaler.inputContentWidth = input_width;
    scaler.inputContentHeight = input_height;
    [scaler encodeToCommandBuffer:cb];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:output_texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(output_width, output_height, 1) toTexture:drawable_texture destinationSlice:0
         destinationLevel:0 destinationOrigin:pending_origin];
    [blit endEncoding];
}

int present_display_max_fps(void) {
    DisplayInfo display;
    if (display_follow_current(&display) == 1 && display.refresh > 0)
        return display.refresh;
    return atomic_load(&screen_max_fps);
}

int present_display_variable_refresh(void) {
    DisplayInfo display;
    if (display_follow_current(&display) == 1 && display.refresh > 0)
        return display.variable_refresh;
    return atomic_load(&screen_variable_refresh);
}

// the game's Metal layer on the main thread: the view we size, or Aspyr's view in one of the app's windows when MetalFX
// is off
static CAMetalLayer *game_layer(void) {
    NSView *view = objc_loadWeak(&game_view);
    Class view_class = objc_getClass("ASL_MetalView");
    for (NSWindow *window in [NSApp windows]) {
        for (NSView *subview in window.contentView.subviews) {
            if (view == nil && view_class != Nil && [subview isKindOfClass:view_class])
                view = subview;
        }
    }
    return view != nil ? ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("metalLayer")) : nil;
}

void present_describe(char *buf, size_t n) {
    unsigned int drawable = atomic_load(&frame_drawable_size);
    if (pthread_main_np()) {
        CAMetalLayer *layer = game_layer();
        if (layer != nil)
            drawable = (unsigned int)lround(layer.drawableSize.width) << 16 | (unsigned int)lround(layer.drawableSize.height);
    }
    char drawable_text[24] = "?";
    if (drawable != 0)
        snprintf(drawable_text, sizeof(drawable_text), "%ux%u", drawable >> 16, drawable & 0xFFFF);
    uint64_t sizes = atomic_load(&frame_sizes);
    if (sizes != 0)
        snprintf(buf, n, "drawable=%s metalfx=%ux%u->%ux%u", drawable_text, (unsigned int)(sizes >> 48),
                 (unsigned int)(sizes >> 32) & 0xFFFF, (unsigned int)(sizes >> 16) & 0xFFFF, (unsigned int)sizes & 0xFFFF);
    else
        snprintf(buf, n, "drawable=%s metalfx=off", drawable_text);
}

static ptrdiff_t ivar_offset(Class cls, const char *name) {
    Ivar ivar = class_getInstanceVariable(cls, name);
    return ivar != NULL ? ivar_getOffset(ivar) : -1;
}

int metalfx_init(int mode) {
    @autoreleasepool {
        metalfx_mode = mode == 2 ? 2 : 1;
        if (NSClassFromString(@"MTLFXSpatialScalerDescriptor") == nil) {
            printf("MetalFX isn't available (it needs macOS 13 or later), not upscaling\n");
            return 0;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        BOOL supported = device != nil && [NSClassFromString(@"MTLFXSpatialScalerDescriptor") supportsDevice:device];
        [device release];
        if (!supported) {
            printf("MetalFX doesn't support this GPU, not upscaling\n");
            return 0;
        }

        Class view = objc_getClass("ASL_MetalView");
        sel_metalLayer = sel_registerName("metalLayer");
        sel_updateDrawableSize = sel_registerName("updateDrawableSize");
        Method update = view != Nil ? class_getInstanceMethod(view, sel_updateDrawableSize) : NULL;
        view_size_offset = view != Nil ? ivar_offset(view, "_viewSize") : -1;
        forced_width_offset = view != Nil ? ivar_offset(view, "_forcedDrawableSizeWidth") : -1;
        forced_height_offset = view != Nil ? ivar_offset(view, "_forcedDrawableSizeHeight") : -1;
        if (update == NULL || view_size_offset < 0 || forced_width_offset < 0 || forced_height_offset < 0 ||
            class_getInstanceMethod(view, sel_metalLayer) == NULL) {
            printf("MetalFX: couldn't find Aspyr's Metal view, not upscaling\n");
            return 0;
        }
        orig_updateDrawableSize = method_setImplementation(update, (IMP)updateDrawableSize_hook);
        // Aspyr's view doesn't have this, and NSView's does nothing
        SEL backing_changed = sel_registerName("viewDidChangeBackingProperties");
        nsview_viewDidChangeBackingProperties = class_getMethodImplementation([NSView class], backing_changed);
        class_addMethod(view, backing_changed, (IMP)viewDidChangeBackingProperties_new, "v@:");
        watch_screens();
        metalfx_ready = 1;
        printf("MetalFX upscaling is on, it upscales when the game's resolution is lower than the screen's%s\n",
               metalfx_mode == 2 ? " (or in a scaled mode, the panel's)" : "");
        return 1;
    }
}
