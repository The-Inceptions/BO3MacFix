// SharpFonts: menu and HUD text from glyphs twice as detailed, as All-around Enhancement 4's Windows plugin draws it
// (its lui_font_resolution component).
//
// The game draws each TrueType glyph once into a 2048x2048 atlas, at the height the text has in the 720-line layout
// its menus are made in, and stretches that picture to the screen: at 1440 lines every pixel of the glyph covers two
// by two pixels of the screen, and the text looks soft. With this on, a glyph gets a rectangle twice as wide and high
// in the atlas and is drawn there from its outline scaled by two. Only its texture coordinates change with that: its
// size, spacing and kerning on screen stay what they were.
//
// The game's glyph function (FontCache_GetGlyph) finds a cached glyph or makes it: it loads the outline through
// FreeType, takes a rectangle 2 pixels bigger on every side from the atlas' allocator, fills in the glyph's metrics and
// texture coordinates, and has FreeType draw the outline through a callback that writes the atlas. The plugin replaces
// two calls inside that function, the allocator's and FT_Outline_Render's. Here those two functions are hooked
// instead, and act only on the calls from those two places (by their return addresses). A hook on the glyph function
// tells them which glyph is in work and writes the glyph's new texture coordinates when it's made.
//
// Glyphs 64 pixels and higher and glyphs with an effect are distance fields, which stretch well as they are: they
// stay the game's, as with the plugin. The plugin's switch works here too once something sets it: the dvar
// aae_lui_font_enabled, 0 for off and anything else for on. A change starts the atlas over, so every glyph is drawn
// again.
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dobby.h"
#include "font_resolution.h"
#include "gsc_obj.h"
#include "offsets.h"
#include "prepatch.h"

#define Atlas_Size 2048   // the atlas' width and height, one byte per pixel
#define Atlas_Border 2    // the empty pixels the game keeps around every glyph
#define Sdf_Height 64     // from this height on, a glyph is a distance field
#define Font_CritSec 0x65 // the game's lock around the glyph cache
#define Option_Every 64   // glyph lookups from one look at aae_lui_font_enabled to the next

// FreeType's types as the Mac executable has them: FT_Pos is a 64-bit long here (32 bits on Windows)
typedef struct {
    int64_t x, y;
} ft_vector_t;
typedef struct {
    int64_t x_min, y_min, x_max, y_max;
} ft_bbox_t;
typedef struct {
    int16_t n_contours, n_points;
    ft_vector_t *points; // in 1/64 pixels
    char *tags;
    int16_t *contours;
    int32_t flags;
} ft_outline_t;
typedef struct {
    int16_t x;
    uint16_t len;
    uint8_t coverage;
} ft_span_t;
typedef void (*ft_spans_fn)(int y, int count, const ft_span_t *spans, void *user);
typedef struct {
    const void *target, *source;
    int32_t flags;
    ft_spans_fn gray_spans;
    void *black_spans, *bit_test, *bit_set;
    void *user;
    ft_bbox_t clip_box; // in pixels
} ft_raster_params_t;
#define Ft_Raster_Flags 7 // FT_RASTER_FLAG_AA, _DIRECT and _CLIP: what the game asks for

// what the game hands its own span callback: where the glyph goes in the atlas
typedef struct {
    int32_t *position; // x and y of the glyph's top left pixel in the atlas
    uint64_t unused;
    uint8_t *bitmap;
    int32_t pitch, left, top, right, bottom, offset; // the glyph's box in the outline's pixels
} glyph_target_t;

// a cached glyph
typedef struct {
    void *face;
    float uv[4]; // its rectangle in the atlas, borders included: left, top, right, bottom, each 0 to 1
    uint16_t next, character;
    uint32_t glyph_index;
    uint16_t height;
    uint8_t effect, pad;
    int16_t left, top, width, rows, advance, unused; // on screen, in the layout's pixels
} font_glyph_t;

