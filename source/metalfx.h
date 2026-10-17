#pragma once

#include <stddef.h>

// MetalFX spatial upscaling of the whole frame when Aspyr's Metal layer presents it, see metalfx.m.

// checks that MetalFX and Aspyr's Metal view are there and starts keeping the drawable at the screen's native size,
// returns 1 if upscaling is on, otherwise logs why not and returns 0
int metalfx_init(void);

// replacement for Aspyr's [blitEncoder copyFromTexture:backbuffer ... toTexture:drawable.texture ...] at 0x1000e8fde
void *metalfx_copy_function(void);

// encodes the upscale of this frame, call right before [commandBuffer presentDrawable:]
void metalfx_before_present(void *command_buffer);

// the highest frame rate of the display the game is on, from display_follow_current() when it has the display, else
// from the game window's screen (only seen while MetalFX is on), 0 if not known. Any thread.
int present_display_max_fps(void);

// 1 if that display's refresh rate is variable (ProMotion or Adaptive-Sync), so any frame rate comes out evenly
int present_display_variable_refresh(void);

// writes "drawable=WxH metalfx=WxH->WxH" (or "metalfx=off") for the display state line
void present_describe(char *buf, size_t n);
