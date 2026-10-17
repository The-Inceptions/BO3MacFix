// Darwin notifications the display tests drive the game with (BO3MACFIX_TESTHOOKS=1 only), see display_testhooks.m.
#pragma once
#include <stdint.h>

void display_testhooks_init(uint64_t game_base_address);
