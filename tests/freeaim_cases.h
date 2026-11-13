// Made-up cases for aae_freeaim.c's sums: numbers to read and shots to turn, the same ones for the plugin's own
// functions and for BO3MacFix's, so that what comes out can be compared bit for bit. A line goes out for each case.
//
// The plugin's side isn't part of the tests: wip/windows/freeaim (not in the repository) maps AAEUnified.dll under
// Wine without running anything of it and calls its functions by address (see compare.sh there). What that gave is
// in test_aae_freeaim.c as a line count and a checksum.
//
// Whoever includes this gives the functions below and cases_line.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void cases_line(const char *line);
// reads a number at *text: whether there was one, with *text behind it then
static int side_number(const char **text, float *value);
// turns the nine directions: whether that worked
static int side_turn(const float muzzle[3], const float directions[9], const float target[3], float turned[9]);

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
    switch (cases_next() % 60) {
    case 0: return cases_from_bits(0x7fc00000);
    case 1: return cases_from_bits(0x7f800000);
    case 2: return cases_from_bits(0xff800000);
    case 3: return 3.0e7f;
    case 4: return -3.0e7f;
    case 5: return 0.0f;
    default: return usual;
    }
}

// ---- numbers

static void cases_number(const char *text) {
    char line[400];
    const char *at = text;
    float value = 7.5f;
    int found = side_number(&at, &value);
    snprintf(line, sizeof(line), "n %d %d %08x", found, (int)(at - text), found ? cases_bits(value) : 0);
    cases_line(line);
}
static void cases_numbers(void) {
    static const char *const written[] = {
        "", " ", "0", "1", "-1", "+1", "12", "12.5", "12.", ".5", ".", "-.5", "+.", "-", "+", "1e3", "1E3", "1e+3", "1e-3",
        "1e", "1e+", "1ex", "1.5e2", "1.5e-2", "5e38", "5e39", "5e-38", "5e-39", "1e100", "1e-100", "1e038", "1e0038",
        "16777216", "16777217", "16777216.5", "-16777216", "-16777217", "0.1", "0.2", "0.3", "0.30000000000000004",
        "123456789", "1234567.875", "3.14159265358979", "  42", "\t\n\v\f\r 7", "4 5 6", "4,5", "4x", "x4", "--4", "+-4",
        "1.2.3", "1..2", "0x10", "1e5e2", "1e-0", "-0", "-0.0", "00012", "9999999999999999999999", "0.000000000000001",
        "124.433 -641.925 149.125", "4.71381 -192.891 149.125", "1e3x", ".e3", "5.e3", "5.x", ("\x80" "5"), "5\x80", "5.\x80",
        "5e\x80", "1e9", "1e7", "1e8", "167772.16e2", "1e-45", "1.4e-45", "3.4028235e38", "inf", "nan", "1e+38", "1e-38",
    };
    for (size_t i = 0; i < sizeof(written) / sizeof(written[0]); i++)
        cases_number(written[i]);
    // and made-up ones
    static const char pieces[] = "0123456789.-+eE x";
    for (int i = 0; i < 20000; i++) {
        char text[40];
        int length = 1 + (int)(cases_next() % 14);
        for (int k = 0; k < length; k++) {
            uint32_t pick = cases_next() % 40;
            text[k] = pick < 24 ? pieces[pick % 10] : pieces[10 + pick % 7];
        }
        text[length] = '\0';
        cases_number(text);
    }
    for (int i = 0; i < 20000; i++) {
        char text[64];
        snprintf(text, sizeof(text), "%.*f", (int)(cases_next() % 8), (double)cases_between(-20000.0f, 20000.0f));
        cases_number(text);
        snprintf(text, sizeof(text), "%.*e", (int)(cases_next() % 9), (double)cases_between(-1.0f, 1.0f) * 1.0e6);
        cases_number(text);
        snprintf(text, sizeof(text), "%g", (double)cases_between(-70000.0f, 70000.0f));
        cases_number(text);
    }
}

