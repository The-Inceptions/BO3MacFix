// Made-up cases for T7Overcharged's slide push (slide_preserve_momentum, aae_move_slide.c): player states and moves
// for both of its pushes and for neither, with ordinary, huge, tiny and denormal numbers, infinities and NaNs. The same
// ones go through T7Overcharged's own function and through BO3MacFix's, so that what comes out can be compared bit for
// bit. A line goes out for each case.
//
// T7Overcharged's side isn't part of the tests: wip/agents/movec/wine (not in the repository) maps its DLL under Wine
// and calls its function by address, with its pointer to the game's PM_Accelerate set to cases_accelerate below.
// What that gave is in test_aae_move_slide.c as a line count and a checksum.
//
// Whoever includes this gives cases_line and side_push (the slide push for one player state and move, which calls
// cases_accelerate where the game's PM_Accelerate would be called).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// every side is to make the same cases: no multiplication and addition folded into one operation
#pragma STDC FP_CONTRACT OFF

static void cases_line(const char *line);
static void side_push(uint8_t *ps, uint8_t *pml);

// 1: a NaN that the sums made goes out with its bits; 0: as "nan", because which NaN an operation makes isn't the same
// on every CPU (x86 makes 0xffc00000 where arm64 makes 0x7fc00000). The comparison with T7Overcharged's code runs the
// x86 build with 1 as well.
static int cases_exact;

#define Cases_Size 0x100   // of the made-up player state and move
#define Cases_Random 100000

static uint64_t cases_seed;
static uint32_t cases_next(void) {
    cases_seed = cases_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(cases_seed >> 33);
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
// a float with a random sign and mantissa and an exponent from low to high (biased, 0 to 255)
static float cases_float_between(uint32_t low, uint32_t high) {
    uint32_t exponent = low + cases_next() % (high - low + 1);
    return cases_from_bits((cases_next() & 0x807fffffu) | exponent << 23);
}

// the odd numbers every part of a case gets now and then
static const uint32_t cases_odd_bits[] = {
    0x00000000, 0x80000000, 0x7f800000, 0xff800000, // zeros and infinities
    0x7fc00000, 0xffc00000, 0x7fd23456, 0xffa00001, 0x7f800001, // NaNs: quiet, with payloads, signalling
    0x7f7fffff, 0xff7fffff, 0x00000001, 0x807fffff, 0x00800000, // the largest, denormals, the least normal
    0x5f8ac723, 0x5ff0000f, 0x1e3ce508, 0x1a0f0000, // about 2e19 and 3.5e19 (squares above the largest float),
                                                     // 1e-20 and 3e-23 (squares denormal or nothing)
    0x3f800000, 0xbf800000, 0x3f3504f3, 0x47afc800, 0x47afc801, // 1, -1, 1/sqrt 2, 90000 and the float after it
};
static float cases_odd(float usual) {
    uint32_t pick = cases_next() % 64;
    if (pick < sizeof(cases_odd_bits) / sizeof(cases_odd_bits[0]))
        return cases_from_bits(cases_odd_bits[pick]);
    return usual;
}

static void cases_append(char *line, size_t size, float value, int made) {
    size_t used = strlen(line);
    uint32_t bits = cases_bits(value);
    bool not_a_number = (bits & 0x7f800000) == 0x7f800000 && (bits & 0x007fffff) != 0;
    if (made && not_a_number && !cases_exact)
        snprintf(line + used, size - used, " nan");
    else
        snprintf(line + used, size - used, " %08x", bits);
}
static uint64_t cases_hash(const uint8_t *bytes, size_t size) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < size; i++)
        hash = (hash ^ bytes[i]) * 0x100000001b3ULL;
    return hash;
}

// what the slide push gave the game's PM_Accelerate, and the velocity PM_Accelerate leaves (a case's own)
static struct {
    int calls;
    const uint8_t *ps, *pml;
    float direction[3], wish_speed, acceleration;
    int write_after;
    float after[3];
} cases_called;
static void cases_accelerate(uint8_t *ps, uint8_t *pml, const float *direction, float wish_speed, float acceleration) {
    cases_called.calls++;
    cases_called.ps = ps, cases_called.pml = pml;
    memcpy(cases_called.direction, direction, sizeof(cases_called.direction));
    cases_called.wish_speed = wish_speed, cases_called.acceleration = acceleration;
    if (cases_called.write_after)
        memcpy(ps + 0x3c, cases_called.after, sizeof(cases_called.after));
}

