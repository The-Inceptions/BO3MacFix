// font_resolution.c against a made-up game: an atlas, an allocator that hands out rectangles, a FreeType that fills an
// outline's bounding box through the span callback, and a glyph function that makes a glyph in the game's order with
// the game's values (the test includes the source itself). The two hooks that know their callers by return address
// can't be called from here; the glyph function calls what they call.
#include <stdio.h>
#include <stdlib.h>
#include "../source/font_resolution.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)address, (void)fake_func, (void)out_origin_func;
    return 0;
}
const void *prepatch_original_code(const void *address, size_t size) {
    (void)size;
    return address;
}

#define Untouched 0x5A // what the test's atlas holds where nothing wrote
#define Ink 200
#define Top_Ink 100 // the glyph's top row, so that a picture upside down shows

// ---- the made-up game
static int lock_depth, resets, finds;
static int *option; // aae_lui_font_enabled, NULL while nothing has set it
static struct {
    int left, top, right, bottom;
} uploads[8];
static int upload_count;
static struct {
    int width, height;
} asked[8];
static int ask_count, fail_asks, cursor_x, cursor_y;
static int render_fails;    // FreeType draws, then reports this error
static int renders;         // times FreeType drew
static ft_bbox_t drawn_box; // the last outline's bounding box, in 1/64 pixels
static int wrong_pitch;     // the glyph function hands its span callback a pitch SharpFonts doesn't know
// the next new glyph's box in the outline's pixels, y up
static struct {
    int left, bottom, right, top;
} shape = { -3, -2, 7, 5 };
static font_glyph_t glyphs[8];
static int glyph_count;

static void game_lock(void *lock, int critical_section, int mode) {
    (void)lock;
    CHECK(critical_section == Font_CritSec && mode == 0);
    lock_depth++;
}
static void game_unlock(void *lock) {
    (void)lock;
    CHECK(lock_depth > 0);
    lock_depth--;
}
static void game_reset(void) {
    CHECK(lock_depth > 0);
    resets++;
    memset(atlas, Untouched, (size_t)Atlas_Size * Atlas_Size);
    cursor_x = cursor_y = 0;
    glyph_count = 0;
}
static void game_upload(int left, int top, int right, int bottom) {
    if (upload_count < 8) {
        uploads[upload_count].left = left, uploads[upload_count].top = top;
        uploads[upload_count].right = right, uploads[upload_count].bottom = bottom;
    }
    upload_count++;
}
static void *game_find_dvar(uint32_t hash) {
    CHECK(hash == gsc_hash("aae_lui_font_enabled"));
    finds++;
    return option;
}
static int game_dvar_int(void *dvar) {
    return *(int *)dvar;
}

// rectangles side by side, a new row when told to
static bool game_allocate(void *packer, int width, int height, bool rotate, int32_t *position) {
    (void)packer, (void)rotate;
    if (ask_count < 8)
        asked[ask_count].width = width, asked[ask_count].height = height;
    ask_count++;
    if (fail_asks > 0) {
        fail_asks--;
        return false;
    }
    if (cursor_x + width > Atlas_Size)
        return false;
    position[0] = cursor_x, position[1] = cursor_y, position[2] = 0;
    cursor_x += width;
    return true;
}

// the game's span callback: row top - 1 - y of the glyph, from its position in the atlas
static void game_spans(int y, int count, const ft_span_t *spans, void *user) {
    const glyph_target_t *t = user;
    uint8_t *row = t->bitmap + (size_t)(t->top - 1 - y + t->position[1]) * t->pitch + t->position[0];
    for (int i = 0; i < count; i++)
        memset(row + spans[i].x - t->left + t->offset, spans[i].coverage, spans[i].len);
}

