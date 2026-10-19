// AvoidNotch's geometry: where the game's picture goes below a display's camera housing, in the numbers ASL's window
// keeps (see notch.m). Plain C, no game or display needed (tests/test_notch.c).
#pragma once
#include <stdbool.h>

// What Window_UpdateLetterbox (0x1000ee14c) fills: Window_ConvertCoords (0x1000ee866) maps the mouse from points in the
// window to the game's logical units as ((x - pillar) * scale, (y - top) * scale), and back.
typedef struct {
    double scale; // logical units per point, the same across and down
    int pillar;   // points left of the picture (and right of it)
    int top;      // points above it: the camera housing, plus half the height the aspect ratio leaves over
} NotchLayout;

typedef struct {
    int x, y, w, h;
} NotchRect;

// The biggest picture with the logical size's aspect ratio that fits in a window_w x window_h point window below its
// top inset points, centred in what's left. False (nothing to do) when there's no inset or a size is unusable.
bool notch_layout(int logical_w, int logical_h, int window_w, int window_h, int inset, NotchLayout *out);

// What the Metal client rect (0x1000ee6c6) sets for a frame_w x frame_h frame in that layout: the drawable to force,
// which the layer stretches over the whole window, and where the frame goes in it (the blit rect), so the frame lands on
// the layout's picture. Aspyr's version centres the frame; the bars stay black from present's clear pass.
bool notch_drawable(const NotchLayout *layout, int logical_w, int logical_h, int window_w, int window_h, int frame_w,
                    int frame_h, int *drawable_w, int *drawable_h, NotchRect *blit);

// The blit rect in a forced_w x forced_h drawable, scaled to a drawable_w x drawable_h one (MetalFX's output, which
// metalfx.m makes the view's pixels) and kept inside it.
bool notch_scale_rect(int forced_w, int forced_h, int drawable_w, int drawable_h, NotchRect blit, NotchRect *out);
