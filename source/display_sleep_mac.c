// IdleDisplaySleep's glue (the decisions are display_sleep.c, see display_sleep.h): finds the game's display sleep
// assertion, lets it go and takes it again on the main thread, and declares a controller's input as user activity.
//
// The assertion: SDL's Cocoa_SuspendScreenSaver makes it with IOPMAssertionCreateWithDescription at the game's video
// start, named "<app> using SDL_DisableScreenSaver", and keeps its ID in its own video data. The interposer below notes
// that ID and from then on this file manages the assertion: it releases the ID while the player is away from play and
// makes a new assertion of the same type and name when play goes on. SDL's memory is never written: its field keeps
// the first ID, which SDL itself only uses again if the game calls SDL_EnableScreenSaver or SDL_DisableScreenSaver
// (it doesn't; if it did, the interposers below see it and follow SDL: a release of the ID this file holds ends its
// management, a new assertion of SDL's becomes the one it holds).
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "display_sleep.h"
#include "display_sleep_mac.h"
#include "dyld-interposing.h"

static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
// under lock:
static display_sleep_t state;
static bool known_away = false, known_battery = false; // the last situation display_sleep_update was told
static IOPMAssertionID held = kIOPMNullAssertionID;   // the game's assertion as it is now, none while let go
static CFStringRef held_name = NULL;                  // its name, for making it again
static bool managed = false;                          // SDL made it and hasn't released it itself
static bool applied_released = false;                 // what was last done to it: let go (true) or held
static atomic_int mode = Display_Sleep_Off;
static atomic_bool released_now = false; // a copy of state.released for the controller's quick look
static bool debug = false;

static uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// on the main thread (where SDL makes the assertion too, so only the main thread changes `held`): brings the assertion
// to what the last decision says. The calls to macOS go out without the lock (each is a message to powerd).
static void apply(void *unused) {
    (void)unused;
    os_unfair_lock_lock(&lock);
    bool want_released = state.released, change = managed && want_released != applied_released;
    IOPMAssertionID id = held;
    CFStringRef name = held_name != NULL ? CFRetain(held_name) : NULL;
    os_unfair_lock_unlock(&lock);
    if (!change) {
        if (name != NULL)
            CFRelease(name);
        return;
    }
    IOReturn result = kIOReturnSuccess;
    if (want_released && id != kIOPMNullAssertionID) {
        result = IOPMAssertionRelease(id);
        id = kIOPMNullAssertionID;
    } else if (!want_released && id == kIOPMNullAssertionID && name != NULL) {
        result = IOPMAssertionCreateWithDescription(kIOPMAssertPreventUserIdleDisplaySleep, name, NULL, NULL, NULL, 0,
                                                    NULL, &id);
        if (result != kIOReturnSuccess)
            id = kIOPMNullAssertionID;
    }
    if (name != NULL)
        CFRelease(name);
    os_unfair_lock_lock(&lock);
    held = id;
    applied_released = want_released;
    os_unfair_lock_unlock(&lock);
    printf(want_released ? "display sleep: away from play, the display may turn off after the Mac's own idle time%s\n"
                         : "display sleep: back in play, the game keeps the display on%s\n",
           result == kIOReturnSuccess ? "" : " (macOS refused the change)");
}

// under lock: decides again and has the main thread apply a change
static void decide_locked(uint64_t now) {
    bool before = state.released;
    bool released = display_sleep_decide(&state, atomic_load(&mode), known_battery, known_away, now);
    atomic_store(&released_now, released);
    if (released != before && managed)
        dispatch_async_f(dispatch_get_main_queue(), NULL, apply);
}

void display_sleep_update(bool away, bool on_battery) {
    // called up to eight times a second (notices.lua's polls): nothing to do with the setting off
    if (atomic_load(&mode) == Display_Sleep_Off && !atomic_load(&released_now))
        return;
    os_unfair_lock_lock(&lock);
    known_away = away;
    known_battery = on_battery;
    decide_locked(now_ns());
    os_unfair_lock_unlock(&lock);
}

