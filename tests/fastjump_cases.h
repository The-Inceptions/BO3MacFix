// Made-up cases for aae_move_fastjump.c: calls of PM_Accelerate and of PM_Friction with the patch on, the same ones
// for T7Overcharged's own code and for BO3MacFix's, so that what comes out can be compared bit for bit. A line goes
// out for each case.
//
// T7Overcharged's side isn't part of the tests: wip/agents/movef/wine (not in the repository) maps its DLL under Wine
// with wip/windows/t7o/t7o_map.h, without running anything of it, and calls its hooks by address (compare.sh there).
// What that gave is in test_aae_move_fastjump.c as a line count and a checksum.
//
// Whoever includes this gives the functions below and cases_line.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// every side is to make the same cases: no multiplication and addition folded into one operation, which a compiler
// may do differently for each side (also when it works a case out itself while compiling)
#pragma STDC FP_CONTRACT OFF

static void cases_line(const char *line);
// One call of PM_Accelerate as the game makes it from this place (0 PM_AirMove's first call, 1 PM_WalkMove's first,
// 2 any other) for a player state with these movement flags and this horizontal velocity: what the game's own
// function then gets. copy is 1 when it got a copy of the direction (the same 12 bytes elsewhere), 0 when it got the
// direction itself, 2 for anything else.
static void side_accelerate(int place, uint8_t movement_flags, float velocity_x, float velocity_y,
                            const float direction[3], float wish_speed, float acceleration, float *got_wish_speed,
                            float *got_acceleration, int *copy);
// One call of PM_Friction for player state number which (0 to 2), when the game's PM_IsInAir answers in_air and the
// clock (GetTickCount64 for T7Overcharged) is at now: the friction the game's own PM_Friction then works with at its
// main call (the game's 5.5, or 0).
static float side_friction(int which, int in_air, uint8_t movement_flags, float velocity_x, float velocity_y,
                           uint64_t now);

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
    case 1: return cases_from_bits(0xffc00000);
    case 2: return cases_from_bits(0x7f800000);
    case 3: return cases_from_bits(0xff800000);
    case 4: return 3.0e19f;  // its square is too big for a float
    case 5: return -0.0f;
    case 6: return cases_from_bits(0x00000001);
    default: return usual;
    }
}
// movement flags' low byte: bit 7 is sliding
static uint8_t cases_flags(void) {
    static const uint8_t usual[] = { 0x00, 0x00, 0x00, 0x20, 0x80, 0x7f, 0xff, 0xa0, 0x01, 0x40 };
    uint32_t pick = cases_next() % 12;
    return pick < sizeof(usual) ? usual[pick] : (uint8_t)cases_next();
}

// a point on the circle of this radius (sqrtf is exact on every side, unlike sinf and cosf)
static void cases_circle(float radius, float *x, float *y) {
    *x = cases_between(-radius, radius);
    *y = sqrtf(radius * radius - *x * *x);
    if (cases_next() & 1)
        *y = -*y;
}

// ---- PM_Accelerate

static void cases_one_accelerate(int place, uint8_t flags, float velocity_x, float velocity_y, float wish_speed,
                                 float acceleration) {
    char line[200];
    float direction[3], got_wish_speed = 0, got_acceleration = 0;
    // one after the other: the order of an initializer list's calls is the compiler's
    direction[0] = cases_between(-1, 1);
    direction[1] = cases_between(-1, 1);
    direction[2] = cases_odd(cases_between(-1, 1));
    int copy = 9;
    side_accelerate(place, flags, velocity_x, velocity_y, direction, wish_speed, acceleration, &got_wish_speed,
                    &got_acceleration, &copy);
    snprintf(line, sizeof(line), "a %d %02x %08x %08x %08x %08x -> %08x %08x %d", place, flags,
             cases_bits(velocity_x), cases_bits(velocity_y), cases_bits(wish_speed), cases_bits(acceleration),
             cases_bits(got_wish_speed), cases_bits(got_acceleration), copy);
    cases_line(line);
}

static void cases_accelerate(void) {
    // the speed of 150 from both sides, on either axis, both, and on the circle around it
    const float speeds[][2] = {
        { 0, 0 }, { 150, 0 }, { 0, 150 }, { -150, 0 }, { 0, -150 }, { 90, 120 }, { 120, -90 }, { -90, -120 },
        { 149.99998f, 0 }, { 150.00002f, 0 }, { 0, -149.99998f }, { 0, 150.00002f }, { 106.06601f, 106.06602f },
        { 106.06601f, 106.06601f }, { 106.06602f, 106.06602f }, { 1, 149.99998f }, { 149.9f, 5.4f }, { 400, 0 },
        { cases_from_bits(0x7fc00000), 0 }, { 0, cases_from_bits(0x7fc00000) }, { cases_from_bits(0x7f800000), 0 },
        { 3.0e19f, 0 }, { 1.0e-30f, 1.0e-30f }, { -0.0f, -0.0f },
    };
    const float pairs[][2] = { { 190, 1 }, { 127.5f, 2 }, { 0, 0.25f }, { 320, 9 }, { -5, 12 }, { 50, 19 } };
    for (int place = 0; place < 3; place++)
        for (size_t s = 0; s < sizeof(speeds) / sizeof(speeds[0]); s++)
            for (int f = 0; f < 4; f++) {
                static const uint8_t flags[] = { 0x00, 0x7f, 0x80, 0xff };
                size_t p = (s + (size_t)f) % (sizeof(pairs) / sizeof(pairs[0]));
                cases_one_accelerate(place, flags[f], speeds[s][0], speeds[s][1], pairs[p][0], pairs[p][1]);
            }
    for (int i = 0; i < 400; i++) {
        float x, y;
        cases_circle(150, &x, &y);
        cases_one_accelerate(0, 0, x, y, 190, 1);
    }
    // what the hook passes on: the wish speeds and accelerations the game uses, and odd ones
    const float accelerations[] = { 1, 2, 0.25f, 9, 12, 19, 10, 4.5f };
    for (int i = 0; i < 4000; i++) {
        int place = (int)(cases_next() % 3);
        float x = cases_odd(cases_between(-300, 300)), y = cases_odd(cases_between(-300, 300));
        float wish_speed = cases_odd(cases_between(-50, 450));
        float acceleration = cases_next() % 3 == 0 ? cases_odd(cases_between(0, 25))
                                                   : accelerations[cases_next() % (sizeof(accelerations) / 4)];
        cases_one_accelerate(place, cases_flags(), x, y, wish_speed, acceleration);
    }
}

