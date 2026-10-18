// PlayStation controllers through macOS's GameController framework, fed into Aspyr's XInput emulation.
//
// The game reads controllers with a built-in SDL 2.0.7 that only knows IOKit HID: it can't read a DualSense over
// Bluetooth, and its rumble goes through ForceFeedback, which has no driver for Sony controllers. With
// ControllerBridge = 1, controllers.c hides Sony controllers from that SDL, and this file answers Aspyr's
// XInputGetState/XInputSetState for them instead, over USB or Bluetooth, with rumble through CoreHaptics and the
// lightbar colour from settings.ini. The game's own code only ever calls those two XInput functions.
//
// Aspyr keeps an SDL controller per XInput user index (player) and gives a new one the first free slot. A bridged
// controller takes the first index SDL isn't using, and moves if SDL later puts a controller of its own there.
// The game only looks for controllers at startup and when asked, so it's asked whenever one connects or moves later.
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <math.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dobby.h"
#include "offsets.h"
#include "controllers.h"
#include "gc_bridge.h"
#include "prepatch.h"

#define XUSER_MAX_COUNT 4
#define ERROR_SUCCESS 0
#define SONY_VENDOR_ID 0x054C

typedef struct {
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
} XINPUT_GAMEPAD;

typedef struct {
    uint32_t dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

typedef struct {
    uint16_t wLeftMotorSpeed;
    uint16_t wRightMotorSpeed;
} XINPUT_VIBRATION;

// one controller from the GameController framework. Objective-C objects are kept as id so this builds for
// macOS 10.13, only code behind @available(macOS 11.3) touches them.
typedef struct {
    // under pads_lock:
    id controller;              // GCController, retained, nil = free entry
    const char *kind;           // "DualSense" or "DualShock 4"
    int user_index;             // XInput user index the game reads it on, -1 = waiting for a free one
    bool found;                 // the game has asked for its state since it connected or moved
    XINPUT_STATE state;
    uint16_t wanted_motors[2];  // last speeds from XInputSetState
    int16_t gyro[2];            // right stick deflection GyroAim adds (x, y)
    // only on input_queue:
    id engines[2];              // CHHapticEngine for the left (large) and right (small) motor, or one for both
    id players[2];              // CHHapticPatternPlayer
    int engine_count;
    uint16_t motors[2];         // speeds the players are running at
    double haptics_retry_at;    // CFAbsoluteTime after which setting up rumble may be tried again
    float gyro_bias[3];         // the gyro's reading at rest, learned while the controller is still
    float gyro_smooth[2];       // smoothed yaw and pitch rate
    int gyro_samples;
} bridged_pad;

static uint64_t base_address;
static bool enabled;
static int lightbar_color = -1;
static int gyro_mode = 0;              // GyroAim: 0 = off, 1 = always, 2 = only while L2 is held
static float gyro_scale = 1.0f;        // GyroSensitivity / 100
static int gyro_invert = 0;            // GyroInvert: 1 = vertical, 2 = horizontal, 3 = both
static bool adaptive_triggers = false; // AdaptiveTriggers
static atomic_bool app_active = true;
static os_unfair_lock pads_lock = OS_UNFAIR_LOCK_INIT;
static bridged_pad pads[XUSER_MAX_COUNT];
static const void *sdl_seen[XUSER_MAX_COUNT]; // under pads_lock: the SDL controller last seen on each user index
static dispatch_queue_t input_queue;

// Aspyr's XInput slots, { SDL_GameController *; SDL_Haptic *; int effect; }[4]
static const uint8_t *sdl_slot(int user_index) {
    return *(const uint8_t *volatile *)(base_address + ADDR_ASL_XInputSlots + user_index * 24);
}

static bool sdl_slot_in_use(int user_index) {
    return sdl_slot(user_index) != NULL;
}

// these all need pads_lock held

// logs SDL taking or letting go of a user index. SDL_GameController.guid is at +0xC, its vendor and product at +4
// and +8 in that
static void note_sdl_slots(void) {
    for (int u = 0; u < XUSER_MAX_COUNT; u++) {
        const uint8_t *controller = sdl_slot(u);
        if (controller == sdl_seen[u])
            continue;
        sdl_seen[u] = controller;
        if (controller == NULL) {
            printf("GameController bridge: SDL let go of player %i\n", u + 1);
            continue;
        }
        uint16_t vendor, product;
        memcpy(&vendor, controller + 0x10, sizeof(vendor));
        memcpy(&product, controller + 0x14, sizeof(product));
        if (vendor == SONY_VENDOR_ID)
            printf("GameController bridge: SDL took a PlayStation controller (%04x:%04x) as player %i, it should have "
                   "been left to the bridge\n", vendor, product, u + 1);
        else
            printf("GameController bridge: SDL has a controller (%04x:%04x) as player %i\n", vendor, product, u + 1);
    }
}

static bool user_index_free(int user_index) {
    if (sdl_slot_in_use(user_index))
        return false;
    for (int i = 0; i < XUSER_MAX_COUNT; i++)
        if (pads[i].controller != nil && pads[i].user_index == user_index)
            return false;
    return true;
}

static int first_free_user_index(void) {
    for (int u = 0; u < XUSER_MAX_COUNT; u++)
        if (user_index_free(u))
            return u;
    return -1;
}

static bridged_pad *pad_for_user(int user_index) {
    if (sdl_slot_in_use(user_index))
        return NULL;
    for (int i = 0; i < XUSER_MAX_COUNT; i++)
        if (pads[i].controller != nil && pads[i].user_index == user_index)
            return &pads[i];
    return NULL;
}

static void player_changed(bridged_pad *pad, int old_index) {
    if (pad->user_index < 0)
        printf("GameController bridge: player %i was taken by another controller, %s waits for a free slot\n",
               old_index + 1, pad->kind);
    else if (old_index < 0)
        printf("GameController bridge: %s is now player %i\n", pad->kind, pad->user_index + 1);
    else
        printf("GameController bridge: player %i was taken by another controller, %s moved to player %i\n",
               old_index + 1, pad->kind, pad->user_index + 1);
    if (@available(macOS 11.3, *)) {
        GCController *controller = [pad->controller retain];
        GCControllerPlayerIndex index = pad->user_index >= 0 ? (GCControllerPlayerIndex)pad->user_index : GCControllerPlayerIndexUnset;
        dispatch_async(dispatch_get_main_queue(), ^{
            controller.playerIndex = index;
            [controller release];
        });
    }
}

// SDL takes the first free slot when a controller it handles connects (an Xbox controller, say), which can be the
// one a bridged controller is on. Move bridged controllers off slots SDL has taken, and give waiting ones a free one.
static void rehome_pads(void) {
    note_sdl_slots();
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        bridged_pad *pad = &pads[i];
        if (pad->controller == nil || (pad->user_index >= 0 && !sdl_slot_in_use(pad->user_index)))
            continue;
        int old_index = pad->user_index;
        pad->user_index = -1;
        pad->user_index = first_free_user_index();
        if (pad->user_index != old_index) {
            player_changed(pad, old_index);
            // the game only finds it on its new index once it looks again
            pad->found = false;
            if (pad->user_index >= 0)
                controllers_request_gamepad_scan("a bridged controller moved");
        }
    }
}

