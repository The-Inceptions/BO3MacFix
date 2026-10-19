// Tests for notch_layout.c, AvoidNotch's geometry: the picture below a camera housing with the right aspect ratio, the
// drawable and blit rect that put the frame there, MetalFX's scaled rect, and the mouse through Aspyr's own mapping.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../source/notch_layout.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// a MacBook Pro 16's built-in display at its default "looks like" size, and its camera housing
#define Built_W 1728
#define Built_H 1117
#define Housing 32

static int aspyr_round(double value) {
    return (int)(value + (value > 0 ? 0.5 : -0.5));
}

// Window_ConvertCoords (0x1000ee866) with the fields Window_UpdateLetterbox fills, points to logical and back
static void to_logical(const NotchLayout *l, int *x, int *y) {
    *x = aspyr_round((*x - l->pillar) * l->scale);
    *y = aspyr_round((*y - l->top) * l->scale);
}

static void to_points(const NotchLayout *l, int *x, int *y) {
    *x = aspyr_round(*x / l->scale + l->pillar);
    *y = aspyr_round(*y / l->scale + l->top);
}

static void test_built_in_display(void) {
    NotchLayout l;
    // fullscreen: the logical size is the desktop's, so below the housing it's narrower: bars at the sides
    CHECK(notch_layout(Built_W, Built_H, Built_W, Built_H, Housing, &l));
    CHECK(fabs(l.scale - (double)Built_H / (Built_H - Housing)) < 1e-12);
    CHECK(l.pillar == 25); // (1728 - 1678.47) / 2
    CHECK(l.top == Housing);
    // a 16:9 logical size is wider than what's below the housing: bars above and below it, under the housing
    CHECK(notch_layout(1920, 1080, Built_W, Built_H, Housing, &l));
    CHECK(fabs(l.scale - 1920.0 / Built_W) < 1e-12);
    CHECK(l.pillar == 0);
    CHECK(l.top == Housing + 57); // 972 points tall in the 1085 below the housing
}

static void test_nothing_to_do(void) {
    NotchLayout l;
    CHECK(!notch_layout(Built_W, Built_H, Built_W, Built_H, 0, &l));     // an external display: Aspyr's own letterbox
    CHECK(!notch_layout(Built_W, Built_H, Built_W, Built_H, -32, &l));
    CHECK(!notch_layout(Built_W, Built_H, Built_W, 100, 32, &l));        // not a camera housing
    CHECK(!notch_layout(0, Built_H, Built_W, Built_H, Housing, &l));
    CHECK(!notch_layout(Built_W, Built_H, 0, Built_H, Housing, &l));
    CHECK(!notch_layout(Built_W, Built_H, Built_W, Built_H, Housing, NULL));
    int w, h;
    NotchRect blit;
    NotchLayout bad = { 0, 0, Housing };
    CHECK(!notch_drawable(&bad, Built_W, Built_H, Built_W, Built_H, 2624, 1696, &w, &h, &blit));
    CHECK(notch_layout(Built_W, Built_H, Built_W, Built_H, Housing, &l));
    CHECK(!notch_drawable(&l, Built_W, Built_H, Built_W, Built_H, 0, 1696, &w, &h, &blit));
    CHECK(!notch_drawable(&l, Built_W, Built_H, Built_W, Built_H, 20000, 12000, &w, &h, &blit)); // past Metal's limit
    NotchRect out;
    CHECK(!notch_scale_rect(2701, 1746, 3456, 2234, (NotchRect){ 39, 51, 2624, 1696 }, &out)); // doesn't fit
    CHECK(!notch_scale_rect(0, 1746, 3456, 2234, (NotchRect){ 0, 0, 1, 1 }, &out));
}

