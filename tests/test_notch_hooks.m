// Tests notch.m's two hooks against a made-up ASL window, with stand-ins for Aspyr's letterbox, client rect, Metal view
// and SDL window size: the picture goes below the housing once there's a Metal view, including when ASL makes the view
// after the letterbox (the swap chain does only the client rect then), and nothing is asked for again every frame.
#include "../source/notch.m"

uint64_t game_base_address;

// what notch.m links against; the hooks are called directly here
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    return -1;
}
bool prepatch_has_hook(uint32_t offset) {
    return false;
}

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            failures++;                                                 \
        }                                                               \
    } while (0)

// a MacBook Pro 16's built-in display at its default size, 32 points of housing, and the game's 2624x1696 buffer
#define Window_W 1728
#define Window_H 1117
_Alignas(8) static char window[0x100];
static int sdl_window, metal_view;
static void *view; // NULL until the swap chain makes it
static int drawable_w, drawable_h, forced_w, forced_h;
static int aspyr_letterboxes, aspyr_client_rects;

static void *get_view(void *sdl) {
    return sdl == &sdl_window ? view : NULL;
}
static void get_window_size(void *sdl, int *w, int *h) {
    *w = Window_W;
    *h = Window_H;
}
// the layer: unforced, the view's size in points
static void force_drawable(void *v, int w, int h) {
    forced_w = w;
    forced_h = h;
    drawable_w = w > 0 ? w : Window_W;
    drawable_h = h > 0 ? h : Window_H;
}
static void get_drawable(void *v, int *w, int *h) {
    *w = drawable_w;
    *h = drawable_h;
}
// Aspyr's: the window has the logical size's aspect ratio, so no bars, and the frame fills the drawable
static void aspyr_letterbox(char *w) {
    aspyr_letterboxes++;
    *(double *)(w + Window_ScaleX) = *(double *)(w + Window_ScaleY) = 1.0;
    *(int *)(w + Window_Pillar) = *(int *)(w + Window_Letterbox) = 0;
}
static void aspyr_client_rect(char *w) {
    aspyr_client_rects++;
    memset(w + Window_Blit, 0, 16);
}

static bool ours(void) {
    return window_int(window, Window_Pillar) == 25 && window_int(window, Window_Letterbox) == 32;
}

int main(void) {
    SDL_GetWindowSize_ = get_window_size;
    Metal_GetView = get_view;
    Metal_GetDrawableSize = get_drawable;
    Metal_ForceDrawableSize = force_drawable;
    orig_UpdateLetterbox = aspyr_letterbox;
    orig_MetalClientRect = aspyr_client_rect;
    atomic_store(&option, 1);
    atomic_store(&screen_inset, 32);
    *(void **)(window + Window_SDL) = &sdl_window;
    *(int *)(window + Window_LogicalW) = Window_W;
    *(int *)(window + Window_LogicalH) = Window_H;
    *(int *)(window + Window_FrameW) = 2624;
    *(int *)(window + Window_FrameH) = 1696;
    window[Window_Flags] = 1;
    int w, h;

    // Window_SetMode before there's a swap chain: no view, so Aspyr's numbers, and nothing to do yet
    UpdateLetterbox_hook(window);
    MetalClientRect_hook(window);
    CHECK(!ours());
    CHECK(aspyr_client_rects == 1);
    CHECK(!notch_take_refresh());
    CHECK(!notch_forced_drawable(&w, &h));

    // the swap chain makes the view and does only the client rect (0x1000e89ab, 0x1000e89ef): that frame is Aspyr's,
    // and the engine thread is asked to do both again
    view = &metal_view;
    MetalClientRect_hook(window);
    CHECK(aspyr_client_rects == 2);
    CHECK(notch_take_refresh());
    CHECK(!notch_take_refresh());

    // engine_display.c's update_asl_window: now it's ours, and nothing more is asked for
    UpdateLetterbox_hook(window);
    MetalClientRect_hook(window);
    CHECK(ours());
    CHECK(aspyr_client_rects == 2);
    CHECK(window_int(window, Window_DrawableW) == 2701 && window_int(window, Window_DrawableH) == 1746);
    CHECK(window_int(window, Window_Blit) == 39 && window_int(window, Window_Blit + 4) == 50);
    CHECK(window_int(window, Window_Blit + 8) == 2624 && window_int(window, Window_Blit + 12) == 1696);
    CHECK(forced_w == 2701 && forced_h == 1746);
    CHECK(notch_forced_drawable(&w, &h) && w == 2701 && h == 1746);
    CHECK(!notch_take_refresh());

    // a client rect on its own (the buffer resized, say) keeps ours and asks for nothing
    MetalClientRect_hook(window);
    CHECK(ours() && aspyr_client_rects == 2);
    CHECK(!notch_take_refresh());

    // windowed: Aspyr's numbers, and a client rect with the view there doesn't ask again
    window[Window_Flags] = 0;
    UpdateLetterbox_hook(window);
    MetalClientRect_hook(window);
    CHECK(!ours() && aspyr_client_rects == 3);
    CHECK(!notch_take_refresh());
    CHECK(!notch_forced_drawable(&w, &h));

    // fullscreen again while the view is being made again: it's ours once the new view is there
    window[Window_Flags] = 1;
    view = NULL;
    UpdateLetterbox_hook(window);
    MetalClientRect_hook(window);
    CHECK(!notch_take_refresh());
    view = &metal_view;
    MetalClientRect_hook(window);
    CHECK(notch_take_refresh());
    UpdateLetterbox_hook(window);
    MetalClientRect_hook(window);
    CHECK(ours());
    CHECK(!notch_take_refresh());

    // AvoidNotch turned off before the view came: nothing is asked for
    view = NULL;
    UpdateLetterbox_hook(window);
    atomic_store(&option, 0);
    UpdateLetterbox_hook(window);
    view = &metal_view;
    MetalClientRect_hook(window);
    CHECK(!ours());
    CHECK(!notch_take_refresh());

    if (failures == 0)
        printf("test_notch_hooks: all passed\n");
    return failures == 0 ? 0 : 1;
}