_Static_assert(sizeof(ft_outline_t) == 0x28, "FT_Outline");
_Static_assert(sizeof(ft_span_t) == 6, "FT_Span");
_Static_assert(sizeof(ft_raster_params_t) == 0x60 && offsetof(ft_raster_params_t, user) == 0x38, "FT_Raster_Params");
_Static_assert(sizeof(glyph_target_t) == 0x30 && offsetof(glyph_target_t, pitch) == 0x18, "the span callback's user");
_Static_assert(sizeof(font_glyph_t) == 0x30 && offsetof(font_glyph_t, height) == 0x20, "the cached glyph");

// the glyph the game is making on this thread
typedef struct {
    bool sharp;              // one to draw twice as detailed
    bool allocated;          // it has its rectangle, twice as wide and high as the game asked for
    bool drawn;              // and is drawn into it
    int x, y, width, height; // that rectangle
} glyph_job_t;
static _Thread_local glyph_job_t *job;

static uint64_t base_address;
static uint8_t *atlas;
static bool sharp = true; // aae_lui_font_enabled's last value; this and the next three only under the font lock
static bool any_glyph;    // the cache may hold glyphs
static uint32_t lookups;
static bool said;

static void *(*Dvar_FindByHash)(uint32_t hash);
static int (*Dvar_GetInt)(void *dvar); // any type: a string dvar (one a "set" made) reads as atoi of its text
static void (*Font_Lock)(void *lock, int critical_section, int mode);
static void (*Font_Unlock)(void *lock);
static void (*Font_Reset)(void);
static void (*Font_Upload)(int left, int top, int right, int bottom);

static bool fits_int(int64_t v) {
    return v >= INT_MIN && v <= INT_MAX;
}

static void clear_rectangle(const glyph_job_t *j) {
    for (int row = 0; row < j->height; row++)
        memset(atlas + (size_t)(j->y + row) * Atlas_Size + j->x, 0, (size_t)j->width);
}

// FreeType's callback for one row of the doubled glyph: y counts up from the rectangle's bottom row. FreeType clips
// to the box it's given, this keeps every write inside the rectangle whatever it's handed.
static void write_spans(int y, int count, const ft_span_t *spans, void *user) {
    const glyph_job_t *j = user;
    if (y < 0 || y >= j->height)
        return;
    uint8_t *row = atlas + (size_t)(j->y + j->height - 1 - y) * Atlas_Size + j->x;
    for (int i = 0; i < count; i++) {
        int left = spans[i].x, right = left + spans[i].len;
        if (left < 0)
            left = 0;
        if (right > j->width)
            right = j->width;
        if (right > left)
            memset(row + left, spans[i].coverage, (size_t)(right - left));
    }
}

// the hooked functions themselves, for the code before their hooks
static int (*orig_FT_Outline_Render)(void *library, ft_outline_t *outline, ft_raster_params_t *params);
static bool (*orig_FontCache_Allocate)(void *packer, int width, int height, bool rotate, int32_t *position);