// FreeType: the outline's bounding box, clipped, one span a row, the top row fainter
static int game_render(void *library, ft_outline_t *outline, ft_raster_params_t *params) {
    CHECK(library == (void *)17 && params->flags == Ft_Raster_Flags);
    renders++;
    ft_bbox_t box = { INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN };
    for (int i = 0; i < outline->n_points; i++) {
        const ft_vector_t *p = &outline->points[i];
        box.x_min = p->x < box.x_min ? p->x : box.x_min, box.x_max = p->x > box.x_max ? p->x : box.x_max;
        box.y_min = p->y < box.y_min ? p->y : box.y_min, box.y_max = p->y > box.y_max ? p->y : box.y_max;
    }
    drawn_box = box;
    const ft_bbox_t *clip = &params->clip_box;
    int64_t left = box.x_min / 64, right = box.x_max / 64, bottom = box.y_min / 64, top = box.y_max / 64;
    left = left < clip->x_min ? clip->x_min : left, right = right > clip->x_max ? clip->x_max : right;
    bottom = bottom < clip->y_min ? clip->y_min : bottom, top = top > clip->y_max ? clip->y_max : top;
    for (int64_t y = bottom; y < top; y++) {
        ft_span_t span = { (int16_t)left, (uint16_t)(right - left), y == top - 1 ? Top_Ink : Ink };
        params->gray_spans((int)y, 1, &span, params->user);
    }
    return render_fails;
}

// the game's glyph function, for a glyph that isn't cached
static font_glyph_t *game_get_glyph(void *face, uint32_t height, uint32_t character, uint32_t effect) {
    CHECK(lock_depth > 0 && job != NULL);
    bool field = height >= Sdf_Height || effect != 0;
    CHECK(job->sharp == (sharp && !field)); // the game's own rule for a distance field
    font_glyph_t *glyph = &glyphs[glyph_count++ % 8];
    if (field && height != Sdf_Height) {
        // a distance field of another height: the 64 pixel one's picture, with its own metrics
        glyph_job_t *mine = job;
        font_glyph_t *big = fake_FontCache_GetGlyph(face, Sdf_Height, character, effect);
        CHECK(job == mine);
        *glyph = *big;
        glyph->height = (uint16_t)height;
        return glyph;
    }
    int width = shape.right - shape.left, rows = shape.top - shape.bottom;
    int32_t position[3];
    if (!allocate_for_glyph(job, (void *)23, width + 4, rows + 4, false, position)) {
        game_reset();
        return fake_FontCache_GetGlyph(face, height, character, effect);
    }
    position[0] += 2, position[1] += 2;
    *glyph = (font_glyph_t){ .face = face, .character = (uint16_t)character, .glyph_index = character + 1000,
                             .height = (uint16_t)height, .effect = (uint8_t)effect,
                             .left = (int16_t)(shape.left - 2), .top = (int16_t)(30 - shape.top),
                             .width = (int16_t)(width + 4), .rows = (int16_t)(rows + 4), .advance = (int16_t)(width + 1) };
    glyph->uv[0] = (float)(position[0] - 2) / Atlas_Size, glyph->uv[1] = (float)(position[1] - 2) / Atlas_Size;
    glyph->uv[2] = (float)(position[0] + width + 2) / Atlas_Size, glyph->uv[3] = (float)(position[1] + rows + 2) / Atlas_Size;

    ft_vector_t points[4] = { { shape.left * 64, shape.bottom * 64 }, { shape.right * 64, shape.bottom * 64 },
                              { shape.right * 64, shape.top * 64 }, { shape.left * 64, shape.top * 64 } };
    ft_vector_t points_before[4];
    memcpy(points_before, points, sizeof(points));
    ft_outline_t outline = { .n_contours = 1, .n_points = 4, .points = points };
    glyph_target_t target = { .position = position, .bitmap = atlas, .pitch = wrong_pitch ? 512 : Atlas_Size,
                              .left = shape.left, .top = shape.top, .right = shape.right, .bottom = shape.bottom };
    ft_raster_params_t params = { .flags = Ft_Raster_Flags, .gray_spans = game_spans, .user = &target,
                                  .clip_box = { shape.left, shape.bottom, shape.right, shape.top } };
    ft_raster_params_t params_before = params;
    int result;
    if (!draw_sharp(job, (void *)17, &outline, &params, &result)) {
        target.pitch = Atlas_Size;
        params_before = params;
        int fails = render_fails;
        render_fails = 0; // the game's own drawing works
        result = game_render((void *)17, &outline, &params);
        render_fails = fails;
    }
    CHECK(result == 0);
    CHECK(memcmp(points, points_before, sizeof(points)) == 0 && outline.points == points);
    CHECK(memcmp(&params, &params_before, sizeof(params)) == 0);
    game_upload(position[0], position[1], position[0] + width, position[1] + rows);
    return glyph;
}

