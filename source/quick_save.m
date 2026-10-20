// Quick save (QuickSave in settings.ini): a key, or L3 + R3 held for a second in the pause menu, saves a solo zombies
// game exactly as the settings page's Save game row does: checkpoint.lua's save_game, with its checks (a solo zombies
// match; not a resumed game that's still short of its saved round) and checkpoint.ini's lock. This side only notices
// the key and the buttons and keeps the request (quick_save_rules.c); checkpoint.lua's timer takes it with
// CheckpointTakeQuickSave, counts the controller's only while the pause menu is open, saves and says how it went in a
// notice.
// - The key: an AppKit local event monitor on the main thread, which sees what SDL reads from the event queue (and the
//   test hooks' made-up keys). The game gets the key as well, so QuickSave offers keys it doesn't use: F5 to F9 (F10
//   opens the settings page from the start menu, F12 is Steam's screenshot key). With Cmd, Ctrl, Option or Shift held
//   it's left alone.
// - The buttons: gc_bridge.m's XInputGetState hook passes each controller's buttons on, so only with ControllerBridge.
#import <AppKit/AppKit.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include "quick_save.h"
#include "quick_save_rules.h"

#define Players 4 // XUSER_MAX_COUNT

static atomic_int quick_key; // the macOS virtual key code, 0 for off
static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
static quick_save_slot_t slot;              // under lock
static quick_save_combo_t combos[Players];  // under lock
static id key_monitor;                      // main thread

static double now(void) {
    return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}

static const char *key_name(int key) {
    switch (key) {
    case 96: return "F5";
    case 97: return "F6";
    case 98: return "F7";
    case 100: return "F8";
    case 101: return "F9";
    default: return "the quick save key";
    }
}

void quick_save_set_key(int key) {
    atomic_store(&quick_key, key > 0 && key < 128 ? key : 0);
}

void quick_save_init(int key) {
    quick_save_set_key(key);
    dispatch_async(dispatch_get_main_queue(), ^{
        const NSEventModifierFlags modifiers = NSEventModifierFlagCommand | NSEventModifierFlagControl |
                                               NSEventModifierFlagOption | NSEventModifierFlagShift;
        key_monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *event) {
            int wanted = atomic_load(&quick_key);
            if (wanted != 0 && event.keyCode == wanted && !event.isARepeat && (event.modifierFlags & modifiers) == 0) {
                os_unfair_lock_lock(&lock);
                quick_save_slot_put(&slot, Quick_Save_Key, now());
                os_unfair_lock_unlock(&lock);
                printf("BO3MacFix: quick save: %s pressed\n", key_name(wanted));
            }
            return event;
        }] retain];
    });
    int set = atomic_load(&quick_key);
    if (set != 0)
        printf("BO3MacFix: quick save on %s, or L3 + R3 held for a second in the pause menu (macOS controller "
               "support)\n", key_name(set));
    else
        printf("BO3MacFix: quick save off\n");
}

void quick_save_pad(uint32_t user, uint16_t buttons) {
    if (user >= Players || atomic_load(&quick_key) == 0)
        return;
    double time = now();
    os_unfair_lock_lock(&lock);
    bool fire = quick_save_combo(&combos[user], buttons, time);
    if (fire)
        quick_save_slot_put(&slot, Quick_Save_Pad, time);
    os_unfair_lock_unlock(&lock);
    if (fire)
        printf("BO3MacFix: quick save: L3 + R3 held on player %u's controller\n", user + 1);
}

int quick_save_lua_take(lua_State *s) {
    os_unfair_lock_lock(&lock);
    quick_save_source_t source = quick_save_slot_take(&slot, now());
    os_unfair_lock_unlock(&lock);
    if (source == Quick_Save_Key) {
        hks_push_string(s, "key");
        hks_push_string(s, key_name(atomic_load(&quick_key)));
        return 2;
    }
    if (source == Quick_Save_Pad)
        hks_push_string(s, "pad");
    else
        hks_push_nil(s);
    return 1;
}
