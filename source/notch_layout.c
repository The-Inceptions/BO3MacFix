// AvoidNotch's geometry, see notch_layout.h.
#include <stddef.h>
#include "notch_layout.h"

#define Max_Drawable 16384 // Metal's largest texture

// how Aspyr rounds its letterbox and client rect: half away from zero, then truncated (cvttsd2si)
static int aspyr_round(double value) {
    return (int)(value + (value > 0 ? 0.5 : -0.5));
}

bool notch_layout(int logical_w, int logical_h, int window_w, int window_h, int inset, NotchLayout *out) {
    // a camera housing is about 3% of the screen's height; anything much bigger isn't one
    if (out == NULL || logical_w <= 0 || logical_h <= 0 || window_w <= 0 || inset <= 0 || inset * 4 > window_h)
        return false;
    int below = window_h - inset;
    // Window_UpdateLetterbox's arithmetic, in the window below the housing: the side that fills sets the scale
    if ((double)window_w / below > (double)logical_w / logical_h) {
        out->scale = (double)logical_h / below;
        out->pillar = aspyr_round((window_w - logical_w / out->scale) * 0.5);
        out->top = inset;
    } else {
        out->scale = (double)logical_w / window_w;
        out->pillar = 0;
        out->top = inset + aspyr_round((below - logical_h / out->scale) * 0.5);
    }
    return true;
}

bool notch_drawable(const NotchLayout *layout, int logical_w, int logical_h, int window_w, int window_h, int frame_w,
                    int frame_h, int *drawable_w, int *drawable_h, NotchRect *blit) {
    if (layout == NULL || drawable_w == NULL || drawable_h == NULL || blit == NULL || !(layout->scale > 0) ||
        logical_w <= 0 || logical_h <= 0 || window_w <= 0 || window_h <= 0 || frame_w <= 0 || frame_h <= 0)
        return false;
    // the whole window at the frame's pixels per point (Aspyr: the frame times 0x1000ee61a's factors)
    double picture_w = logical_w / layout->scale, picture_h = logical_h / layout->scale;
    int w = aspyr_round(frame_w * (window_w / picture_w)), h = aspyr_round(frame_h * (window_h / picture_h));
    if (w > Max_Drawable || h > Max_Drawable)
        return false;
    // the picture's corner in those pixels, within half a pixel; present skips the copy ("bad size") when the frame
    // doesn't fit, which the rounding can make it miss by a pixel
    int x = aspyr_round(layout->pillar * ((double)w / window_w)), y = aspyr_round(layout->top * ((double)h / window_h));
    if (x > w - frame_w)
        x = w - frame_w;
    if (y > h - frame_h)
        y = h - frame_h;
    *drawable_w = w;
    *drawable_h = h;
    *blit = (NotchRect){ x < 0 ? 0 : x, y < 0 ? 0 : y, frame_w, frame_h };
    return true;
}

bool notch_scale_rect(int forced_w, int forced_h, int drawable_w, int drawable_h, NotchRect blit, NotchRect *out) {
    if (out == NULL || forced_w <= 0 || forced_h <= 0 || drawable_w <= 0 || drawable_h <= 0 || blit.w <= 0 ||
        blit.h <= 0 || blit.x < 0 || blit.y < 0 || blit.x + blit.w > forced_w || blit.y + blit.h > forced_h)
        return false;
    double scale_x = (double)drawable_w / forced_w, scale_y = (double)drawable_h / forced_h;
    // both edges scaled, so the far one stays inside the drawable
    int x = aspyr_round(blit.x * scale_x), y = aspyr_round(blit.y * scale_y);
    int right = aspyr_round((blit.x + blit.w) * scale_x), bottom = aspyr_round((blit.y + blit.h) * scale_y);
    *out = (NotchRect){ x, y, right - x, bottom - y };
    return out->w > 0 && out->h > 0;
}