// Draws the glyph's outline twice as big into its rectangle and returns true, with FreeType's result in *result.
// False when the call isn't the one the game's glyph function is known to make, or when FreeType fails: the game then
// draws the glyph its own way, into the top left quarter of the rectangle, which its own texture coordinates name.
static bool draw_sharp(glyph_job_t *j, void *library, const ft_outline_t *outline, const ft_raster_params_t *params,
                       int *result) {
    if (!j->allocated || outline == NULL || params == NULL || params->flags != Ft_Raster_Flags ||
        params->user == NULL || outline->n_points < 0 || (outline->n_points > 0 && outline->points == NULL))
        return false;
    const glyph_target_t *target = params->user;
    const ft_bbox_t *box = &params->clip_box;
    // the game draws behind the border of the rectangle it asked for, into a box as big as the rest of it
    if (target->position == NULL || target->bitmap != atlas || target->pitch != Atlas_Size ||
        target->position[0] != j->x + Atlas_Border || target->position[1] != j->y + Atlas_Border ||
        !fits_int(box->x_min) || !fits_int(box->y_min) || !fits_int(box->x_max) || !fits_int(box->y_max) ||
        box->x_max - box->x_min != j->width / 2 - 2 * Atlas_Border ||
        box->y_max - box->y_min != j->height / 2 - 2 * Atlas_Border)
        return false;

    // the outline, moved so that its box starts behind the border, then doubled. The game's own stays as it is: the
    // glyph's metrics are read from it afterwards.
    ft_vector_t *points = NULL;
    if (outline->n_points > 0) {
        points = malloc((size_t)outline->n_points * sizeof(*points));
        if (points == NULL)
            return false;
    }
    int64_t move_x = (Atlas_Border - box->x_min) * 64, move_y = (Atlas_Border - box->y_min) * 64;
    for (int i = 0; i < outline->n_points; i++) {
        int64_t x = outline->points[i].x, y = outline->points[i].y;
        if (!fits_int(x) || !fits_int(y)) {
            free(points);
            return false;
        }
        points[i] = (ft_vector_t){ (x + move_x) * 2, (y + move_y) * 2 };
    }
    ft_outline_t doubled_outline = *outline;
    doubled_outline.points = points;
    ft_raster_params_t doubled_params = *params;
    doubled_params.gray_spans = write_spans;
    doubled_params.user = j;
    doubled_params.clip_box =
        (ft_bbox_t){ 2 * Atlas_Border, 2 * Atlas_Border, j->width - 2 * Atlas_Border, j->height - 2 * Atlas_Border };

    clear_rectangle(j);
    *result = orig_FT_Outline_Render(library, &doubled_outline, &doubled_params);
    free(points);
    if (*result != 0) {
        clear_rectangle(j); // it may have drawn rows before it failed
        return false;
    }
    j->drawn = true;
    Font_Upload(j->x, j->y, j->x + j->width, j->y + j->height);
    if (!said) {
        said = true;
        printf("fonts: glyphs under %d pixels are drawn twice as detailed (SharpFonts)\n", Sdf_Height);
    }
    return true;
}

// the glyph function's request for a rectangle of width x height (the glyph and its border): twice that for a glyph
// to draw twice as detailed
static bool allocate_for_glyph(glyph_job_t *j, void *packer, int width, int height, bool rotate, int32_t *position) {
    if (j == NULL || !j->sharp || j->allocated || rotate || position == NULL || width < 2 * Atlas_Border ||
        height < 2 * Atlas_Border || width > Atlas_Size / 2 || height > Atlas_Size / 2)
        return orig_FontCache_Allocate(packer, width, height, rotate, position);
    if (!orig_FontCache_Allocate(packer, width * 2, height * 2, rotate, position))
        return false; // no room: the game starts the atlas over and comes back for this glyph
    j->allocated = true;
    j->x = position[0];
    j->y = position[1];
    j->width = width * 2;
    j->height = height * 2;
    return true;
}

// FontCache_Allocate(packer, width, height, rotate, position): finds a free rectangle of the atlas and writes its
// corner to position
install_hook_name(FontCache_Allocate, bool, void *packer, int width, int height, bool rotate, int32_t *position) {
    if (__builtin_return_address(0) == (void *)(base_address + ADDR_FontCache_AllocateReturn))
        return allocate_for_glyph(job, packer, width, height, rotate, position);
    return orig_FontCache_Allocate(packer, width, height, rotate, position);
}

// FT_Outline_Render(library, outline, params): FreeType draws an outline, here through params' callback
install_hook_name(FT_Outline_Render, int, void *library, ft_outline_t *outline, ft_raster_params_t *params) {
    int result;
    if (job != NULL && __builtin_return_address(0) == (void *)(base_address + ADDR_FontCache_RenderReturn) &&
        draw_sharp(job, library, outline, params, &result))
        return result;
    return orig_FT_Outline_Render(library, outline, params);
}