static void new_game(void) {
    free(atlas);
    atlas = malloc((size_t)Atlas_Size * Atlas_Size);
    memset(atlas, Untouched, (size_t)Atlas_Size * Atlas_Size);
    orig_FontCache_GetGlyph = game_get_glyph;
    orig_FontCache_Allocate = game_allocate;
    orig_FT_Outline_Render = game_render;
    Font_Lock = game_lock, Font_Unlock = game_unlock, Font_Reset = game_reset, Font_Upload = game_upload;
    Dvar_FindByHash = game_find_dvar, Dvar_GetInt = game_dvar_int;
    job = NULL;
    sharp = true, any_glyph = false, lookups = 0;
    lock_depth = resets = finds = upload_count = ask_count = fail_asks = cursor_x = cursor_y = 0;
    render_fails = renders = wrong_pitch = glyph_count = 0;
    option = NULL;
    shape.left = -3, shape.bottom = -2, shape.right = 7, shape.top = 5;
}

// every byte of the atlas: ink inside the box (its top row fainter), nothing in the rest of the rectangle,
// untouched around it
static bool atlas_is(int x, int y, int width, int height, int ink_x, int ink_y, int ink_width, int ink_height) {
    for (int row = 0; row < Atlas_Size; row++) {
        for (int column = 0; column < Atlas_Size; column++) {
            uint8_t expected = Untouched;
            if (column >= ink_x && column < ink_x + ink_width && row >= ink_y && row < ink_y + ink_height)
                expected = row == ink_y ? Top_Ink : Ink;
            else if (column >= x && column < x + width && row >= y && row < y + height)
                expected = 0;
            if (atlas[(size_t)row * Atlas_Size + column] != expected) {
                printf("atlas[%d][%d] is %d, expected %d\n", row, column, atlas[(size_t)row * Atlas_Size + column], expected);
                return false;
            }
        }
    }
    return true;
}

static void test_sharp_glyph(void) {
    new_game();
    // the game's own glyph first, for its metrics
    int off = 0;
    option = &off;
    font_glyph_t stock = *fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(ask_count == 1 && asked[0].width == 14 && asked[0].height == 11);
    CHECK(stock.uv[0] == 0 && stock.uv[1] == 0 && stock.uv[2] == 14.0f / Atlas_Size && stock.uv[3] == 11.0f / Atlas_Size);
    CHECK(upload_count == 1 && uploads[0].left == 2 && uploads[0].right == 12 && uploads[0].top == 2 && uploads[0].bottom == 9);
    // the game writes no border, a fresh atlas has it
    CHECK(atlas_is(2, 2, 10, 7, 2, 2, 10, 7));

    new_game();
    cursor_x = 100, cursor_y = 40;
    font_glyph_t *glyph = fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(lock_depth == 0 && job == NULL && renders == 1);
    // twice the rectangle, the outline moved behind the border and doubled, drawn once
    CHECK(ask_count == 1 && asked[0].width == 28 && asked[0].height == 22);
    CHECK(drawn_box.x_min == 4 * 64 && drawn_box.y_min == 4 * 64 && drawn_box.x_max == 24 * 64 && drawn_box.y_max == 18 * 64);
    CHECK(atlas_is(100, 40, 28, 22, 104, 44, 20, 14));
    // its place in the atlas is the whole rectangle, everything else is the game's
    CHECK(glyph->uv[0] == 100.0f / Atlas_Size && glyph->uv[1] == 40.0f / Atlas_Size);
    CHECK(glyph->uv[2] == 128.0f / Atlas_Size && glyph->uv[3] == 62.0f / Atlas_Size);
    font_glyph_t same = *glyph;
    memcpy(same.uv, stock.uv, sizeof(same.uv));
    CHECK(memcmp(&same, &stock, sizeof(same)) == 0);
    // the whole rectangle goes to the GPU (and then the game's part of it again)
    CHECK(upload_count == 2 && uploads[0].left == 100 && uploads[0].top == 40 && uploads[0].right == 128 && uploads[0].bottom == 62);
}

