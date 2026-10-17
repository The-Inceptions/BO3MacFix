#pragma once
#include <stdint.h>

// enabled: RawMouse (0/1), scale_percent: RawMouseScale, how far the view turns per raw count (100 = 1:1)
void rawmouse_init(uint64_t base_address, int enabled, int scale_percent);

// RawMouseScale changed while the game runs
void rawmouse_set_scale(int scale_percent);
