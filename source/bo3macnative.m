#include <Foundation/Foundation.h>
#include <AppKit/AppKit.h>
#include <ImageIO/ImageIO.h>
#include <dispatch/dispatch.h>
#include <string.h>
#include <unistd.h>
#include "versioninfo.h"

void display_nsalert(const char *title, const char *message); // fwd declare
void set_player_name(const char *name); // declared in bo3macfix.c
const char *get_player_name(); // declared in bo3macfix.c

@interface MacFixNativeDelegate : NSObject
@end
@implementation MacFixNativeDelegate
+(void)setUpMenuBar {
    NSMenuItem *macfixMenuItem = [NSMenuItem new];
    NSMenu *macfixMenu = [[NSMenu alloc] initWithTitle:@"BO3MacFix"];
    // the two dialogs. TODO(Emma): implement network password
    [[macfixMenu addItemWithTitle:@"Change Player Name" action:@selector(doPlayerNameChange) keyEquivalent:@"n"] setTarget:self];
    [[macfixMenu addItemWithTitle:@"Set Network Password" action:@selector(doPlayerNameChange) keyEquivalent:@"p"] setTarget:self];
    // friends only. TODO(Emma): implement this properly
    NSMenuItem *fo = [macfixMenu addItemWithTitle:@"Enable Friends Only" action:@selector(doPlayerNameChange) keyEquivalent:@"f"];
    [fo setState:NSControlStateValueOn];
    [fo setTarget:self];
    // spacer then version info
    [macfixMenu addItem:[NSMenuItem separatorItem]];
    [macfixMenu addItemWithTitle:@"BO3MacFix " @MACFIX_VERSION action:NULL keyEquivalent:@""];
    // add the menu to the main application's menu
    [macfixMenuItem setSubmenu:macfixMenu];
    [[[NSApplication sharedApplication] mainMenu] addItem:macfixMenuItem];
}
+(void)doPlayerNameChange {
    // create an NSAlert
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:[NSString stringWithUTF8String:"Change Player Name"]];
    [alert setInformativeText:[NSString stringWithUTF8String:"(leave blank to use your Steam name)"]];
    // make a text box with our existing custom player name as its contents
    NSTextField *txt = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)];
    [txt setStringValue:[NSString stringWithUTF8String:get_player_name()]];
    [alert setAccessoryView:txt];
    // launch it as a sheet modal so it doesn't freeze the whole game
    [alert beginSheetModalForWindow:[[NSApplication sharedApplication] mainWindow] completionHandler:^(NSModalResponse returnCode) {
        if (returnCode == NSModalResponseCancel || returnCode == NSModalResponseOK) { // pressing Enter can mean Cancel
            set_player_name([[txt stringValue] UTF8String]);
        }
    }];
}
@end

static char new_title_buf[0x40];
void set_window_title(const char *new_title) {
    strncpy(new_title_buf, new_title, sizeof(new_title_buf));
    dispatch_async(dispatch_get_main_queue(), ^(void) {
        [[[NSApplication sharedApplication] mainWindow] setTitle:[NSString stringWithUTF8String:new_title_buf]];
        if (@available(macOS 10.14, *)) {
            [NSApplication sharedApplication].appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        }
    });
}