// the plugin's switch. Nothing registers the dvar on the Mac, so it only exists once a "set" made it.
static void read_option(void) {
    void *option = Dvar_FindByHash(gsc_hash("aae_lui_font_enabled"));
    bool wanted = option == NULL || Dvar_GetInt(option) != 0;
    if (wanted == sharp)
        return;
    sharp = wanted;
    if (any_glyph)
        Font_Reset(); // empties the cache and the atlas, as the game does when either is full
    printf("fonts: aae_lui_font_enabled is %d, glyphs are drawn %s from now on\n", wanted,
           wanted ? "twice as detailed" : "as the game draws them");
}

// FontCache_GetGlyph(face, height, character, effect): the cached glyph, made first if it's new
install_hook_name(FontCache_GetGlyph, font_glyph_t *, void *face, uint32_t height, uint32_t character,
                  uint32_t effect) {
    // The game takes this lock inside as well (it counts). Held from here, no other thread finds the glyph in the
    // cache before its texture coordinates are the final ones.
    uint64_t lock[2] = { 0 };
    Font_Lock(lock, Font_CritSec, 0);
    if (job == NULL && lookups++ % Option_Every == 0)
        read_option();
    // the game calls itself: for a distance field of another height than 64, and after it started a full cache or
    // atlas over
    glyph_job_t this_job = { .sharp = sharp && height < Sdf_Height && effect == 0 }, *outer = job;
    job = &this_job;
    font_glyph_t *glyph = orig_FontCache_GetGlyph(face, height, character, effect);
    job = outer;
    if (glyph != NULL && this_job.drawn) {
        glyph->uv[0] = (float)this_job.x / Atlas_Size;
        glyph->uv[1] = (float)this_job.y / Atlas_Size;
        glyph->uv[2] = (float)(this_job.x + this_job.width) / Atlas_Size;
        glyph->uv[3] = (float)(this_job.y + this_job.height) / Atlas_Size;
    }
    any_glyph = true;
    Font_Unlock(lock);
    return glyph;
}

// whether the 5 bytes before return_address are a call of target
static bool calls(uint32_t return_address, uint32_t target) {
    uint8_t expected[5] = { 0xE8 };
    int32_t distance = (int32_t)(target - return_address);
    memcpy(expected + 1, &distance, sizeof(distance));
    const void *code = prepatch_original_code((void *)(base_address + return_address - sizeof(expected)), sizeof(expected));
    return memcmp(code, expected, sizeof(expected)) == 0;
}

void font_resolution_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    // the two calls inside the glyph function that the hooks know by their return addresses
    if (!calls(ADDR_FontCache_AllocateReturn, ADDR_FontCache_Allocate) ||
        !calls(ADDR_FontCache_RenderReturn, ADDR_FT_Outline_Render)) {
        printf("fonts: the game's glyph function isn't the one SharpFonts knows, text stays as it is\n");
        return;
    }
    atlas = (uint8_t *)(base_address + ADDR_FontCache_Bitmap);
    Dvar_FindByHash = (void *)(base_address + ADDR_Dvar_FindByHash);
    Dvar_GetInt = (void *)(base_address + ADDR_Dvar_GetInt);
    Font_Lock = (void *)(base_address + ADDR_CritSec_ScopedLock);
    Font_Unlock = (void *)(base_address + ADDR_CritSec_ScopedUnlock);
    Font_Reset = (void *)(base_address + ADDR_FontCache_Reset);
    Font_Upload = (void *)(base_address + ADDR_FontCache_Upload);
    // the glyph function's last: the other two do nothing until it names a glyph
    install_hook_FontCache_Allocate((void *)(base_address + ADDR_FontCache_Allocate));
    install_hook_FT_Outline_Render((void *)(base_address + ADDR_FT_Outline_Render));
    install_hook_FontCache_GetGlyph((void *)(base_address + ADDR_FontCache_GetGlyph));
}
