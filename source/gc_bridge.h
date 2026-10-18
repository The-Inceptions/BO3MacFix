#pragma once
#include <stdbool.h>
#include <stdint.h>

// DualSense and DualShock 4 controllers through macOS's GameController framework instead of the game's SDL 2.0.7:
// works over Bluetooth and USB, with rumble and the lightbar. Needs macOS 11.3 or newer.
// mode: 0 = off, 1 = on. lightbar_rgb: 0xRRGGBB, or -1 to leave the light alone.
// BO3MACFIX_CONTROLLERBRIDGE and BO3MACFIX_LIGHTBARCOLOR (RRGGBB) at env override both.
void gc_bridge_init(uint64_t base_address, int mode, int lightbar_rgb);

// DualSense extras for bridged controllers, call before gc_bridge_init. gyro: 0 = off, 1 = gyro aiming always,
// 2 = only while L2 is held. gyro_sensitivity: percent. gyro_invert: 1 = vertical, 2 = horizontal, 3 = both.
// triggers: 1 = adaptive trigger resistance on L2 and R2. Env: BO3MACFIX_GYROAIM, _GYROSENSITIVITY, _GYROINVERT,
// _ADAPTIVETRIGGERS.
void gc_bridge_set_dualsense_options(int gyro, int gyro_sensitivity, int gyro_invert, int triggers);

// is the bridge on? controllers.c then keeps the game's SDL away from Sony controllers, so they don't give double input
bool gc_bridge_enabled(void);

// is a PlayStation controller from the bridge on this XInput user index? for ButtonPrompts = 0
bool gc_bridge_is_playstation_user(int user_index);

// turns adaptive triggers off before the game ends, once (also from atexit and AppKit terminating); from any thread
void gc_bridge_quit(void);

// changes from the settings menu while the game runs, applied to connected controllers right away
void gc_bridge_set_gyro_mode(int mode);
void gc_bridge_set_gyro_sensitivity(int percent);
void gc_bridge_set_gyro_invert(int axes);
void gc_bridge_set_adaptive_triggers(int on);
void gc_bridge_set_lightbar(int rgb);
