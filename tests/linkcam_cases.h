// Made-up cases for aae_linkcam.c: links to the camera, deletes, entities drawn and views worked out, in random
// order on a made-up game with two local clients and eight client entities, the same ones for the plugin's own
// functions and for BO3MacFix's, so that what comes out can be compared bit for bit. A line goes out for each thing
// the game is asked to do (draw an entity, make an axis, take note of a moved entity, its own part of a method) and
// for what everything is after each step.
//
// The plugin's side isn't part of the tests: wip/agents/ltc/compare (not in the repository) maps AAEUnified.dll under
// Wine without running anything of it and calls its functions by address (see compare.sh there). What that gave is
// in test_aae_linkcam.c as a line count and a checksum.
//
// Whoever includes this gives the side_ functions and cases_line, and has the game's functions call the cases_ ones.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// every side is to make the same cases: no multiplication and addition folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Cases_Locals 2
#define Cases_Cents 8
#define Cases_Cent_Size 0x400    // what of a client entity is used: its local client (a byte at +2), origin
#define Cases_Cent_Local 0x2     // (+0x40), angles (+0x4c) and number (+0x3f8), the same on both games
#define Cases_Cent_Origin 0x40
#define Cases_Cent_Angles 0x4c
#define Cases_Cent_Entnum 0x3f8
#define Cases_Lighting_Extra 0x18

// the list as both games have it in their cg_t (at different places)
typedef struct {
    int32_t count;
    int32_t entnum[4];
    uint32_t mode[4];
    float offset[4][3];
} cases_list_t;

static void cases_line(const char *line);
// the side's made-up game: a local client's list, the time and view, and the client entities
static cases_list_t *side_list(int local);
static int32_t *side_time(int local);
static float *side_view_origin(int local);
static float *side_view_axis(int local);
static float *side_view_angles(int local);
static uint8_t *side_cent(int index);
// the plugin's own state as it is before anything happened
static void side_reset(void);
// linktocamera for an entity of this local client (the game's own part of it is cases_stock_link), delete or
// forcedelete, an entity drawn, a view's axis just made (the game's own AnglesToAxis call comes first)
static void side_link(int local, int32_t entnum);
static void side_delete(int force, int local, int32_t entnum);
static void side_draw(void *object, uint8_t *cent, int32_t entnum, uint32_t flags, const uint8_t *lighting);
static void side_view(int local);
// what the plugin keeps: its copy of the numbers, and how many entities it holds back
static void side_noted(int32_t *count, int32_t entnum[4]);
static uint32_t side_held(void);

static uint64_t cases_seed;
static uint32_t cases_next(void) {
    cases_seed = cases_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(cases_seed >> 33);
}
static float cases_between(float low, float high) {
    return low + (high - low) * (float)(cases_next() & 0xffffff) / 16777216.0f;
}
static uint32_t cases_bits(float value) {
    uint32_t b;
    memcpy(&b, &value, sizeof(b));
    return b;
}
static float cases_from_bits(uint32_t b) {
    float value;
    memcpy(&value, &b, sizeof(value));
    return value;
}
static float cases_odd(float usual) {
    switch (cases_next() % 50) {
    case 0: return cases_from_bits(0x7fc00000);
    case 1: return cases_from_bits(0x7f800000);
    case 2: return cases_from_bits(0xff800000);
    case 3: return 3.0e38f;
    case 4: return -0.0f;
    case 5: return cases_from_bits(0x00000001);
    default: return usual;
    }
}