// the menu cursor: the picture's corners and centre on the screen are the game's corners and centre
static void test_mouse(void) {
    NotchLayout l;
    CHECK(notch_layout(Built_W, Built_H, Built_W, Built_H, Housing, &l));
    int x = l.pillar, y = l.top;
    to_logical(&l, &x, &y);
    CHECK(x == 0 && y == 0);
    x = Built_W - l.pillar, y = Built_H;
    to_logical(&l, &x, &y);
    CHECK(abs(x - Built_W) <= 1 && y == Built_H);
    x = Built_W / 2, y = Housing + (Built_H - Housing) / 2;
    to_logical(&l, &x, &y);
    CHECK(abs(x - Built_W / 2) <= 1 && abs(y - Built_H / 2) <= 1);
    // the strip beside the housing is above the game's picture
    x = 10, y = 10;
    to_logical(&l, &x, &y);
    CHECK(y < 0 && x < 0);
    // and the game's own positions (where it warps the cursor) come back where they were
    for (int lx = 0; lx <= Built_W; lx += 97) {
        for (int ly = 0; ly <= Built_H; ly += 89) {
            x = lx, y = ly;
            to_points(&l, &x, &y);
            CHECK(x >= l.pillar && x <= Built_W - l.pillar + 1 && y >= Housing && y <= Built_H);
            to_logical(&l, &x, &y);
            CHECK(abs(x - lx) <= 1 && abs(y - ly) <= 1);
        }
    }
}

// the 2624x1696 the built-in display's settings use, with and without MetalFX
static void test_frame(void) {
    NotchLayout l;
    int w, h;
    NotchRect blit, scaled;
    CHECK(notch_layout(Built_W, Built_H, Built_W, Built_H, Housing, &l));
    CHECK(notch_drawable(&l, Built_W, Built_H, Built_W, Built_H, 2624, 1696, &w, &h, &blit));
    CHECK(w == 2701 && h == 1746);
    CHECK(blit.x == 39 && blit.y == 50 && blit.w == 2624 && blit.h == 1696);
    // Aspyr would centre it (0x1000ee6c6), 25 pixels down: 16 points, under the housing
    CHECK(aspyr_round((h - 1696) * 0.5) * (double)Built_H / h < Housing - 10);
    // MetalFX upscales into the view's pixels: the 32 points of the housing are its top 64 pixels, left black
    CHECK(notch_scale_rect(w, h, Built_W * 2, Built_H * 2, blit, &scaled));
    CHECK(scaled.x == 50 && scaled.y == 64 && scaled.w == 3357 && scaled.h == 2170);
    CHECK(scaled.y + scaled.h == Built_H * 2);
    // a drawable as big as forced is left as it is
    CHECK(notch_scale_rect(w, h, w, h, blit, &scaled));
    CHECK(scaled.x == blit.x && scaled.y == blit.y && scaled.w == blit.w && scaled.h == blit.h);
}