#pragma mark - input

static int16_t stick_value(float value) {
    float scaled = value * 32767.0f;
    if (scaled > 32767.0f)
        return 32767;
    if (scaled < -32768.0f)
        return -32768;
    return (int16_t)scaled;
}

// runs on input_queue whenever anything on the controller changes
API_AVAILABLE(macos(11.3))
static void update_state(bridged_pad *pad, GCExtendedGamepad *gamepad) {
    uint16_t buttons = 0;
    if (gamepad.dpad.up.pressed) buttons |= 0x0001;
    if (gamepad.dpad.down.pressed) buttons |= 0x0002;
    if (gamepad.dpad.left.pressed) buttons |= 0x0004;
    if (gamepad.dpad.right.pressed) buttons |= 0x0008;
    if (gamepad.buttonMenu.pressed) buttons |= 0x0010;              // Options -> Start
    if (gamepad.buttonOptions.pressed) buttons |= 0x0020;           // Create/Share -> Back
    if (gamepad.leftThumbstickButton.pressed) buttons |= 0x0040;
    if (gamepad.rightThumbstickButton.pressed) buttons |= 0x0080;
    if (gamepad.leftShoulder.pressed) buttons |= 0x0100;
    if (gamepad.rightShoulder.pressed) buttons |= 0x0200;
    if (gamepad.buttonA.pressed) buttons |= 0x1000;                 // Cross
    if (gamepad.buttonB.pressed) buttons |= 0x2000;                 // Circle
    if (gamepad.buttonX.pressed) buttons |= 0x4000;                 // Square
    if (gamepad.buttonY.pressed) buttons |= 0x8000;                 // Triangle
    // the touchpad click is the scoreboard on PlayStation, which is Back for the game
    if ([gamepad isKindOfClass:[GCDualSenseGamepad class]] && ((GCDualSenseGamepad *)gamepad).touchpadButton.pressed)
        buttons |= 0x0020;
    if ([gamepad isKindOfClass:[GCDualShockGamepad class]] && ((GCDualShockGamepad *)gamepad).touchpadButton.pressed)
        buttons |= 0x0020;

    XINPUT_GAMEPAD state = {
        .wButtons = buttons,
        .bLeftTrigger = (uint8_t)(gamepad.leftTrigger.value * 255.0f + 0.5f),
        .bRightTrigger = (uint8_t)(gamepad.rightTrigger.value * 255.0f + 0.5f),
        .sThumbLX = stick_value(gamepad.leftThumbstick.xAxis.value),
        .sThumbLY = stick_value(gamepad.leftThumbstick.yAxis.value),
        .sThumbRX = stick_value(gamepad.rightThumbstick.xAxis.value),
        .sThumbRY = stick_value(gamepad.rightThumbstick.yAxis.value),
    };
    os_unfair_lock_lock(&pads_lock);
    // a late update from a controller that has since disconnected must not land on whatever took its entry
    if (pad->controller == gamepad.controller) {
        pad->state.dwPacketNumber++;
        pad->state.Gamepad = state;
    }
    os_unfair_lock_unlock(&pads_lock);
}

// GyroAim: the controller's turn rate becomes extra right stick deflection, about 2.5 rad/s (140 degrees a second)
// for full deflection at GyroSensitivity 100. It starts just past the game's stick deadzone, so small movements
// still turn the view, then the game's own stick response applies.
#define Gyro_Full_Rate 2.5f
#define Gyro_Min_Deflection 0.15f
#define Gyro_Deadzone 0.012f // rad/s once the bias is removed
#define Gyro_Still 0.05f     // rad/s, below this on every axis the controller counts as resting

static int16_t gyro_deflection(float rate) {
    float magnitude = fabsf(rate);
    if (magnitude < Gyro_Deadzone)
        return 0;
    float deflection = Gyro_Min_Deflection + (1.0f - Gyro_Min_Deflection) * fminf((magnitude - Gyro_Deadzone) / Gyro_Full_Rate, 1.0f);
    return (int16_t)copysignf(deflection * 32767.0f, rate);
}