static void test_glyphs_left_alone(void) {
    // distance fields: 64 pixels and more, or an effect
    new_game();
    font_glyph_t *glyph = fake_FontCache_GetGlyph((void *)5, 64, 'A', 0);
    CHECK(ask_count == 1 && asked[0].width == 14 && asked[0].height == 11 && glyph->uv[2] == 14.0f / Atlas_Size);
    glyph = fake_FontCache_GetGlyph((void *)5, 20, 'A', 3);
    CHECK(ask_count == 2 && asked[1].width == 14 && glyph->height == 20 && lock_depth == 0 && job == NULL);
    // another height: the game calls itself for the 64 pixel one
    new_game();
    glyph = fake_FontCache_GetGlyph((void *)5, 80, 'A', 0);
    CHECK(ask_count == 1 && asked[0].width == 14 && glyph->height == 80 && lock_depth == 0 && job == NULL);
    CHECK(atlas_is(2, 2, 10, 7, 2, 2, 10, 7));

    // a glyph whose doubled rectangle could never fit
    new_game();
    shape.right = shape.left + 1100;
    glyph = fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(ask_count == 1 && asked[0].width == 1104 && asked[0].height == 11 && glyph->uv[2] == 1104.0f / Atlas_Size);
    CHECK(atlas_is(2, 2, 1100, 7, 2, 2, 1100, 7));
}

static void test_full_atlas(void) {
    // no room for the doubled rectangle: the game starts the atlas over and comes back, as it does for its own
    new_game();
    cursor_x = 300;
    fail_asks = 1;
    font_glyph_t *glyph = fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(resets == 1 && ask_count == 2 && asked[0].width == 28 && asked[1].width == 28);
    CHECK(glyph->uv[0] == 0 && glyph->uv[2] == 28.0f / Atlas_Size && glyph->uv[3] == 22.0f / Atlas_Size);
    CHECK(atlas_is(0, 0, 28, 22, 4, 4, 20, 14));
    CHECK(lock_depth == 0 && job == NULL);
}

static void test_fallbacks(void) {
    // a call SharpFonts doesn't know: the game's own drawing, in the corner of the bigger rectangle its own texture
    // coordinates name. The rest of the rectangle stays as the atlas was.
    new_game();
    wrong_pitch = 1;
    font_glyph_t *glyph = fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(asked[0].width == 28 && renders == 1);
    CHECK(glyph->uv[0] == 0 && glyph->uv[2] == 14.0f / Atlas_Size && glyph->uv[3] == 11.0f / Atlas_Size);
    CHECK(atlas_is(2, 2, 10, 7, 2, 2, 10, 7));
    CHECK(upload_count == 1);

    // FreeType fails on the doubled outline after it drew rows: those are gone again
    new_game();
    render_fails = 9;
    glyph = fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(renders == 2 && glyph->uv[2] == 14.0f / Atlas_Size);
    CHECK(atlas_is(0, 0, 28, 22, 2, 2, 10, 7));

    // each thing draw_sharp looks at, on its own
    new_game();
    glyph_job_t j = { .sharp = true, .allocated = true, .x = 60, .y = 80, .width = 28, .height = 22 };
    int32_t position[3] = { 62, 82, 0 };
    ft_vector_t points[4] = { { -192, -128 }, { 448, -128 }, { 448, 320 }, { -192, 320 } };
    ft_outline_t outline = { .n_contours = 1, .n_points = 4, .points = points };
    glyph_target_t target = { .position = position, .bitmap = atlas, .pitch = Atlas_Size };
    ft_raster_params_t params = { .flags = Ft_Raster_Flags, .gray_spans = game_spans, .user = &target,
                                  .clip_box = { -3, -2, 7, 5 } };
    int result = -1;
#define Refused() (!draw_sharp(&j, (void *)17, &outline, &params, &result) && renders == 0 && !j.drawn && \
                   atlas_is(0, 0, 0, 0, 0, 0, 0, 0))
    j.allocated = false;
    CHECK(Refused());
    j.allocated = true;
    params.flags = 3;
    CHECK(Refused());
    params.flags = Ft_Raster_Flags;
    params.user = NULL;
    CHECK(Refused());
    params.user = &target;
    target.bitmap = atlas + 1;
    CHECK(Refused());
    target.bitmap = atlas;
    position[0] = 60;
    CHECK(Refused());
    position[0] = 62;
    position[1] = 83;
    CHECK(Refused());
    position[1] = 82;
    params.clip_box.x_max = 8;
    CHECK(Refused());
    params.clip_box.x_max = 7;
    params.clip_box.y_min = INT64_MIN;
    CHECK(Refused());
    params.clip_box.y_min = -2;
    points[2].x = INT64_MAX;
    CHECK(Refused());
    points[2].x = 448;
    outline.n_points = -4;
    CHECK(Refused());
    outline.n_points = 4;
    outline.points = NULL;
    CHECK(Refused());
    outline.points = points;