// Every built-in display size macOS offers, housings of 32 to 38 points, other aspect ratios and frame sizes: the
// picture keeps its aspect ratio, fills one side below the housing, the frame lands on it, and the mouse lines up.
static void test_sweep(void) {
    static const int windows[][2] = { { 1728, 1117 }, { 1512, 982 }, { 2056, 1329 }, { 1800, 1169 }, { 1496, 967 },
                                      { 1352, 878 }, { 1147, 745 }, { 1470, 956 }, { 1710, 1112 } };
    static const int insets[] = { 32, 33, 37, 38 };
    static const double aspects[] = { 0, 16.0 / 9, 16.0 / 10, 4.0 / 3, 21.0 / 9 }; // 0: the window's own
    static const double frames[] = { 0.6, 0.76, 1, 1.5, 2 };
    int cases = 0;
    for (size_t wi = 0; wi < sizeof(windows) / sizeof(windows[0]); wi++) {
        int W = windows[wi][0], H = windows[wi][1];
        for (size_t ii = 0; ii < sizeof(insets) / sizeof(insets[0]); ii++) {
            int inset = insets[ii];
            for (size_t ai = 0; ai < sizeof(aspects) / sizeof(aspects[0]); ai++) {
                int lw = W, lh = aspects[ai] == 0 ? H : (int)lround(W / aspects[ai]);
                NotchLayout l;
                CHECK(notch_layout(lw, lh, W, H, inset, &l));
                double pw = lw / l.scale, ph = lh / l.scale;
                // the aspect ratio is the game's, and one side fills what's below the housing
                CHECK(fabs(pw / ph - (double)lw / lh) < 1e-9);
                CHECK(fabs(pw - W) < 1e-6 || fabs(ph - (H - inset)) < 1e-6);
                CHECK(pw <= W + 1e-6 && ph <= H - inset + 1e-6);
                // centred below the housing, nothing under it
                CHECK(l.top >= inset && fabs(l.top + ph / 2 - (inset + (H - inset) / 2.0)) <= 0.5 + 1e-9);
                CHECK(fabs(l.pillar + pw / 2 - W / 2.0) <= 0.5 + 1e-9);
                int x = l.pillar, y = l.top;
                to_logical(&l, &x, &y);
                CHECK(x == 0 && y == 0);
                x = (int)lround(l.pillar + pw), y = (int)lround(l.top + ph);
                to_logical(&l, &x, &y);
                CHECK(abs(x - lw) <= 1 && abs(y - lh) <= 1);
                for (size_t fi = 0; fi < sizeof(frames) / sizeof(frames[0]); fi++) {
                    int fw = (int)lround(lw * frames[fi]), fh = (int)lround(lh * frames[fi]), dw, dh;
                    NotchRect blit, scaled;
                    CHECK(notch_drawable(&l, lw, lh, W, H, fw, fh, &dw, &dh, &blit));
                    // present's size check (0x1000e8b78): the frame fits in the drawable
                    CHECK(blit.x >= 0 && blit.y >= 0 && blit.x + blit.w <= dw && blit.y + blit.h <= dh);
                    // the layer stretches the drawable over the window: the frame lands on the picture, to within half
                    // a drawable pixel (sx and sy points), so at most that of it goes under the housing
                    double sx = (double)W / dw, sy = (double)H / dh, e = 1e-9;
                    CHECK(fabs(blit.x * sx - l.pillar) <= sx / 2 + e && fabs(blit.y * sy - l.top) <= sy / 2 + e);
                    CHECK(fabs(blit.w * sx - pw) <= sx / 2 + e && fabs(blit.h * sy - ph) <= sy / 2 + e);
                    CHECK(blit.y * sy >= inset - sy / 2 - e);
                    // and with MetalFX into the window's pixels at 2x, which rounds again by up to half a pixel
                    CHECK(notch_scale_rect(dw, dh, W * 2, H * 2, blit, &scaled));
                    CHECK(scaled.x + scaled.w <= W * 2 && scaled.y + scaled.h <= H * 2);
                    CHECK(scaled.y / 2.0 >= inset - sy / 2 - 0.25 - e);
                    CHECK(fabs(scaled.x / 2.0 - l.pillar) <= sx / 2 + 0.25 + e);
                    CHECK(fabs(scaled.y / 2.0 - l.top) <= sy / 2 + 0.25 + e);
                    CHECK(fabs(scaled.w / 2.0 - pw) <= sx / 2 + 0.5 + e && fabs(scaled.h / 2.0 - ph) <= sy / 2 + 0.5 + e);
                    cases++;
                }
            }
        }
    }
    CHECK(cases == 9 * 4 * 5 * 5);
}

// the game moving from the built-in display to an external one and back: on, off, then the same again
static void test_display_change(void) {
    NotchLayout first, again, none;
    int w1, h1, w2, h2;
    NotchRect b1, b2;
    CHECK(notch_layout(Built_W, Built_H, Built_W, Built_H, Housing, &first));
    CHECK(notch_drawable(&first, Built_W, Built_H, Built_W, Built_H, 2624, 1696, &w1, &h1, &b1));
    CHECK(!notch_layout(2560, 1440, 2560, 1440, 0, &none)); // the external has no housing: Aspyr's numbers stay
    CHECK(notch_layout(Built_W, Built_H, Built_W, Built_H, Housing, &again));
    CHECK(notch_drawable(&again, Built_W, Built_H, Built_W, Built_H, 2624, 1696, &w2, &h2, &b2));
    CHECK(again.scale == first.scale && again.pillar == first.pillar && again.top == first.top);
    CHECK(w1 == w2 && h1 == h2 && b1.x == b2.x && b1.y == b2.y);
}

int main(void) {
    test_built_in_display();
    test_nothing_to_do();
    test_mouse();
    test_frame();
    test_sweep();
    test_display_change();
    if (failures == 0)
        printf("test_notch: all passed\n");
    return failures == 0 ? 0 : 1;
}