// one case: the push for this player state and move, and a line with what PM_Accelerate was given and what the player
// state and the move are afterwards
static void cases_one(uint8_t *ps, uint8_t *pml) {
    char line[400];
    uint8_t ps_before[Cases_Size], pml_before[Cases_Size];
    memcpy(ps_before, ps, Cases_Size);
    memcpy(pml_before, pml, Cases_Size);
    cases_called.calls = 0, cases_called.ps = NULL, cases_called.pml = NULL;
    side_push(ps, pml);
    snprintf(line, sizeof(line), "c %d %d %d", cases_called.calls, cases_called.ps == ps, cases_called.pml == pml);
    if (cases_called.calls != 0) {
        for (int i = 0; i < 3; i++)
            cases_append(line, sizeof(line), cases_called.direction[i], 1);
        cases_append(line, sizeof(line), cases_called.wish_speed, 1);
        cases_append(line, sizeof(line), cases_called.acceleration, 1);
    }
    // the velocity afterwards is only ever copied, never worked out: its bits are the same everywhere
    float velocity[3];
    memcpy(velocity, ps + 0x3c, sizeof(velocity));
    for (int i = 0; i < 3; i++)
        cases_append(line, sizeof(line), velocity[i], 0);
    size_t used = strlen(line);
    snprintf(line + used, sizeof(line) - used, " %d %d %016llx %016llx", memcmp(ps, ps_before, Cases_Size) == 0,
             memcmp(pml, pml_before, Cases_Size) == 0, (unsigned long long)cases_hash(ps, Cases_Size),
             (unsigned long long)cases_hash(pml, Cases_Size));
    cases_line(line);
}

// A player state and a move with everything else made up. kind: 0 on a slide surface, 1 the other push, 2 the other
// push's flag without the move's int, 3 neither.
static void cases_make(uint8_t *ps, uint8_t *pml, int kind, const float along[2], const float velocity[3]) {
    for (int i = 0; i < Cases_Size; i++)
        ps[i] = (uint8_t)cases_next(), pml[i] = (uint8_t)cases_next();
    uint64_t flags = (uint64_t)cases_next() << 32;
    flags |= cases_next();
    int32_t flagged = (int32_t)cases_next();
    flags = kind == 1 || kind == 2 ? flags | 1ull << 51 : flags & ~(1ull << 51);
    if (kind == 0)
        pml[0x54] |= 2;
    else
        pml[0x54] &= (uint8_t)~2;
    if (kind == 1 && flagged == 0)
        flagged = 1;
    if (kind == 2)
        flagged = 0;
    memcpy(ps + 0x10, &flags, sizeof(flags));
    memcpy(ps + 0x3c, velocity, 3 * sizeof(float));
    memcpy(pml + 0x40, along, 2 * sizeof(float));
    memcpy(pml + 0x64, &flagged, sizeof(flagged));
    // what PM_Accelerate leaves: as it is, a velocity of the usual size, or one far above 90000 or near it
    cases_called.write_after = cases_next() % 4 != 0;
    switch (cases_next() % 4) {
    case 0:
        for (int i = 0; i < 3; i++)
            cases_called.after[i] = cases_odd(cases_float_between(118, 137));
        break;
    case 1:
        for (int i = 0; i < 3; i++)
            cases_called.after[i] = cases_float_between(140, 145);
        break;
    default: {
        // a speed of 90000 give or take a little, all of it in one part
        uint32_t around = 0x47afc800u + (cases_next() % 5) - 2, part = cases_next() % 3;
        around |= cases_next() & 0x80000000u;
        cases_called.after[0] = cases_called.after[1] = cases_called.after[2] = 0.0f;
        cases_called.after[part] = cases_from_bits(around);
    }
    }
}

static void cases_run(void) {
    static uint8_t ps[Cases_Size], pml[Cases_Size];
    cases_seed = 0x5113de5113de5113ULL;
    const size_t odd_count = sizeof(cases_odd_bits) / sizeof(cases_odd_bits[0]);
    // every odd number in each part of the move's two floats and of the velocity, in both pushes
    for (int kind = 0; kind < 2; kind++) {
        for (size_t i = 0; i < odd_count; i++) {
            for (size_t j = 0; j < odd_count; j++) {
                float along[2] = { cases_from_bits(cases_odd_bits[i]), cases_from_bits(cases_odd_bits[j]) };
                float velocity[3] = { 312.5f, -147.25f, 12.0f };
                cases_make(ps, pml, kind, along, velocity);
                cases_one(ps, pml);
                float along2[2] = { 0.6f, -0.8f };
                float velocity2[3] = { cases_from_bits(cases_odd_bits[i]), cases_from_bits(cases_odd_bits[j]),
                                       cases_from_bits(cases_odd_bits[(i + j) % odd_count]) };
                cases_make(ps, pml, kind, along2, velocity2);
                cases_one(ps, pml);
            }
        }
    }
    // made-up ones: directions and velocities of every size the game has, now and then an odd one
    for (int n = 0; n < Cases_Random; n++) {
        uint32_t pick = cases_next() % 10;
        int kind = pick < 4 ? 0 : pick < 9 ? 1 : (int)(2 + cases_next() % 2);
        float along[2], velocity[3];
        uint32_t range = cases_next() % 4;
        for (int i = 0; i < 2; i++)
            along[i] = cases_odd(range == 0   ? cases_float_between(0, 254)
                                 : range == 1 ? cases_float_between(100, 130)
                                              : cases_float_between(120, 127));
        for (int i = 0; i < 3; i++)
            velocity[i] = cases_odd(range == 0 ? cases_float_between(0, 254) : cases_float_between(110, 137));
        cases_make(ps, pml, kind, along, velocity);
        cases_one(ps, pml);
    }
}