static int16_t add_stick(int16_t stick, int16_t extra) {
    int sum = stick + extra;
    return sum > 32767 ? 32767 : sum < -32768 ? -32768 : (int16_t)sum;
}

// runs on input_queue for every gyro reading (about 250 a second from a DualSense)
API_AVAILABLE(macos(11.3))
static void gyro_update(bridged_pad *pad, GCMotion *motion) {
    GCRotationRate r = motion.rotationRate;
    float rate[3] = { (float)r.x, (float)r.y, (float)r.z };
    // at rest the gyro reads a small constant rate (about 0.02 rad/s on x on one DualSense), which would slowly
    // turn the view: learn it quickly at first, then slowly, whenever the controller is still
    bool still = true;
    for (int i = 0; i < 3; i++)
        still = still && fabsf(rate[i] - pad->gyro_bias[i]) < Gyro_Still;
    if (still || pad->gyro_samples < 50) {
        float rate_of_learning = pad->gyro_samples < 250 ? 0.05f : 0.002f;
        for (int i = 0; i < 3; i++)
            pad->gyro_bias[i] += (rate[i] - pad->gyro_bias[i]) * rate_of_learning;
    }
    pad->gyro_samples++;
    // turning the controller left (about its up axis, y) looks left; tilting its front up (about x) looks up
    float yaw = -(rate[1] - pad->gyro_bias[1]) * gyro_scale, pitch = (rate[0] - pad->gyro_bias[0]) * gyro_scale;
    if (gyro_invert & 1)
        pitch = -pitch;
    if (gyro_invert & 2)
        yaw = -yaw;
    pad->gyro_smooth[0] += (yaw - pad->gyro_smooth[0]) * 0.35f;
    pad->gyro_smooth[1] += (pitch - pad->gyro_smooth[1]) * 0.35f;
    int16_t x = pad->gyro_samples < 250 ? 0 : gyro_deflection(pad->gyro_smooth[0]);
    int16_t y = pad->gyro_samples < 250 ? 0 : gyro_deflection(pad->gyro_smooth[1]);
    os_unfair_lock_lock(&pads_lock);
    if (pad->controller == motion.controller && (pad->gyro[0] != x || pad->gyro[1] != y)) {
        pad->gyro[0] = x;
        pad->gyro[1] = y;
        pad->state.dwPacketNumber++;
    }
    os_unfair_lock_unlock(&pads_lock);
}

// AdaptiveTriggers: a trigger break on R2 like a gun's, and light resistance on L2 for aiming down sights
API_AVAILABLE(macos(11.3))
static void set_adaptive_triggers(GCController *controller, bool on) {
    if (![controller.extendedGamepad isKindOfClass:[GCDualSenseGamepad class]])
        return;
    GCDualSenseGamepad *dualsense = (GCDualSenseGamepad *)controller.extendedGamepad;
    if (on) {
        [dualsense.rightTrigger setModeWeaponWithStartPosition:0.25f endPosition:0.55f resistiveStrength:0.5f];
        [dualsense.leftTrigger setModeFeedbackWithStartPosition:0.15f resistiveStrength:0.25f];
        // the controller takes a moment to report the new modes back, so check once, a little later
        static atomic_bool checked = false;
        if (!atomic_exchange(&checked, true)) {
            [dualsense retain];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), input_queue, ^{
                printf("GameController bridge: adaptive triggers report R2 mode %ld, L2 mode %ld (0 = off)\n",
                       (long)dualsense.rightTrigger.mode, (long)dualsense.leftTrigger.mode);
                [dualsense release];
            });
        }
    } else {
        [dualsense.rightTrigger setModeOff];
        [dualsense.leftTrigger setModeOff];
    }
}

// DWORD XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState)
install_hook_name(ASL_XInputGetState, uint32_t, uint32_t user_index, XINPUT_STATE *state) {
    if (enabled && user_index < XUSER_MAX_COUNT) {
        os_unfair_lock_lock(&pads_lock);
        rehome_pads();
        bridged_pad *pad = pad_for_user(user_index);
        if (pad != NULL) {
            if (!pad->found) {
                printf("GameController bridge: the game found the %s on player %i\n", pad->kind, user_index + 1);
                pad->found = true;
            }
            // like Aspyr's, a neutral state while XInput is disabled, and also while the game isn't focused
            bool active = atomic_load(&app_active) && (*(volatile uint8_t *)(base_address + ADDR_ASL_XInputEnabled) & 1);
            if (active) {
                *state = pad->state;
                // GyroAim = 2 only aims with the gyro while L2 (aim down sights) is held
                if (gyro_mode == 1 || (gyro_mode == 2 && state->Gamepad.bLeftTrigger > 30)) {
                    state->Gamepad.sThumbRX = add_stick(state->Gamepad.sThumbRX, pad->gyro[0]);
                    state->Gamepad.sThumbRY = add_stick(state->Gamepad.sThumbRY, pad->gyro[1]);
                }
            } else {
                memset(state, 0, sizeof(*state));
            }
            os_unfair_lock_unlock(&pads_lock);
            return ERROR_SUCCESS;
        }
        os_unfair_lock_unlock(&pads_lock);
    }
    return orig_ASL_XInputGetState(user_index, state);
}

#pragma mark - rumble

// CoreHaptics throws an Objective-C exception, instead of returning an error, when a player's engine isn't
// running (it stopped on its own, or the controller just went away), so every call into it is wrapped
static void haptics_exception(NSException *exception, const char *where) {
    static int said = 0;
    if (said++ < 5)
        printf("GameController bridge: rumble %s failed: %s\n", where, exception.reason.UTF8String ?: "?");
}