// ---- turns

static void cases_cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
static void cases_scale(float v[3], float to) {
    float length = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    // a square root by halving, so that both sides get the same numbers whatever their libraries do
    float root = length > 1.0f ? length : 1.0f;
    for (int i = 0; i < 40; i++)
        root = 0.5f * (root + length / root);
    if (root > 0.0f)
        for (int i = 0; i < 3; i++)
            v[i] = v[i] / root * to;
}
static void cases_turn_one(const float muzzle[3], const float directions[9], const float target[3]) {
    char line[400];
    float turned[9];
    for (int i = 0; i < 9; i++)
        turned[i] = 5.0f;
    int worked = side_turn(muzzle, directions, target, turned);
    int n = snprintf(line, sizeof(line), "t %d", worked);
    for (int i = 0; i < 9 && worked; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %08x", cases_bits(turned[i]));
    cases_line(line);
}
static void cases_turns(void) {
    for (int i = 0; i < 60000; i++) {
        float muzzle[3], target[3], d[9];
        // three directions at right angles, a little off now and then
        float forward[3] = { cases_between(-1, 1), cases_between(-1, 1), cases_between(-1, 1) };
        float other[3] = { cases_between(-1, 1), cases_between(-1, 1), cases_between(-1, 1) };
        if (cases_next() % 4 == 0)
            forward[0] = forward[1] = 0.0f, forward[2] = cases_next() % 2 ? 1.0f : -1.0f;
        cases_scale(forward, 1.0f);
        float right[3], up[3];
        cases_cross(forward, other, right);
        cases_scale(right, 1.0f);
        cases_cross(right, forward, up);
        cases_scale(up, 1.0f);
        memcpy(d, forward, sizeof(forward));
        memcpy(d + 3, right, sizeof(right));
        memcpy(d + 6, up, sizeof(up));
        uint32_t off = cases_next() % 12;
        if (off == 0)
            d[cases_next() % 9] += cases_between(-0.08f, 0.08f);
        else if (off == 1)
            cases_scale(d + 3 * (cases_next() % 3), cases_between(0.9f, 1.1f));
        else if (off == 2)
            d[cases_next() % 9] = cases_odd(0.5f);
        for (int k = 0; k < 3; k++)
            muzzle[k] = cases_between(-6000.0f, 6000.0f);
        if (cases_next() % 30 == 0)
            muzzle[cases_next() % 3] = cases_odd(2.0e7f);
        // a target somewhere, or straight ahead, or straight back, or at the muzzle
        uint32_t where = cases_next() % 16;
        float away = cases_between(1.0f, 4000.0f);
        for (int k = 0; k < 3; k++) {
            if (where == 0)
                target[k] = muzzle[k] + forward[k] * away;
            else if (where == 1)
                target[k] = muzzle[k] - forward[k] * away;
            else if (where == 2)
                target[k] = muzzle[k];
            else if (where == 3)
                target[k] = muzzle[k] + cases_between(-0.000001f, 0.000001f);
            else
                target[k] = muzzle[k] + cases_between(-3000.0f, 3000.0f);
        }
        if (cases_next() % 40 == 0)
            target[cases_next() % 3] = cases_odd(1.0e9f);
        cases_turn_one(muzzle, d, target);
    }
    // exactly straight back along each axis
    for (int axis = 0; axis < 3; axis++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            float muzzle[3] = { 10, 20, 30 }, target[3] = { 10, 20, 30 }, d[9] = { 0 };
            d[axis] = (float)sign;
            d[3 + (axis + 1) % 3] = 1.0f;
            d[6 + (axis + 2) % 3] = (float)sign;
            target[axis] -= 100.0f * (float)sign;
            cases_turn_one(muzzle, d, target);
            target[axis] += 200.0f * (float)sign;
            cases_turn_one(muzzle, d, target);
        }
    }
}

static void cases_run(void) {
    cases_seed = 20261112;
    cases_numbers();
    cases_turns();
}
