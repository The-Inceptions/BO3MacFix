// Made-up cases for aae_vmwidget.c: widgets and lines set up and updated, and bones kept from a skeleton, the same
// ones for the plugin's own functions and for BO3MacFix's, so that what comes out can be compared bit for bit. A line
// goes out for each case.
//
// The plugin's side isn't part of the tests: wip/agents/vua/wine (not in the repository) maps AAEUnified.dll under
// Wine without running anything of it and calls its functions by address (see compare.sh there): its two methods to
// set an element up, the updates they give it, and its function that runs after a skeleton is computed. What that
// gave is in test_aae_vmwidget.c as a line count and a checksum.
//
// Whoever includes this gives the functions below and cases_line.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// every side is to make the same cases: no multiplication and addition folded into one operation, which a compiler
// may do differently for each side (also when it works a case out itself while compiling)
#pragma STDC FP_CONTRACT OFF

typedef struct {
    uint32_t type[3];  // the offsets as arguments 3 to 5 of the method: their type words (3: a number)
    float value[3];    // and their numbers
    float position[3]; // the bone as it is kept for local client 0
    float rotation[4];
    float screen[2];   // what the projection answers
    int writes;        // whether the projection writes an answer at all
    uint32_t size;     // how many places the line's table has in its array
    int fails;         // whether the line's Lua function fails
} cases_widget_t;

typedef struct {
    float rotation[4], position[3]; // the bone in the skeleton
    uint16_t bones;                 // how many bones the model has
    int16_t index;                  // where DObjGetBoneIndex finds it
    int found;                      // what DObjGetBoneIndex answers
    int computed;                   // whether the model's bits say the bone has been computed
    int skeleton;                   // whether the model has a skeleton at all
} cases_bone_t;

static void cases_line(const char *line);
// sets an element up with setupViewmodelTagWidget and updates it for local client 0: whether the game's 3D widget
// update was called, with the point the element had then
static int side_widget(const cases_widget_t *c, float point[3]);
// sets an element up with setupViewmodelTagWidgetLine and updates it: whether the point was projected and which, what
// the Lua function was handed (0: it wasn't called, 1: nil, 2: the table, with the two numbers in it), and whether the
// line was switched off afterwards
static void side_line(const cases_widget_t *c, int *projected, float point[3], int *handed, float numbers[2],
                      int *off);
// the skeleton of local client 0's weapon model is computed with this bone: whether it was kept, and what was kept
// (rotation, then position)
static int side_bone(const cases_bone_t *c, float kept[7]);

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
// now and then a value at an edge of the plugin's rules, or one that is no number at all
static float cases_odd(float usual) {
    switch (cases_next() % 48) {
    case 0: return cases_from_bits(0x7fc00000);
    case 1: return cases_from_bits(0x7f800000);
    case 2: return cases_from_bits(0xff800000);
    case 3: return 1000000.0f;
    case 4: return -1000000.0f;
    case 5: return cases_from_bits(0x49742401); // the float above 1,000,000
    case 6: return 10000000.0f;
    case 7: return cases_from_bits(0x4b18967f); // the float below 10,000,000
    case 8: return -9999999.0f;
    case 9: return 0.0f;
    case 10: return -0.0f;
    case 11: return cases_from_bits(0x00000001);
    case 12: return 3.0e7f;
    default: return usual;
    }
}

// a rotation of the usual kind: a unit quaternion made longer or shorter
static void cases_rotation(float rotation[4], float least, float most) {
    float x = cases_between(-1.0f, 1.0f), y = cases_between(-1.0f, 1.0f), z = cases_between(-1.0f, 1.0f),
          w = cases_between(-1.0f, 1.0f);
    float length = x * x + y * y + z * z + w * w, scale = cases_between(least, most);
    // a square root that both sides do the same way: Newton's steps from 1
    float root = 1.0f;
    for (int i = 0; i < 30; i++)
        root = 0.5f * (root + length / root);
    rotation[0] = x / root * scale, rotation[1] = y / root * scale;
    rotation[2] = z / root * scale, rotation[3] = w / root * scale;
}

static void cases_widget_one(const cases_widget_t *c) {
    char line[400];
    float point[3] = { 0.0f, 0.0f, 0.0f }, numbers[2] = { 0.0f, 0.0f };
    int called = side_widget(c, point);
    snprintf(line, sizeof(line), "widget %d %08x %08x %08x", called, called ? cases_bits(point[0]) : 0,
             called ? cases_bits(point[1]) : 0, called ? cases_bits(point[2]) : 0);
    cases_line(line);
    int projected = 0, handed = 0, off = 0;
    memset(point, 0, sizeof(point));
    side_line(c, &projected, point, &handed, numbers, &off);
    snprintf(line, sizeof(line), "line %d %08x %08x %08x %d %08x %08x %d", projected,
             projected ? cases_bits(point[0]) : 0, projected ? cases_bits(point[1]) : 0,
             projected ? cases_bits(point[2]) : 0, handed, handed == 2 ? cases_bits(numbers[0]) : 0,
             handed == 2 ? cases_bits(numbers[1]) : 0, off);
    cases_line(line);
}