API_AVAILABLE(macos(11.3))
static id make_player(CHHapticEngine *engine) {
    // a continuous event that never ends, or if CoreHaptics won't take that, a 30 second one. (Not an advanced
    // player, which could loop it: those never work with controller haptics on macOS, "Couldn't communicate with
    // a helper application".)
    const float durations[2] = { GCHapticDurationInfinite, 30.0f };
    for (int i = 0; i < 2; i++) {
        NSError *error = nil;
        CHHapticEventParameter *intensity =
            [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f] autorelease];
        CHHapticEvent *event = [[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                              parameters:@[ intensity ]
                                                            relativeTime:0
                                                                duration:durations[i]] autorelease];
        CHHapticPattern *pattern = [[[CHHapticPattern alloc] initWithEvents:@[ event ] parameters:@[] error:&error] autorelease];
        id<CHHapticPatternPlayer> player = nil;
        @try {
            player = pattern != nil ? [engine createPlayerWithPattern:pattern error:&error] : nil;
        } @catch (NSException *exception) {
            haptics_exception(exception, "setup");
        }
        if (player != nil)
            return [player retain];
        printf("GameController bridge: couldn't make the %s rumble pattern: %s\n", i == 0 ? "endless" : "30 second",
               error.localizedDescription.UTF8String ?: "unknown error");
    }
    return nil;
}


// the motors run as continuous events at full strength, scaled by a dynamic intensity, so speed changes don't
// restart anything
API_AVAILABLE(macos(11.3))
static void set_motors(bridged_pad *pad, uint16_t left, uint16_t right) {
    uint16_t speeds[2] = { left, right };
    if (pad->engine_count == 1)
        speeds[0] = left > right ? left : right;    // one engine for both plays the stronger one
    for (int i = 0; i < pad->engine_count; i++) {
        id<CHHapticPatternPlayer> player = pad->players[i];
        if (player == nil || pad->motors[i] == speeds[i])
            continue;
        NSError *error = nil;
        @try {
        if (speeds[i] == 0) {
            [player stopAtTime:CHHapticTimeImmediate error:&error];
        } else {
            // macOS can stop an engine on its own (the game lost focus, say), start it again if so
            if (pad->motors[i] == 0 && ![player startAtTime:CHHapticTimeImmediate error:&error] &&
                [pad->engines[i] startAndReturnError:nil])
                [player startAtTime:CHHapticTimeImmediate error:&error];
            CHHapticDynamicParameter *intensity =
                [[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                                                                 value:speeds[i] / 65535.0f
                                                          relativeTime:0] autorelease];
            BOOL sent = [player sendParameters:@[ intensity ] atTime:CHHapticTimeImmediate error:&error];
            static bool said = false;
            if (!said) {
                printf("GameController bridge: first rumble (motor %i at %.0f%%): %s\n", i, speeds[i] / 655.35f,
                       sent ? "playing" : error.localizedDescription.UTF8String ?: "failed");
                said = true;
            }
        }
        } @catch (NSException *exception) {
            haptics_exception(exception, "change");
        }
        pad->motors[i] = speeds[i];
    }
}

// on input_queue: play what the game last asked for, or nothing while it isn't focused
API_AVAILABLE(macos(11.3))
static void start_haptics(bridged_pad *pad, GCController *controller, int user_index);

API_AVAILABLE(macos(11.3))
static void apply_motors(bridged_pad *pad) {
    os_unfair_lock_lock(&pads_lock);
    uint16_t left = pad->wanted_motors[0], right = pad->wanted_motors[1];
    GCController *controller = [pad->controller retain];
    int user_index = pad->user_index;
    os_unfair_lock_unlock(&pads_lock);
    if (controller == nil || !atomic_load(&app_active))
        left = right = 0;
    // rumble couldn't be set up when the controller connected (the game may not have been frontmost yet),
    // so try again when the game first asks for it, at most every 3 seconds
    if (pad->engine_count == 0 && (left != 0 || right != 0) && CFAbsoluteTimeGetCurrent() >= pad->haptics_retry_at)
        start_haptics(pad, controller, user_index);
    [controller release];
    set_motors(pad, left, right);
}

API_AVAILABLE(macos(11.3))
static void restart_haptics(bridged_pad *pad) {
    // after a reset (the haptics server restarted, the Mac slept) each engine has to be started again and its
    // players made again
    for (int i = 0; i < pad->engine_count; i++) {
        [pad->players[i] release];
        pad->players[i] = nil;
        @try {
            if ([pad->engines[i] startAndReturnError:nil])
                pad->players[i] = make_player(pad->engines[i]);
        } @catch (NSException *exception) {
            haptics_exception(exception, "restart");
        }
    }
    pad->motors[0] = pad->motors[1] = 0;
    apply_motors(pad);
}

