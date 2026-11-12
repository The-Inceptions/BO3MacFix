// Made-up frames for a prompt's glide (aae_prompt.c): the same ones for the plugin's own function and for
// BO3MacFix's, so that what comes out can be compared bit for bit. After every frame a line goes out with the
// prompt's place and everything kept for it, as bits.
//
// The plugin's side isn't part of the tests: wip/windows/glide (not in the repository) maps AAEUnified.dll under
// Wine without running anything of it and calls its three functions by address. What that gave is in
// test_aae_prompt.c as a line count and a checksum.
//
// Whoever includes this gives the three functions below and frames_line.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void frames_line(const char *line);
static void side_reset(void *element);
static void side_step(void *element, float *x, float *y, float speed, uint32_t frame_ms, float width);
// the kept values as the plugin lays them out: 8 floats, then "has target", "start pending", "active"
static void side_state(void *element, uint8_t out[0x23]);

static uint64_t seed;
static uint32_t next(void) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(seed >> 33);
}
static float between(float low, float high) {
    return low + (high - low) * (float)(next() & 0xffffff) / 16777216.0f;
}
static uint32_t bits(float value) {
    uint32_t b;
    memcpy(&b, &value, sizeof(b));
    return b;
}
static float from_bits(uint32_t b) {
    float value;
    memcpy(&value, &b, sizeof(value));
    return value;
}
// now and then a value that isn't an ordinary one
static float odd(float usual) {
    switch (next() % 40) {
    case 0: return from_bits(0x7fc00000);  // not a number
    case 1: return from_bits(0x7f800000);  // infinite
    case 2: return from_bits(0xff800000);
    case 3: return 1.0e9f;
    case 4: return -1.0e9f;
    case 5: return 0.0f;
    case 6: return -usual;
    default: return usual;
    }
}

// a prompt's element: only the two rectangles matter
struct element {
    uint8_t before[0x34];
    float rectangle[4], drawn[4];
    uint8_t after[0x200];
};
static struct element elements[400];

static void print_frame(int scenario, int frame, struct element *e, float x, float y) {
    uint8_t state[0x23];
    side_state(e, state);
    char line[160];
    int n = snprintf(line, sizeof(line), "%d %d %08x %08x |", scenario, frame, bits(x), bits(y));
    for (int i = 0; i < 8; i++) {
        uint32_t b;
        memcpy(&b, state + 4 * i, sizeof(b));
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", b);
    }
    snprintf(line + n, sizeof(line) - (size_t)n, " %d%d%d", state[0x20], state[0x21], state[0x22]);
    frames_line(line);
}

#define Frames_Scenarios 600
#define Frames_Each 160
static void frames_run(void) {
    const int scenarios = Frames_Scenarios, frames = Frames_Each;
    for (int s = 0; s < scenarios; s++) {
        seed = 0x9e3779b97f4a7c15ULL * (uint64_t)(s + 1);
        struct element *e = &elements[s % 3];
        memset(e, 0, sizeof(*e));
        // the element's own rectangle: mostly around its place, like a prompt's
        float half_w = between(10.0f, 300.0f), half_h = between(5.0f, 80.0f);
        e->rectangle[0] = -half_w, e->rectangle[1] = -half_h, e->rectangle[2] = half_w, e->rectangle[3] = half_h;
        if (next() % 6 == 0)
            for (int i = 0; i < 4; i++)
                e->rectangle[i] = odd(between(-2000.0f, 2000.0f));
        float width = next() % 5 == 0 ? odd(between(100.0f, 9000.0f)) : (next() % 2 ? 1280.0f : 1680.0f);
        float speed = next() % 6 == 0 ? odd(between(-10.0f, 12000.0f)) : between(50.0f, 900.0f);
        // the prompt's own place wanders, and was drawn somewhere else before
        float x = between(0.0f, 1280.0f), y = between(0.0f, 720.0f);
        float fixed_x = between(0.0f, 1280.0f), fixed_y = between(0.0f, 720.0f);
        e->drawn[0] = fixed_x + e->rectangle[0], e->drawn[1] = fixed_y + e->rectangle[1];
        if (next() % 12 == 0)
            e->drawn[next() % 2] = odd(5.0f);
        int docked = 0;
        if (next() % 3)
            side_reset(e);
        for (int f = 0; f < frames; f++) {
            x += between(-6.0f, 6.0f), y += between(-4.0f, 4.0f);
            uint32_t ms = next() % 30 == 0 ? next() % 400 : 4 + next() % 30;
            if (next() % 50 == 0)
                ms = 0;
            if (next() % 60 == 0)
                speed = odd(between(50.0f, 900.0f));
            // sometimes the prompt is the usable one for a while, drawn at the fixed place
            if (!docked && next() % 45 == 0)
                docked = 3 + (int)(next() % 10);
            if (docked) {
                docked--;
                side_reset(e);
                e->drawn[0] = fixed_x + e->rectangle[0], e->drawn[1] = fixed_y + e->rectangle[1];
                print_frame(s, f, e, fixed_x, fixed_y);
                continue;
            }
            float px = next() % 25 == 0 ? odd(x) : x, py = next() % 25 == 0 ? odd(y) : y;
            side_step(e, &px, &py, speed, ms, width);
            // the game writes the rectangle from the place it is left with
            e->drawn[0] = px + e->rectangle[0], e->drawn[1] = py + e->rectangle[1];
            e->drawn[2] = px + e->rectangle[2], e->drawn[3] = py + e->rectangle[3];
            print_frame(s, f, e, px, py);
        }
    }
    // more prompts than there is room for: which ones keep their glide
    seed = 12345;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 400; i++) {
            struct element *e = &elements[(i * 7 + round * 13) % 400];
            float half = 20.0f + (float)(i % 9);
            e->rectangle[0] = -half, e->rectangle[1] = -half, e->rectangle[2] = half, e->rectangle[3] = half;
            e->drawn[0] = 100.0f - half, e->drawn[1] = 100.0f - half;
            if (next() % 3 == 0)
                side_reset(e);
            float px = 600.0f + (float)(i % 50), py = 300.0f;
            if (next() % 2)
                side_step(e, &px, &py, 300.0f, 16, 1280.0f);
            print_frame(1000 + round, i, e, px, py);
        }
    }
}