#undef Refused
    CHECK(draw_sharp(&j, (void *)17, &outline, &params, &result) && result == 0 && j.drawn);
    CHECK(atlas_is(60, 80, 28, 22, 64, 84, 20, 14));
    // an outline without points is an empty glyph, not an error
    new_game();
    j.drawn = false;
    outline.n_points = 0, outline.points = NULL;
    target.bitmap = atlas;
    CHECK(draw_sharp(&j, (void *)17, &outline, &params, &result) && j.drawn && atlas_is(60, 80, 28, 22, 0, 0, 0, 0));
}

static void test_spans_stay_inside(void) {
    new_game();
    glyph_job_t j = { .sharp = true, .allocated = true, .x = Atlas_Size - 28, .y = Atlas_Size - 22, .width = 28, .height = 22 };
    ft_span_t wild[] = { { -300, 400, Top_Ink }, { 20, 65535, Top_Ink } };
    write_spans(-1, 2, wild, &j);
    write_spans(22, 2, wild, &j);
    CHECK(atlas_is(0, 0, 0, 0, 0, 0, 0, 0));
    write_spans(0, 2, wild, &j); // the bottom row, cut at both ends of the rectangle
    CHECK(atlas_is(0, 0, 0, 0, Atlas_Size - 28, Atlas_Size - 1, 28, 1));
    write_spans(21, 1, wild, &j); // the top row
    CHECK(atlas[(size_t)(Atlas_Size - 22) * Atlas_Size + Atlas_Size - 28] == Top_Ink);
    CHECK(atlas[(size_t)(Atlas_Size - 22) * Atlas_Size + Atlas_Size - 29] == Untouched);
}

static void test_option(void) {
    // nothing set the dvar: on, and it's looked for once in Option_Every lookups
    new_game();
    for (int i = 0; i < Option_Every; i++)
        fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(finds == 1 && resets == 0 && asked[0].width == 28);
    // set to 0 during play: the atlas starts over, the glyphs are the game's again
    int value = 0;
    option = &value;
    cursor_x = 0;
    ask_count = 0;
    fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(finds == 2 && resets == 1 && !sharp && asked[0].width == 14);
    for (int i = 1; i < 2 * Option_Every; i++)
        fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(finds == 3 && resets == 1);
    // and back, with any value but 0
    value = 2;
    ask_count = 0;
    fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(finds == 4 && resets == 2 && sharp && asked[0].width == 28);
    CHECK(lock_depth == 0 && job == NULL);

    // 0 from the start: nothing is cached yet, so nothing to start over
    new_game();
    option = &value;
    value = 0;
    fake_FontCache_GetGlyph((void *)5, 20, 'A', 0);
    CHECK(!sharp && resets == 0 && asked[0].width == 14);
}

static void test_calls(void) {
    static uint8_t code[64];
    base_address = (uint64_t)code;
    code[20] = 0xE8;
    int32_t distance = 0x1000 - 25;
    memcpy(code + 21, &distance, sizeof(distance));
    CHECK(calls(25, 0x1000));
    CHECK(!calls(25, 0x1001) && !calls(26, 0x1000));
    code[20] = 0xE9;
    CHECK(!calls(25, 0x1000));
    // backwards, as the glyph function's call of FT_Outline_Render is
    code[40] = 0xE8;
    distance = 4 - 45;
    memcpy(code + 41, &distance, sizeof(distance));
    CHECK(calls(45, 4));
}

int main(void) {
    test_sharp_glyph();
    test_glyphs_left_alone();
    test_full_atlas();
    test_fallbacks();
    test_spans_stay_inside();
    test_option();
    test_calls();
    free(atlas);
    if (failures != 0) {
        printf("test_font_resolution: %d failures\n", failures);
        return 1;
    }
    printf("test_font_resolution: all passed\n");
    return 0;
}