// on input_queue. DualSense and DualShock 4 have a motor in each grip, so one engine per grip where macOS offers it
// makes, starts and keeps one rumble engine for this locality, returns 0 (saying why) if macOS won't
API_AVAILABLE(macos(11.3))
static int add_engine(bridged_pad *pad, GCDeviceHaptics *haptics, GCHapticsLocality locality, int user_index) {
    CHHapticEngine *engine = [haptics createEngineWithLocality:locality];
    if (engine == nil) {
        printf("GameController bridge: macOS made no rumble engine for %s on player %i\n", locality.UTF8String, user_index + 1);
        return 0;
    }
    engine.playsHapticsOnly = YES;
    NSError *error = nil;
    if (![engine startAndReturnError:&error]) {
        printf("GameController bridge: couldn't start rumble (%s) on player %i: %s\n", locality.UTF8String, user_index + 1,
               error.localizedDescription.UTF8String ?: "unknown error");
        return 0;
    }
    id player = make_player(engine);
    if (player == nil) {
        [engine stopWithCompletionHandler:nil];
        return 0;
    }
    engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
        printf("GameController bridge: rumble engine on player %i stopped (reason %li)\n", user_index + 1, (long)reason);
    };
    engine.resetHandler = ^{
        dispatch_async(input_queue, ^{
            @autoreleasepool {
                restart_haptics(pad);
            }
        });
    };
    pad->engines[pad->engine_count] = [engine retain];
    pad->players[pad->engine_count] = player;
    pad->engine_count++;
    return 1;
}

API_AVAILABLE(macos(11.3))
static void start_haptics(bridged_pad *pad, GCController *controller, int user_index) {
    pad->haptics_retry_at = CFAbsoluteTimeGetCurrent() + 3.0;
    GCDeviceHaptics *haptics = controller.haptics;
    if (haptics == nil) {
        printf("GameController bridge: no rumble on player %i, macOS gives no haptics for this controller\n", user_index + 1);
        return;
    }
    NSSet *localities = haptics.supportedLocalities;
    bool grips = [localities containsObject:GCHapticsLocalityLeftHandle] && [localities containsObject:GCHapticsLocalityRightHandle];
    pad->engine_count = 0;
    // one engine per grip for the game's two motors, otherwise (or if that fails) one engine for both
    if (grips && add_engine(pad, haptics, GCHapticsLocalityLeftHandle, user_index))
        add_engine(pad, haptics, GCHapticsLocalityRightHandle, user_index);
    if (pad->engine_count == 0)
        add_engine(pad, haptics, GCHapticsLocalityDefault, user_index);
    if (pad->engine_count == 2)
        printf("GameController bridge: rumble ready on player %i (left and right grip)\n", user_index + 1);
    else if (pad->engine_count == 1)
        printf("GameController bridge: rumble ready on player %i (both motors on one engine)\n", user_index + 1);
    else
        printf("GameController bridge: no rumble on player %i\n", user_index + 1);
}

API_AVAILABLE(macos(11.3))
static void stop_haptics(bridged_pad *pad) {
    for (int i = 0; i < pad->engine_count; i++) {
        @try {
            [pad->players[i] stopAtTime:CHHapticTimeImmediate error:nil];
            [pad->engines[i] stopWithCompletionHandler:nil];
        } @catch (NSException *exception) {
            haptics_exception(exception, "stop");
        }
        [pad->players[i] release];
        [pad->engines[i] release];
        pad->players[i] = nil;
        pad->engines[i] = nil;
    }
    pad->engine_count = 0;
    pad->motors[0] = pad->motors[1] = 0;
}

// DWORD XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration)
install_hook_name(ASL_XInputSetState, uint32_t, uint32_t user_index, XINPUT_VIBRATION *vibration) {
    if (enabled && user_index < XUSER_MAX_COUNT && vibration != NULL) {
        os_unfair_lock_lock(&pads_lock);
        bridged_pad *pad = pad_for_user(user_index);
        if (pad != NULL) {
            uint16_t left = vibration->wLeftMotorSpeed, right = vibration->wRightMotorSpeed;
            bool changed = pad->wanted_motors[0] != left || pad->wanted_motors[1] != right;
            pad->wanted_motors[0] = left;
            pad->wanted_motors[1] = right;
            os_unfair_lock_unlock(&pads_lock);
            // the game sends the same speeds every frame, only pass on changes
            if (changed) {
                if (@available(macOS 11.3, *)) {
                    dispatch_async(input_queue, ^{
                        @autoreleasepool {
                            apply_motors(pad);
                        }
                    });
                }
            }
            return ERROR_SUCCESS;
        }
        os_unfair_lock_unlock(&pads_lock);
    }
    return orig_ASL_XInputSetState(user_index, vibration);
}

#pragma mark - controllers coming and going

static bool is_playstation_product(int product) {
    return product == 0x05C4 || product == 0x09CC || product == 0x0BA0 || product == 0x0CE6 || product == 0x0DF2;
}

// the GameController framework doesn't say how a controller is connected, IOKit does
static const char *sony_transport(void) {
    bool usb = false, bluetooth = false;
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (manager == NULL)
        return "unknown connection";
    int vendor = SONY_VENDOR_ID;
    CFNumberRef vendor_ref = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
    const void *keys[] = { CFSTR(kIOHIDVendorIDKey) };
    const void *values[] = { vendor_ref };
    CFDictionaryRef matching = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(manager, matching);
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (devices != NULL) {
        CFIndex count = CFSetGetCount(devices);
        IOHIDDeviceRef *list = calloc(count, sizeof(IOHIDDeviceRef));
        CFSetGetValues(devices, (const void **)list);
        for (CFIndex i = 0; i < count; i++) {
            int product = 0;
            CFTypeRef product_ref = IOHIDDeviceGetProperty(list[i], CFSTR(kIOHIDProductIDKey));
            if (product_ref != NULL && CFGetTypeID(product_ref) == CFNumberGetTypeID())
                CFNumberGetValue(product_ref, kCFNumberIntType, &product);
            CFTypeRef transport = IOHIDDeviceGetProperty(list[i], CFSTR(kIOHIDTransportKey));
            if (!is_playstation_product(product) || transport == NULL || CFGetTypeID(transport) != CFStringGetTypeID())
                continue;
            if (CFStringCompare(transport, CFSTR("USB"), 0) == kCFCompareEqualTo)
                usb = true;
            else if (CFStringHasPrefix(transport, CFSTR("Bluetooth")))
                bluetooth = true;
        }
        free(list);
        CFRelease(devices);
    }
    CFRelease(matching);
    CFRelease(vendor_ref);
    CFRelease(manager);
    return usb && bluetooth ? "USB or Bluetooth" : usb ? "USB" : bluetooth ? "Bluetooth" : "unknown connection";
}