void display_sleep_set_mode(int value) {
    atomic_store(&mode, value);
    os_unfair_lock_lock(&lock);
    decide_locked(now_ns());
    os_unfair_lock_unlock(&lock);
}

void display_sleep_init(int value) {
    const char *env = getenv("BO3MACFIX_IDLEDISPLAYSLEEPDEBUG");
    debug = env != NULL && atoi(env) != 0;
    atomic_store(&mode, value);
    if (value != Display_Sleep_Off)
        printf("display sleep: the display may turn off while the game is paused, in the menus or in the background%s\n",
               value == Display_Sleep_On_Battery ? ", on battery" : "");
}

void display_sleep_controller(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, int16_t left_x,
                              int16_t left_y, int16_t right_x, int16_t right_y) {
    // the game asks for each controller's state every frame: nothing more than this look while the display is held
    if (!atomic_load_explicit(&released_now, memory_order_relaxed) ||
        !display_sleep_is_input(buttons, left_trigger, right_trigger, left_x, left_y, right_x, right_y))
        return;
    os_unfair_lock_lock(&lock);
    bool due = display_sleep_activity_due(&state, now_ns());
    os_unfair_lock_unlock(&lock);
    if (!due)
        return;
    static IOPMAssertionID activity = kIOPMNullAssertionID;
    IOReturn result = IOPMAssertionDeclareUserActivity(CFSTR("BO3MacFix: controller input in the game"),
                                                       kIOPMUserActiveLocal, &activity);
    if (debug)
        printf("display sleep: a controller's input declared as user activity (%s)\n",
               result == kIOReturnSuccess ? "done" : "refused");
}

// SDL's assertion is made here (and any other code's, which passes through untouched)
static IOReturn assertion_create(CFStringRef type, CFStringRef name, CFStringRef details, CFStringRef reason,
                                 CFStringRef bundle, CFTimeInterval timeout, CFStringRef timeout_action,
                                 IOPMAssertionID *id) {
    IOReturn result = IOPMAssertionCreateWithDescription(type, name, details, reason, bundle, timeout, timeout_action, id);
    if (result != kIOReturnSuccess || id == NULL || type == NULL || name == NULL ||
        CFStringCompare(type, kIOPMAssertPreventUserIdleDisplaySleep, 0) != kCFCompareEqualTo ||
        !CFStringHasSuffix(name, CFSTR("using SDL_DisableScreenSaver")))
        return result;
    os_unfair_lock_lock(&lock);
    IOPMAssertionID previous = held;
    held = *id;
    if (held_name != NULL)
        CFRelease(held_name);
    held_name = CFStringCreateCopy(kCFAllocatorDefault, name);
    managed = true;
    // SDL made it again while it was let go: it goes too (on this thread, SDL's, the main thread)
    bool release_now = applied_released;
    if (release_now) {
        IOPMAssertionRelease(held);
        held = kIOPMNullAssertionID;
    }
    os_unfair_lock_unlock(&lock);
    if (previous != kIOPMNullAssertionID && previous != *id)
        IOPMAssertionRelease(previous); // the copy this file had made, SDL has its own now
    if (debug || atomic_load(&mode) != Display_Sleep_Off)
        printf("display sleep: the game holds the display on (SDL's assertion %u)%s\n", (unsigned)*id,
               release_now ? ", let go at once: away from play" : "");
    return result;
}
DYLD_INTERPOSE(assertion_create, IOPMAssertionCreateWithDescription);

// SDL letting go of its assertion itself (SDL_EnableScreenSaver): then it isn't this file's to make again
static IOReturn assertion_release(IOPMAssertionID id) {
    os_unfair_lock_lock(&lock);
    if (managed && id == held && id != kIOPMNullAssertionID) {
        managed = false;
        held = kIOPMNullAssertionID;
    }
    os_unfair_lock_unlock(&lock);
    return IOPMAssertionRelease(id);
}
DYLD_INTERPOSE(assertion_release, IOPMAssertionRelease);
