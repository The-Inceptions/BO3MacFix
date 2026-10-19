#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDevice.h>

// PlayStation controller support: SDL mappings for the DualSense and DualShock 4, and PlayStation button prompts.
// add_mappings: 0 = off, 1 = on. button_prompts: 0 = auto, 1 = always Xbox (stock), 2 = always PlayStation.
void controllers_init(uint64_t base_address, int add_mappings, int button_prompts);

// called from the IOHIDDeviceGetProperty interposer, returns the value BO3's SDL 2.0.7 should see
CFTypeRef controllers_hid_property(IOHIDDeviceRef device, CFStringRef key, CFTypeRef value);

// ButtonPrompts changed while the game runs (false: needs a restart), and whether that can happen now
bool controllers_set_button_prompts(int mode);
bool controllers_prompts_live(void);

// a controller connected while the game runs: asks the game to look for controllers again, as Windows' device change
// message would. From any thread; does nothing until the game's gamepad code is up, and requests close together make
// one. why: a string literal for the log.
void controllers_request_gamepad_scan(const char *why);

// the XInput user index (player, 0-3) the game's first gamepad reads, or -1: none, or the game's gamepad code isn't up
int controllers_game_user_index(void);