static void cases_widgets(void) {
    static const uint32_t odd_types[] = { 0x13, 4, 0, 0x103, 2 };
    for (int n = 0; n < 60000; n++) {
        cases_widget_t c;
        memset(&c, 0, sizeof(c));
        for (int i = 0; i < 3; i++) {
            c.type[i] = cases_next() % 16 == 0 ? odd_types[cases_next() % 5] : 3;
            switch (cases_next() % 8) {
            case 0: c.value[i] = 0.0f; break; // no offset along this direction
            case 1: c.value[i] = cases_between(-2000000.0f, 2000000.0f); break;
            default: c.value[i] = cases_odd(cases_between(-40.0f, 40.0f)); break;
            }
        }
        if (cases_next() % 10 == 0)
            c.value[0] = c.value[1] = c.value[2] = 0.0f;
        for (int i = 0; i < 3; i++)
            c.position[i] = cases_next() % 20 == 0 ? cases_odd(9999990.0f) : cases_between(-4000.0f, 4000.0f);
        switch (cases_next() % 12) {
        case 0: c.rotation[0] = c.rotation[1] = c.rotation[2] = c.rotation[3] = 0.0f; break;
        case 1: cases_rotation(c.rotation, 0.0001f, 100.0f); break;
        case 2: cases_rotation(c.rotation, 1.0f, 1.0f); c.rotation[cases_next() % 4] = cases_odd(0.5f); break;
        default: cases_rotation(c.rotation, 0.5f, 1.9f); break;
        }
        c.screen[0] = cases_odd(cases_between(-300.0f, 1600.0f));
        c.screen[1] = cases_odd(cases_between(-300.0f, 1000.0f));
        c.writes = cases_next() % 10 != 0;
        static const uint32_t sizes[] = { 0, 1, 2, 3, 0x100000, 0x100001, 0xffffffff };
        c.size = cases_next() % 6 == 0 ? sizes[cases_next() % 7] : 2;
        c.fails = cases_next() % 9 == 0;
        cases_widget_one(&c);
    }
}

static void cases_bone_one(const cases_bone_t *c) {
    char line[400];
    float kept[7] = { 0 };
    int valid = side_bone(c, kept);
    int at = snprintf(line, sizeof(line), "bone %d", valid);
    for (int i = 0; i < 7; i++)
        at += snprintf(line + at, sizeof(line) - (size_t)at, " %08x", valid ? cases_bits(kept[i]) : 0);
    cases_line(line);
}

static void cases_bones(void) {
    // rotations whose squared length is within a float of the edges, where the order of the sum decides
    static const uint32_t edges[4][4] = { { 0xbe90bcf2, 0xbe130f78, 0x3e9a9194, 0xbe774b94 },
                                          { 0x3e86d6d7, 0xbe8d2416, 0x3e6d7f3b, 0xbe66eea0 },
                                          { 0x3f7b7752, 0xbfbfc9e2, 0xbf1e356d, 0x3f238bd1 },
                                          { 0xbdc8f522, 0xbfb84619, 0xbfae3f42, 0xbe8230e1 } };
    for (int n = 0; n < 4; n++) {
        cases_bone_t c;
        memset(&c, 0, sizeof(c));
        for (int i = 0; i < 4; i++)
            c.rotation[i] = cases_from_bits(edges[n][i]);
        c.position[0] = 1.0f, c.position[1] = 2.0f, c.position[2] = 3.0f;
        c.bones = 8, c.index = 3, c.found = 1, c.computed = 1, c.skeleton = 1;
        cases_bone_one(&c);
    }
    for (int n = 0; n < 60000; n++) {
        cases_bone_t c;
        memset(&c, 0, sizeof(c));
        switch (cases_next() % 10) {
        case 0: cases_rotation(c.rotation, 0.49f, 0.51f); break; // a squared length about the least
        case 1: cases_rotation(c.rotation, 1.99f, 2.01f); break; // about the most
        case 2: cases_rotation(c.rotation, 0.0f, 3.0f); break;
        case 3: {
            // exactly at the edges: 0.25 and 4
            float edge = cases_next() % 2 ? 0.5f : 2.0f;
            c.rotation[0] = c.rotation[1] = c.rotation[2] = 0.0f, c.rotation[3] = edge;
            c.rotation[cases_next() % 4] = cases_next() % 2 ? edge : -edge;
            break;
        }
        default: cases_rotation(c.rotation, 0.6f, 1.6f); break;
        }
        if (cases_next() % 15 == 0)
            c.rotation[cases_next() % 4] = cases_odd(0.5f);
        for (int i = 0; i < 3; i++)
            c.position[i] = cases_next() % 12 == 0 ? cases_odd(9999990.0f) : cases_between(-4000.0f, 4000.0f);
        static const uint16_t counts[] = { 0, 1, 0x17f, 0x180, 0x181, 0xffff };
        c.bones = cases_next() % 8 == 0 ? counts[cases_next() % 6] : (uint16_t)(1 + cases_next() % 0x180);
        int bones = c.bones > 0x181 ? 0x181 : c.bones;
        c.index = cases_next() % 8 == 0 ? (int16_t)(cases_next() % 5) - 2 : (int16_t)(cases_next() % (bones + 2));
        if (c.index > 0x180)
            c.index = 0x180;
        c.found = cases_next() % 10 != 0;
        c.computed = cases_next() % 10 != 0;
        c.skeleton = cases_next() % 30 != 0;
        cases_bone_one(&c);
    }
}

static void cases_run(void) {
    cases_seed = 0x76756121;
    cases_widgets();
    cases_bones();
}
