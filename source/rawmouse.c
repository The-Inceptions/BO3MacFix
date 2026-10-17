// RawMouse = 1: aiming (SDL's relative mouse mode) uses the mouse's raw movement instead of macOS's accelerated
// deltas. SDL 2.0.7's Cocoa_HandleMouseEvent passes [NSEvent deltaX/deltaY] on to SDL_SendMouseMotion, and those
// already have macOS's pointer acceleration applied, which the game's own MouseAcceleration setting can't undo.
// Here the raw counts come from IOHIDManager, and the hooked handler sends those instead. Menus (absolute cursor)
// aren't affected.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <objc/message.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include "dobby.h"
#include "offsets.h"
#include "rawmouse.h"

static uint64_t base_address = 0;
static _Atomic int raw_x = 0, raw_y = 0;
static double scale = 1.0, rest_x = 0, rest_y = 0;

static void raw_input(void *context, IOReturn result, void *sender, IOHIDValueRef value) {
    IOHIDElementRef element = IOHIDValueGetElement(value);
    if (IOHIDElementGetUsagePage(element) != kHIDPage_GenericDesktop)
        return;
    int amount = (int)IOHIDValueGetIntegerValue(value);
    if (IOHIDElementGetUsage(element) == kHIDUsage_GD_X)
        atomic_fetch_add(&raw_x, amount);
    else if (IOHIDElementGetUsage(element) == kHIDUsage_GD_Y)
        atomic_fetch_add(&raw_y, amount);
}

// void Cocoa_HandleMouseEvent(SDL_VideoDevice *_this, NSEvent *event), only called in relative mode
install_hook_name(Cocoa_HandleMouseEvent, void, void *video, void *event) {
    // NSEventTypeMouseMoved, Left/Right/OtherMouseDragged, like the original
    unsigned long type = ((unsigned long (*)(void *, SEL))objc_msgSend)(event, sel_registerName("type"));
    if (type != 5 && type != 6 && type != 7 && type != 27)
        return;
    uint8_t *mouse = ((uint8_t *(*)(void))(base_address + ADDR_SDL_GetMouse))();
    double x = atomic_exchange(&raw_x, 0) * scale + rest_x, y = atomic_exchange(&raw_y, 0) * scale + rest_y;
    int send_x = (int)trunc(x), send_y = (int)trunc(y);
    rest_x = x - send_x;
    rest_y = y - send_y;
    if (mouse != NULL && (send_x != 0 || send_y != 0))
        ((int (*)(void *, uint32_t, int, int, int))(base_address + ADDR_SDL_SendMouseMotion))(
            *(void **)(mouse + 0x58), *(uint32_t *)(mouse + 0x50), 1, send_x, send_y);
}

void rawmouse_init(uint64_t base, int enabled, int scale_percent) {
    base_address = base;
    const char *env = getenv("BO3MACFIX_RAWMOUSE");
    if (env != NULL)
        enabled = atoi(env);
    if (!enabled)
        return;
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int page = kHIDPage_GenericDesktop, usage = kHIDUsage_GD_Mouse;
    CFNumberRef page_number = CFNumberCreate(NULL, kCFNumberIntType, &page);
    CFNumberRef usage_number = CFNumberCreate(NULL, kCFNumberIntType, &usage);
    const void *keys[] = { CFSTR(kIOHIDDeviceUsagePageKey), CFSTR(kIOHIDDeviceUsageKey) };
    const void *values[] = { page_number, usage_number };
    CFDictionaryRef match = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(manager, match);
    CFRelease(match);
    CFRelease(page_number);
    CFRelease(usage_number);
    IOHIDManagerRegisterInputValueCallback(manager, raw_input, NULL);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOReturn opened = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (opened != kIOReturnSuccess) {
        printf("BO3MacFix: raw mouse input isn't available (IOHIDManagerOpen 0x%x), using macOS's\n", opened);
        return;
    }
    scale = (scale_percent > 0 ? scale_percent : 100) / 100.0;
    install_hook_Cocoa_HandleMouseEvent((void *)(base + ADDR_Cocoa_HandleMouseEvent));
    printf("BO3MacFix: aiming uses raw mouse input (RawMouse), scale %.2f\n", scale);
}
