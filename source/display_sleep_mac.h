// IdleDisplaySleep's glue: see display_sleep.h for what it does and display_sleep_mac.c for how.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// at startup: the setting (Display_Sleep_*), before SDL makes its assertion
void display_sleep_init(int mode);

// from the settings page, live
void display_sleep_set_mode(int mode);

// from any thread whenever the situation may have changed (notices.lua's polls, a map loading, the game going to the
// background or coming back, the power source): whether nobody can be playing (frame_limit_away_from_play) and
// whether the Mac runs on battery
void display_sleep_update(bool away, bool on_battery);

// from the controller code for each state the game reads (every frame): while the display may turn off, a player's
// input is declared as user activity (at most every 10 s)
void display_sleep_controller(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, int16_t left_x,
                              int16_t left_y, int16_t right_x, int16_t right_y);