// ---- PM_Friction

static void cases_one_friction(int which, int in_air, uint8_t flags, float velocity_x, float velocity_y,
                               uint64_t now) {
    char line[200];
    float friction = side_friction(which, in_air, flags, velocity_x, velocity_y, now);
    snprintf(line, sizeof(line), "f %d %d %02x %08x %08x %016llx -> %08x", which, in_air, flags,
             cases_bits(velocity_x), cases_bits(velocity_y), (unsigned long long)now, cases_bits(friction));
    cases_line(line);
}

// in the air at one time, then on the ground at each of the ms after it
static void cases_landing(int which, uint8_t flags, float x, float y, uint64_t in_air, int up_to) {
    cases_one_friction(which, 1, flags, x, y, in_air);
    for (int ms = 0; ms <= up_to; ms++)
        cases_one_friction(which, 0, flags, x, y, in_air + (uint64_t)ms);
}

static void cases_friction(void) {
    // never in the air: the game's friction, whatever the rest
    cases_one_friction(2, 0, 0, 300, 0, 1000);
    cases_one_friction(2, 0, 0, 300, 0, 1001);
    // a landing: no friction up to 67 ms after the last time in the air, from both sides
    cases_landing(0, 0x00, 250, 0, 5000, 70);
    cases_landing(1, 0x20, 0, -250, 6000, 70);
    // the speed of 100 from both sides, on either axis, both, and what isn't a number; sliding
    const float speeds[][2] = {
        { 100, 0 }, { 0, 100 }, { -100, 0 }, { 60, 80 }, { -80, 60 }, { 99.99999f, 0 }, { 100.00001f, 0 },
        { 70.71068f, 70.71068f }, { 70.71067f, 70.71067f }, { 70.710686f, 70.710686f }, { 0, 0 }, { 1000, 1000 },
        { cases_from_bits(0x7fc00000), 0 }, { 0, cases_from_bits(0xffc00000) }, { cases_from_bits(0x7f800000), 0 },
        { 3.0e19f, 0 }, { 5, 99.9f },
    };
    uint64_t now = 20000;
    for (size_t s = 0; s < sizeof(speeds) / sizeof(speeds[0]); s++)
        for (int f = 0; f < 4; f++) {
            static const uint8_t flags[] = { 0x00, 0x7f, 0x80, 0xff };
            cases_one_friction(0, 1, flags[f], speeds[s][0], speeds[s][1], now);
            cases_one_friction(0, 0, flags[f], speeds[s][0], speeds[s][1], now + 30);
            now += 1000;
        }
    for (int i = 0; i < 300; i++) {
        float x, y;
        cases_circle(100, &x, &y);
        cases_one_friction(1, 1, 0, x, y, now);
        cases_one_friction(1, 0, 0, x, y, now + 10);
        now += 500;
    }
    // the clock going round: in the air shortly before, on the ground shortly after
    cases_landing(0, 0, 300, 0, 0xffffffffffffffe0ULL, 70);
    cases_landing(1, 0, 300, 0, 0xffffffffffffffffULL, 70);
    // a clock behind the time in the air
    cases_one_friction(0, 1, 0, 300, 0, 90000);
    cases_one_friction(0, 0, 0, 300, 0, 89999);
    cases_one_friction(0, 0, 0, 300, 0, 90000 - 67);
    // and three player states at once, in the air and not, over a while
    for (int walk = 0; walk < 2; walk++) {
        now = walk == 0 ? 1000000 : 0xfffffffffffff000ULL;
        for (int i = 0; i < 6000; i++) {
            uint32_t step = cases_next() % 20;
            now += step < 14 ? cases_next() % 25 : step < 18 ? 55 + cases_next() % 20 : (uint64_t)0 - (cases_next() % 8);
            int which = (int)(cases_next() % 3), in_air = cases_next() % 5 < 2;
            float x = cases_odd(cases_between(-220, 220)), y = cases_odd(cases_between(-220, 220));
            cases_one_friction(which, in_air, cases_flags(), x, y, now);
        }
    }
}

static void cases_run(void) {
    cases_seed = 0x6a756d70; // "jump"
    cases_accelerate();
    cases_friction();
}