static void cases_say(const char *format, ...) {
    char text[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    cases_line(text);
}

// an entity's number as the side's made-up game has it, or 99 for no client entity of it, -1 for none
static int cases_cent_index(const uint8_t *cent) {
    if (cent == NULL)
        return -1;
    for (int i = 0; i < Cases_Cents; i++)
        if (side_cent(i) == cent)
            return i;
    return 99;
}
static uint32_t cases_word(const uint8_t *at) {
    uint32_t word;
    memcpy(&word, at, sizeof(word));
    return word;
}

// ---- the game's functions, which both sides' code calls

// what the next linktocamera is given
static uint32_t cases_link_mode;
static float cases_link_offset[3];

// the game's own part of linktocamera: the entity added to the list while there are fewer than four. With four the
// game makes a script error, which isn't made here: both sides go on behind it alike. (A count below 0, which only a
// made-up list has, is refused too; the game would write in front of the list.)
static void cases_stock_link(int local, int32_t entnum) {
    cases_list_t *list = side_list(local);
    if (list->count < 0 || list->count > 3) {
        cases_say("game: link %d refused, %d there", entnum, list->count);
        return;
    }
    list->entnum[list->count] = entnum;
    list->mode[list->count] = cases_link_mode;
    memcpy(list->offset[list->count], cases_link_offset, sizeof(cases_link_offset));
    list->count++;
    cases_say("game: link %d", entnum);
}
static void cases_stock_delete(int force, int32_t entnum) {
    cases_say("game: %s %d", force ? "forcedelete" : "delete", entnum);
}

static void cases_drawn(void *object, uint8_t *cent, int32_t entnum, uint32_t flags, const uint8_t *lighting) {
    char text[1024];
    int at = snprintf(text, sizeof(text), "game: drawn object %d cent %d entity %d flags %x", (int)(uintptr_t)object,
                      cases_cent_index(cent), entnum, flags);
    if (cent != NULL && cases_cent_index(cent) != 99)
        for (int k = 0; k < 6; k++)
            at += snprintf(text + at, sizeof(text) - at, " %08x", cases_word(cent + Cases_Cent_Origin + 4 * k));
    if (lighting == NULL) {
        at += snprintf(text + at, sizeof(text) - at, " lit by nothing");
    } else {
        at += snprintf(text + at, sizeof(text) - at, " lit");
        for (int i = 0; i < Cases_Lighting_Extra; i++)
            at += snprintf(text + at, sizeof(text) - at, " %02x", lighting[i]);
        const uint8_t *extra;
        memcpy(&extra, lighting + Cases_Lighting_Extra, sizeof(extra));
        if (extra == NULL) {
            at += snprintf(text + at, sizeof(text) - at, " and no more");
        } else if ((lighting[0] & 8) == 0) {
            at += snprintf(text + at, sizeof(text) - at, " and a pointer not used");
        } else {
            at += snprintf(text + at, sizeof(text) - at, " and more");
            for (int i = 0; i < 48; i++)
                at += snprintf(text + at, sizeof(text) - at, " %02x", extra[i]);
        }
    }
    cases_line(text);
}

// an axis from angles: not the game's, but made the same way on both sides from what it is given
static void cases_angles_to_axis(const float *angles, float *axis) {
    cases_say("game: axis from %08x %08x %08x", cases_bits(angles[0]), cases_bits(angles[1]), cases_bits(angles[2]));
    for (int k = 0; k < 9; k++)
        axis[k] = angles[k % 3] * (0.25f + 0.125f * (float)k) - angles[(k + 1) % 3] * 0.0625f + (float)(k - 4);
}

static void cases_pose_changed(int local, int32_t entnum) {
    cases_say("game: pose of %d changed for local client %d", entnum, local);
}

// ---- what is printed after each step

static void cases_state(void) {
    char text[1024];
    int at = 0;
    for (int local = 0; local < Cases_Locals; local++) {
        const cases_list_t *list = side_list(local);
        at += snprintf(text + at, sizeof(text) - at, "%slist %d:", local ? " | " : "", list->count);
        for (int i = 0; i < 4; i++)
            at += snprintf(text + at, sizeof(text) - at, " %d/%x/%08x,%08x,%08x", list->entnum[i], list->mode[i],
                           cases_bits(list->offset[i][0]), cases_bits(list->offset[i][1]),
                           cases_bits(list->offset[i][2]));
    }
    int32_t count, noted[4];
    side_noted(&count, noted);
    at += snprintf(text + at, sizeof(text) - at, " | noted %d:", count);
    for (int i = 0; i < count && i < 4; i++)
        at += snprintf(text + at, sizeof(text) - at, " %d", noted[i]);
    at += snprintf(text + at, sizeof(text) - at, " | held %u | cents", side_held());
    for (int i = 0; i < Cases_Cents; i++) {
        uint32_t hash = 2166136261u;
        for (int k = 0; k < 6; k++) {
            uint32_t word = cases_word(side_cent(i) + Cases_Cent_Origin + 4 * k);
            for (int b = 0; b < 4; b++)
                hash = (hash ^ ((word >> (8 * b)) & 0xff)) * 16777619u;
        }
        at += snprintf(text + at, sizeof(text) - at, " %08x", hash);
    }
    cases_line(text);
}

// ---- the made-up game's parts

static int32_t cases_entity(void) {
    static const int32_t numbers[] = { 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031, 7, -1 };
    return numbers[cases_next() % 10];
}
static uint32_t cases_mode(void) {
    switch (cases_next() % 12) {
    case 9: return 0xffffffff;
    case 10: return cases_next();
    case 11: return 6 + cases_next() % 4;
    default: return cases_next() % 6;
    }
}
static void cases_vector(float v[3], float range) {
    for (int k = 0; k < 3; k++)
        v[k] = cases_odd(cases_between(-range, range));
}

static void cases_new_view(int local) {
    cases_vector(side_view_angles(local), 360.0f);
    cases_vector(side_view_origin(local), 4000.0f);
    for (int k = 0; k < 9; k++)
        side_view_axis(local)[k] = cases_odd(cases_between(-1.0f, 1.0f));
    if (cases_next() % 3 != 0)
        *side_time(local) += 16 + (int32_t)(cases_next() % 3);
}
static void cases_new_cent(int index) {
    uint8_t *cent = side_cent(index);
    float origin[3], angles[3];
    int32_t entnum = cases_next() % 5 == 0 ? cases_entity() : 1024 + index;
    cent[Cases_Cent_Local] = (uint8_t)(cases_next() % 6);
    cases_vector(origin, 4000.0f);
    cases_vector(angles, 360.0f);
    memcpy(cent + Cases_Cent_Origin, origin, sizeof(origin));
    memcpy(cent + Cases_Cent_Angles, angles, sizeof(angles));
    memcpy(cent + Cases_Cent_Entnum, &entnum, sizeof(entnum));
}
static void cases_new_list(int local) {
    cases_list_t *list = side_list(local);
    static const int32_t counts[] = { 0, 1, 2, 3, 4, 4, 3, 5, -1, 0x7fffffff };
    list->count = counts[cases_next() % 10];
    for (int i = 0; i < 4; i++) {
        list->entnum[i] = cases_entity();
        list->mode[i] = cases_mode();
        cases_vector(list->offset[i], 30.0f);
    }
}

// how an entity is lit: nothing, a struct without more, one with more (or with the flag for it and no pointer)
static uint8_t cases_lighting[4][32], cases_extra[4][48];
static const uint8_t *cases_new_lighting(void) {
    unsigned kind = cases_next() % 5, which = cases_next() % 4;
    if (kind == 0)
        return NULL;
    uint8_t *lighting = cases_lighting[which];
    for (int i = 0; i < 32; i++)
        lighting[i] = (uint8_t)cases_next();
    for (int i = 0; i < 48; i++)
        cases_extra[which][i] = (uint8_t)cases_next();
    const uint8_t *extra = kind == 4 ? NULL : cases_extra[which];
    lighting[0] = (uint8_t)(kind == 1 ? lighting[0] & ~8 : lighting[0] | 8);
    memcpy(lighting + Cases_Lighting_Extra, &extra, sizeof(extra));
    return lighting;
}

static void cases_draw_one(int index) {
    uint8_t *cent = index < 0 ? NULL : side_cent(index);
    int32_t entnum =
        index < 0 || cases_next() % 8 == 0 ? cases_entity() : (int32_t)cases_word(cent + Cases_Cent_Entnum);
    void *object = (void *)(uintptr_t)(cases_next() % 9 == 0 ? 0 : 1 + cases_next() % 3);
    side_draw(object, cent, entnum, cases_next(), cases_new_lighting());
}

static void cases_step(void) {
    int local = cases_next() % 4 == 0 ? 1 : 0;
    unsigned what = cases_next() % 100;
    if (what < 14) {
        int32_t entnum = cases_next() % 4 == 0 ? cases_entity() : 1024 + (int32_t)(cases_next() % Cases_Cents);
        cases_link_mode = cases_mode();
        cases_vector(cases_link_offset, 30.0f);
        cases_say("link %d for local client %d, mode %x", entnum, local, cases_link_mode);
        side_link(local, entnum);
    } else if (what < 22) {
        int force = cases_next() % 2;
        int32_t entnum = cases_next() % 3 == 0 ? cases_entity() : 1024 + (int32_t)(cases_next() % Cases_Cents);
        cases_say("%s %d for local client %d", force ? "forcedelete" : "delete", entnum, local);
        side_delete(force, local, entnum);
    } else if (what < 50) {
        // a frame: some entities drawn, then the view worked out
        cases_say("frame of local client %d", local);
        int drawn = 1 + cases_next() % 6;
        for (int i = 0; i < drawn; i++)
            cases_draw_one(cases_next() % 12 == 0 ? -1 : (int)(cases_next() % Cases_Cents));
        if (cases_next() % 6 != 0)
            side_view(local);
    } else if (what < 62) {
        cases_say("draw");
        cases_draw_one(cases_next() % 12 == 0 ? -1 : (int)(cases_next() % Cases_Cents));
    } else if (what < 74) {
        cases_say("view of local client %d", local);
        side_view(local);
    } else if (what < 84) {
        cases_say("new view for local client %d", local);
        cases_new_view(local);
    } else if (what < 92) {
        int index = (int)(cases_next() % Cases_Cents);
        cases_say("entity %d moved", index);
        cases_new_cent(index);
    } else if (what < 94) {
        cases_say("the list of local client %d made up", local);
        cases_new_list(local);
    } else if (what < 99) {
        // a crowd: entities with the numbers in the list (several with one number, so up to eight are held back for
        // four places), drawn in a frame, some twice, then the view
        cases_say("crowd for local client %d", local);
        const cases_list_t *list = side_list(local);
        if (list->count > 0 && list->count <= 4)
            for (int i = 0; i < Cases_Cents; i++)
                if (cases_next() % 3 != 0)
                    memcpy(side_cent(i) + Cases_Cent_Entnum, &list->entnum[cases_next() % (uint32_t)list->count],
                           sizeof(int32_t));
        for (int i = 0; i < Cases_Cents; i++) {
            if (cases_next() % 4 != 0)
                cases_draw_one(i);
            if (cases_next() % 6 == 0)
                cases_draw_one(i);
        }
        side_view(local);
    } else {
        // the same frame once more: what was placed is where it goes already
        cases_say("frame again for local client %d", local);
        for (int i = 0; i < Cases_Cents; i++)
            if (cases_next() % 2 == 0)
                cases_draw_one(i);
        side_view(local);
    }
}

static void cases_run(void) {
    cases_seed = 0x6c696e6b63616d00ULL; // "linkcam"
    for (int c = 0; c < 4000; c++) {
        side_reset();
        for (int local = 0; local < Cases_Locals; local++) {
            memset(side_list(local), 0, sizeof(cases_list_t));
            *side_time(local) = 1000 + (int32_t)(cases_next() % 50);
            cases_new_view(local);
        }
        for (int i = 0; i < Cases_Cents; i++)
            cases_new_cent(i);
        cases_say("case %d", c);
        int steps = 6 + (int)(cases_next() % 14);
        for (int s = 0; s < steps; s++) {
            cases_step();
            cases_state();
        }
    }
}