API_AVAILABLE(macos(11.3))
static void controller_disconnected(GCController *controller);

// a controller that comes back (over Bluetooth, say) is a new GCController. Should macOS not have said the old one
// went away, it would keep its player slot with frozen input, so drop bridged controllers macOS no longer lists
API_AVAILABLE(macos(11.3))
static void drop_unlisted_pads(GCController *connected) {
    NSArray<GCController *> *listed = GCController.controllers;
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        os_unfair_lock_lock(&pads_lock);
        GCController *controller = [pads[i].controller retain];
        const char *kind = pads[i].kind;
        os_unfair_lock_unlock(&pads_lock);
        if (controller != nil && controller != connected && ![listed containsObject:controller]) {
            printf("GameController bridge: macOS no longer lists a bridged %s, dropping it\n", kind);
            controller_disconnected(controller);
        }
        [controller release];
    }
}

API_AVAILABLE(macos(11.3))
static void controller_connected(GCController *controller) {
    GCExtendedGamepad *gamepad = controller.extendedGamepad;
    const char *kind = NULL;
    if ([gamepad isKindOfClass:[GCDualSenseGamepad class]])
        kind = "DualSense";
    else if ([gamepad isKindOfClass:[GCDualShockGamepad class]])
        kind = "DualShock 4";
    if (kind == NULL) {
        // Xbox and other controllers stay with SDL, which Aspyr takes them from, but the game still has to look
        printf("GameController bridge: %s connected, it's left to SDL\n", controller.vendorName.UTF8String ?: "a controller");
        controllers_request_gamepad_scan("a controller connected");
        return;
    }
    drop_unlisted_pads(controller);

    os_unfair_lock_lock(&pads_lock);
    bridged_pad *pad = NULL;
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        if (pads[i].controller == controller) {
            os_unfair_lock_unlock(&pads_lock);
            return;
        }
        if (pad == NULL && pads[i].controller == nil)
            pad = &pads[i];
    }
    if (pad == NULL) {
        os_unfair_lock_unlock(&pads_lock);
        printf("GameController bridge: %s connected, but 4 controllers are already bridged\n", kind);
        return;
    }
    memset(&pad->state, 0, sizeof(pad->state));
    pad->wanted_motors[0] = pad->wanted_motors[1] = 0;
    pad->gyro[0] = pad->gyro[1] = 0;
    pad->kind = kind;
    pad->found = false;
    note_sdl_slots();
    pad->user_index = first_free_user_index();
    pad->controller = [controller retain];
    int user_index = pad->user_index;
    os_unfair_lock_unlock(&pads_lock);

    // a DualSense over Bluetooth reports its level with an unknown charging state
    char battery[48] = "";
    GCDeviceBattery *device_battery = controller.battery;
    if (device_battery != nil && device_battery.batteryLevel > 0.0f)
        snprintf(battery, sizeof(battery), ", battery %i%%%s", (int)(device_battery.batteryLevel * 100.0f + 0.5f),
                 device_battery.batteryState == GCDeviceBatteryStateCharging ? " and charging" :
                 device_battery.batteryState == GCDeviceBatteryStateFull ? " (full)" : "");
    if (user_index >= 0)
        printf("GameController bridge: %s connected as player %i (%s)%s\n", kind, user_index + 1, sony_transport(), battery);
    else
        printf("GameController bridge: %s connected (%s)%s, waiting for a free player slot\n", kind, sony_transport(), battery);

    controller.playerIndex = user_index >= 0 ? (GCControllerPlayerIndex)user_index : GCControllerPlayerIndexUnset;
    if (lightbar_color < 0) {
        printf("GameController bridge: lightbar left as it is (no LightbarColor in settings.ini)\n");
    } else if (controller.light != nil) {
        controller.light.color = [[[GCColor alloc] initWithRed:((lightbar_color >> 16) & 0xFF) / 255.0f
                                                         green:((lightbar_color >> 8) & 0xFF) / 255.0f
                                                          blue:(lightbar_color & 0xFF) / 255.0f] autorelease];
        printf("GameController bridge: lightbar set to %06X\n", lightbar_color);
    } else {
        printf("GameController bridge: this %s has no lightbar macOS can set\n", kind);
    }

    controller.handlerQueue = input_queue;
    gamepad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
        update_state(pad, changed);
    };
    GCMotion *motion = controller.motion;
    if (gyro_mode != 0 && motion != nil && motion.hasRotationRate) {
        if (motion.sensorsRequireManualActivation)
            motion.sensorsActive = YES;
        motion.valueChangedHandler = ^(GCMotion *changed) {
            gyro_update(pad, changed);
        };
        printf("GameController bridge: gyro aim on for player %i (%s, sensitivity %.0f%%)\n", user_index + 1,
               gyro_mode == 2 ? "while L2 is held" : "always", gyro_scale * 100.0f);
    } else if (gyro_mode != 0) {
        printf("GameController bridge: this %s has no gyro macOS can read\n", kind);
    }
    if (adaptive_triggers && [gamepad isKindOfClass:[GCDualSenseGamepad class]]) {
        set_adaptive_triggers(controller, true);
        GCDualSenseGamepad *dualsense = (GCDualSenseGamepad *)gamepad;
        printf("GameController bridge: adaptive triggers on (R2 mode %ld, L2 mode %ld)\n", (long)dualsense.rightTrigger.mode,
               (long)dualsense.leftTrigger.mode);
    }
    dispatch_async(input_queue, ^{
        @autoreleasepool {
            pad->gyro_samples = 0;
            pad->gyro_smooth[0] = pad->gyro_smooth[1] = 0.0f;
            update_state(pad, gamepad);
            start_haptics(pad, controller, user_index);
        }
    });
    // the game only looks for controllers at startup and when asked
    if (user_index >= 0)
        controllers_request_gamepad_scan("a PlayStation controller connected");
}