// decodes an image file into 8-bit RGBA pixels (straight alpha), which the caller frees, or returns NULL
uint8_t *decode_image_rgba(const char *path, uint32_t *width, uint32_t *height) {
    @autoreleasepool {
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
        if (url == NULL)
            return NULL;
        CGImageSourceRef source = CGImageSourceCreateWithURL(url, NULL);
        CFRelease(url);
        if (source == NULL)
            return NULL;
        CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
        CFRelease(source);
        if (image == NULL)
            return NULL;
        size_t w = CGImageGetWidth(image);
        size_t h = CGImageGetHeight(image);
        uint8_t *pixels = (w > 0 && h > 0 && w <= 16384 && h <= 16384) ? calloc(w * h, 4) : NULL;
        CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = pixels != NULL ? CGBitmapContextCreate(pixels, w, h, 8, w * 4, colorSpace, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : NULL;
        CGColorSpaceRelease(colorSpace);
        if (context == NULL) {
            free(pixels);
            CGImageRelease(image);
            return NULL;
        }
        CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
        CGContextRelease(context);
        CGImageRelease(image);
        // CoreGraphics only draws premultiplied alpha, undo it for any translucent pixels
        for (size_t i = 0; i < w * h * 4; i += 4) {
            uint8_t a = pixels[i + 3];
            if (a != 0 && a != 255) {
                pixels[i] = (uint8_t)((pixels[i] * 255 + a / 2) / a);
                pixels[i + 1] = (uint8_t)((pixels[i + 1] * 255 + a / 2) / a);
                pixels[i + 2] = (uint8_t)((pixels[i + 2] * 255 + a / 2) / a);
            }
        }
        *width = (uint32_t)w;
        *height = (uint32_t)h;
        return pixels;
    }
}

// returns a copy of the clipboard's text, which the caller frees, or NULL if there isn't any
char *copy_clipboard_text() {
    @autoreleasepool {
        NSString *text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        return (text != nil && [text UTF8String] != NULL) ? strdup([text UTF8String]) : NULL;
    }
}

void set_clipboard_text(const char *text) {
    @autoreleasepool {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        [pasteboard setString:[NSString stringWithUTF8String:text] forType:NSPasteboardTypeString];
    }
}

// AAE's online requests (T7Overcharged's Axios): plain HTTPS GETs to a few public hosts, results kept by key
static NSMutableDictionary<NSString *, NSString *> *http_results = nil;

static bool http_host_allowed(NSURL *url) {
    NSArray *hosts = @[ @"raw.githubusercontent.com", @"wttr.in" ];
    return url != nil && [url.scheme isEqualToString:@"https"] && [hosts containsObject:url.host.lowercaseString];
}

// starts fetching url, keeping the response text under key. Returns false for anything but an allowed host.
bool http_get_async(const char *url_string, const char *key_string) {
    @autoreleasepool {
        NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:url_string]];
        NSString *key = [NSString stringWithUTF8String:key_string];
        if (!http_host_allowed(url) || key == nil) {
            printf("BO3MacFix: AAE online: not fetching %s (only HTTPS to a few hosts is allowed)\n", url_string);
            return false;
        }
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            http_results = [[NSMutableDictionary alloc] init];
        });
        NSURLRequest *request = [NSURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                             timeoutInterval:10];
        [[[NSURLSession sharedSession] dataTaskWithRequest:request
                                         completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            NSInteger code = [response isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse *)response).statusCode : 0;
            printf("BO3MacFix: AAE online: %s from %s (%lu bytes)\n", error != nil ? "failed" : code == 200 ? "got" : "HTTP error",
                   url.host.UTF8String ?: "?", (unsigned long)data.length);
            if (error != nil || code != 200 || data.length == 0 || data.length > 512 * 1024)
                return;
            NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            if (text == nil)
                return;
            @synchronized (http_results) {
                http_results[key] = text;
            }
            [text release];
        }] resume];
        return true;
    }
}

// the text fetched for key, which the caller frees, or NULL while there's none yet
char *http_copy_result(const char *key_string) {
    @autoreleasepool {
        if (http_results == nil)
            return NULL;
        NSString *key = [NSString stringWithUTF8String:key_string];
        @synchronized (http_results) {
            NSString *text = key != nil ? http_results[key] : nil;
            return text != nil ? strdup(text.UTF8String) : NULL;
        }
    }
}

void add_menu_bar() {
    dispatch_async(dispatch_get_main_queue(), ^(void) {
        [MacFixNativeDelegate setUpMenuBar];
    });
}

void bounce_dock_icon() {
    dispatch_async(dispatch_get_main_queue(), ^(void) {
        [[NSApplication sharedApplication] requestUserAttention:NSCriticalRequest];
    });
}

bool alert_displaying = false;

void display_nsalert(const char *title, const char *message) {
    // don't ping another message if an alert is displaying
    while (alert_displaying) {
        usleep(1000);
    }
    // really shit way of halting the thread until the alert returns
    alert_displaying = true;
    dispatch_async(dispatch_get_main_queue(), ^(void) {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:[NSString stringWithUTF8String:title]];
        [alert setInformativeText:[NSString stringWithUTF8String:message]];
        [alert runModal];
        alert_displaying = false;
    });
    // halt our calling thread while the alert hasn't returned
    while (alert_displaying) {
        usleep(1000);
    }
}
