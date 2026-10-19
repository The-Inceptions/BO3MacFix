// On-screen notices in the game (notices.m, drawn by notices.lua): a controller connecting or disconnecting, its battery
// running low, the game moving to another display. And PauseOnDisconnect: opening the pause menu when the controller
// goes away mid-match, in games that can pause.
#pragma once
#include "havok.h"

// notices: Notices (1 = on). pause_on_disconnect: PauseOnDisconnect (1 = on). With BO3MACFIX_TESTHOOKS=1 it also listens
// for the test notifications listed in notices.m.
void notices_init(int notices, int pause_on_disconnect);

// the settings page changed them
void notices_set_enabled(int on);
void notices_set_pause_on_disconnect(int on);

// the controller the game reads went away and no other took its place (gc_bridge.m): with PauseOnDisconnect on, asks
// the UI to pause (thread-safe)
void notices_controller_lost(void);

// shows text in the game (thread-safe), or only logs it while Notices is off
void notices_post(const char *format, ...) __attribute__((format(printf, 1, 2)));

// adds BO3MacFix.TakeNotice and friends to a UI Lua state that has the BO3MacFix table, and has its first menu load
// notices.lua (called from settings_menu.c, once for each new UI state, which also ends PauseMenuFPS's limit)
void notices_lua_install(lua_State *s);