API_AVAILABLE(macos(11.3))
static void controller_disconnected(GCController *controller) {
    bool bridged = false;
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        bridged_pad *pad = &pads[i];
        os_unfair_lock_lock(&pads_lock);
        bool match = pad->controller == controller;
        int user_index = pad->user_index;
        if (match)
            pad->controller = nil; // the game stops seeing it right away
        os_unfair_lock_unlock(&pads_lock);
        if (!match)
            continue;
        bridged = true;
        controller.extendedGamepad.valueChangedHandler = nil;
        controller.motion.valueChangedHandler = nil;
        if (user_index >= 0)
            printf("GameController bridge: %s on player %i disconnected\n", pad->kind, user_index + 1);
        else
            printf("GameController bridge: %s disconnected\n", pad->kind);
        // rumble is torn down on the input queue, after anything still queued for this controller
        dispatch_async(input_queue, ^{
            @autoreleasepool {
                stop_haptics(pad);
                [controller release];
            }
        });
    }
    if (!bridged)
        printf("GameController bridge: %s disconnected\n", controller.vendorName.UTF8String ?: "a controller");
    // nothing to ask the game here, it notices a controller it reads going away by itself
}

// in the background macOS stops sending controller input, so a held button would stay held: send the game a neutral
// state and stop rumble until it's focused again, then pick up the controllers' current state
API_AVAILABLE(macos(11.3))
static void app_focus_changed(bool active) {
    if (atomic_exchange(&app_active, active) == active)
        return;
    dispatch_async(input_queue, ^{
        @autoreleasepool {
            for (int i = 0; i < XUSER_MAX_COUNT; i++) {
                bridged_pad *pad = &pads[i];
                os_unfair_lock_lock(&pads_lock);
                GCController *controller = [pad->controller retain];
                os_unfair_lock_unlock(&pads_lock);
                if (controller == nil)
                    continue;
                if (active)
                    update_state(pad, controller.extendedGamepad);
                apply_motors(pad);
                if (adaptive_triggers)
                    set_adaptive_triggers(controller, active);
                [controller release];
            }
        }
    });
}

API_AVAILABLE(macos(11.3))
static void gc_setup(void) {
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    NSOperationQueue *main_queue = [NSOperationQueue mainQueue];
    [center addObserverForName:GCControllerDidConnectNotification object:nil queue:main_queue usingBlock:^(NSNotification *note) {
        controller_connected(note.object);
    }];
    [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:main_queue usingBlock:^(NSNotification *note) {
        controller_disconnected(note.object);
    }];
    // AppKit's NSApplicationDidResignActiveNotification and NSApplicationDidBecomeActiveNotification
    [center addObserverForName:@"NSApplicationDidResignActiveNotification" object:nil queue:main_queue usingBlock:^(NSNotification *note) {
        app_focus_changed(false);
    }];
    [center addObserverForName:@"NSApplicationDidBecomeActiveNotification" object:nil queue:main_queue usingBlock:^(NSNotification *note) {
        app_focus_changed(true);
    }];
    NSArray<GCController *> *controllers = GCController.controllers;
    printf("GameController bridge: watching for controllers, macOS lists %lu now\n", (unsigned long)controllers.count);
    for (GCController *controller in controllers)
        controller_connected(controller);
}

#pragma mark - setup

bool gc_bridge_enabled(void) {
    return enabled;
}

bool gc_bridge_is_playstation_user(int user_index) {
    if (!enabled || user_index < 0 || user_index >= XUSER_MAX_COUNT)
        return false;
    os_unfair_lock_lock(&pads_lock);
    bool playstation = pad_for_user(user_index) != NULL; // only PlayStation controllers are bridged
    os_unfair_lock_unlock(&pads_lock);
    return playstation;
}

void gc_bridge_set_dualsense_options(int gyro, int gyro_sensitivity, int gyro_invert_axes, int triggers) {
    const char *env = getenv("BO3MACFIX_GYROAIM");
    gyro_mode = env != NULL ? atoi(env) : gyro;
    env = getenv("BO3MACFIX_GYROSENSITIVITY");
    gyro_scale = (env != NULL ? atoi(env) : gyro_sensitivity) / 100.0f;
    env = getenv("BO3MACFIX_GYROINVERT");
    gyro_invert = env != NULL ? atoi(env) : gyro_invert_axes;
    env = getenv("BO3MACFIX_ADAPTIVETRIGGERS");
    adaptive_triggers = (env != NULL ? atoi(env) : triggers) != 0;
}

static int parse_rgb(const char *text) {
    text += strspn(text, " \t\"#");
    return (int)strtol(text, NULL, 16);
}

void gc_bridge_init(uint64_t base, int mode, int lightbar_rgb) {
    const char *env = getenv("BO3MACFIX_CONTROLLERBRIDGE");
    if (env != NULL)
        mode = atoi(env);
    env = getenv("BO3MACFIX_LIGHTBARCOLOR");
    if (env != NULL)
        lightbar_rgb = parse_rgb(env);
    if (mode == 0)
        return;

    if (@available(macOS 11.3, *)) {
        // push rbp; mov rbp, rsp; push r14; push rbx; sub rsp, 0x80 - both start the same way
        static const uint8_t expected[] = { 0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 };
        void *get_state = (void *)(base + ADDR_ASL_XInputGetState);
        void *set_state = (void *)(base + ADDR_ASL_XInputSetState);
        if (memcmp(prepatch_original_code(get_state, sizeof(expected)), expected, sizeof(expected)) != 0 ||
            memcmp(prepatch_original_code(set_state, sizeof(expected)), expected, sizeof(expected)) != 0) {
            printf("unexpected code at %08x or %08x, GameController bridge off\n", ADDR_ASL_XInputGetState, ADDR_ASL_XInputSetState);
            return;
        }
        base_address = base;
        lightbar_color = lightbar_rgb > 0xFFFFFF ? -1 : lightbar_rgb;
        input_queue = dispatch_queue_create("BO3MacFix controller input", DISPATCH_QUEUE_SERIAL);
        install_hook_ASL_XInputGetState(get_state);
        install_hook_ASL_XInputSetState(set_state);
        enabled = true;
        // the GameController framework needs the app's run loop, which runs once the game is up
        dispatch_async(dispatch_get_main_queue(), ^{
            @autoreleasepool {
                gc_setup();
            }
        });
        printf("GameController bridge: on, DualSense and DualShock 4 controllers go through macOS instead of SDL\n");
    } else {
        printf("GameController bridge: needs macOS 11.3 or newer, staying off\n");
    }
}

#pragma mark - changes from the settings menu

// GyroAim switched on or off while the game runs: start or stop the connected controllers' motion sensors
static void apply_gyro_mode(int mode) {
    if (!enabled)
        return;
    if (@available(macOS 11.3, *)) {
        dispatch_async(dispatch_get_main_queue(), ^{
            @autoreleasepool {
                for (int i = 0; i < XUSER_MAX_COUNT; i++) {
                    bridged_pad *pad = &pads[i];
                    os_unfair_lock_lock(&pads_lock);
                    GCController *controller = [pad->controller retain];
                    os_unfair_lock_unlock(&pads_lock);
                    GCMotion *motion = controller.motion;
                    if (motion == nil || !motion.hasRotationRate) {
                        [controller release];
                        continue;
                    }
                    if (mode != 0) {
                        dispatch_async(input_queue, ^{
                            pad->gyro_samples = 0;
                            pad->gyro_smooth[0] = pad->gyro_smooth[1] = 0.0f;
                        });
                        if (motion.sensorsRequireManualActivation)
                            motion.sensorsActive = YES;
                        motion.valueChangedHandler = ^(GCMotion *changed) {
                            gyro_update(pad, changed);
                        };
                    } else {
                        motion.valueChangedHandler = nil;
                        if (motion.sensorsRequireManualActivation)
                            motion.sensorsActive = NO;
                    }
                    [controller release];
                }
            }
        });
    }
}

void gc_bridge_set_gyro_mode(int mode) {
    int was = gyro_mode;
    gyro_mode = mode;
    if ((was != 0) != (mode != 0))
        apply_gyro_mode(mode);
    printf("GameController bridge: gyro aim %s\n", mode == 0 ? "off" : mode == 2 ? "on while L2 is held" : "on");
}

void gc_bridge_set_gyro_sensitivity(int percent) {
    gyro_scale = (percent > 0 ? percent : 100) / 100.0f;
}

void gc_bridge_set_gyro_invert(int axes) {
    gyro_invert = axes;
}

void gc_bridge_set_adaptive_triggers(int on) {
    adaptive_triggers = on != 0;
    if (!enabled)
        return;
    if (@available(macOS 11.3, *)) {
        dispatch_async(input_queue, ^{
            @autoreleasepool {
                for (int i = 0; i < XUSER_MAX_COUNT; i++) {
                    os_unfair_lock_lock(&pads_lock);
                    GCController *controller = [pads[i].controller retain];
                    os_unfair_lock_unlock(&pads_lock);
                    if (controller != nil)
                        set_adaptive_triggers(controller, on != 0 && atomic_load(&app_active));
                    [controller release];
                }
            }
        });
    }
}

// -1 means leave the lightbar alone, which can't bring back a colour already set, so it only stops changing it
void gc_bridge_set_lightbar(int rgb) {
    lightbar_color = rgb > 0xFFFFFF ? -1 : rgb;
    if (!enabled || lightbar_color < 0)
        return;
    int color = lightbar_color;
    if (@available(macOS 11.3, *)) {
        dispatch_async(dispatch_get_main_queue(), ^{
            @autoreleasepool {
                for (int i = 0; i < XUSER_MAX_COUNT; i++) {
                    os_unfair_lock_lock(&pads_lock);
                    GCController *controller = [pads[i].controller retain];
                    os_unfair_lock_unlock(&pads_lock);
                    if (controller.light != nil)
                        controller.light.color = [[[GCColor alloc] initWithRed:((color >> 16) & 0xFF) / 255.0f
                                                                         green:((color >> 8) & 0xFF) / 255.0f
                                                                          blue:(color & 0xFF) / 255.0f] autorelease];
                    [controller release];
                }
            }
        });
    }
    printf("GameController bridge: lightbar set to %06X\n", color);
}
