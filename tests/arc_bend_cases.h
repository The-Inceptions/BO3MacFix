// Made-up cases for aae_arc.c's sums: arcs whose first points are bent towards a start, and points to be told usable
// or not, the same ones for the plugin's own functions and for BO3MacFix's, so that what comes out can be compared
// bit for bit. A line goes out for each case.
//
// The plugin's side isn't part of the tests: wip/agents/arc/wine (not in the repository) maps AAEUnified.dll under
// Wine without running anything of it and calls its functions by address. What that gave is in test_aae_arc.c as a
// line count and a checksum.
//
// Whoever includes this gives the functions below and arc_cases_line.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// every side is to make the same cases: no multiplication and addition folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Arc_Cases_Points 61

static void arc_cases_line(const char *line);
// bends the points towards the start: what the bend returned
static int side_bend(float (*points)[3], const float anchor[3], int n);
// whether a point is usable
static int side_usable(const float point[3]);

static uint64_t arc_cases_seed;
static uint32_t arc_cases_next(void) {
    arc_cases_seed = arc_cases_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(arc_cases_seed >> 33);
}
static float arc_cases_between(float low, float high) {
    return low + (high - low) * (float)(arc_cases_next() & 0xffffff) / 16777216.0f;
}
static uint32_t arc_cases_bits(float value) {
    uint32_t b;
    memcpy(&b, &value, sizeof(b));
    return b;
}
static float arc_cases_from_bits(uint32_t b) {
    float value;
    memcpy(&value, &b, sizeof(value));
    return value;
}
static float arc_cases_odd(float usual) {
    switch (arc_cases_next() % 400) {
    case 0: return arc_cases_from_bits(0x7fc00000);
    case 1: return arc_cases_from_bits(0x7f800000);
    case 2: return arc_cases_from_bits(0xff800000);
    case 3: return 1.0e7f;
    case 4: return -9999999.0f;
    case 5: return 3.0e38f;
    default: return usual;
    }
}

// the 64-bit FNV-1a of every point's bits: what a case left in the points
static uint64_t arc_cases_hash(const float (*points)[3], int count) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < 3; c++) {
            uint32_t b = arc_cases_bits(points[i][c]);
            for (int byte = 0; byte < 4; byte++)
                hash = (hash ^ ((b >> (8 * byte)) & 0xff)) * 0x100000001b3ULL;
        }
    }
    return hash;
}

static void arc_cases_bend_one(float (*points)[3], const float anchor[3], int n) {
    char line[200];
    int k = side_bend(points, anchor, n);
    snprintf(line, sizeof(line), "b %d %d %016llx %08x %08x %08x", n, k,
             (unsigned long long)arc_cases_hash((const float (*)[3])points, Arc_Cases_Points),
             arc_cases_bits(points[1][0]), arc_cases_bits(points[1][1]), arc_cases_bits(points[1][2]));
    arc_cases_line(line);
}

// a throw's arc as the game's job lays it out: from a place near the hand, along a speed, falling
static void arc_cases_throw(float (*points)[3], float start[3], int n, int odd) {
    float speed[3] = { arc_cases_between(-900.0f, 900.0f), arc_cases_between(-900.0f, 900.0f),
                       arc_cases_between(-200.0f, 600.0f) };
    float step = arc_cases_between(0.01f, 0.08f), gravity = arc_cases_between(300.0f, 800.0f);
    for (int i = 0; i < Arc_Cases_Points; i++) {
        float t = step * (float)i;
        points[i][0] = start[0] + speed[0] * t;
        points[i][1] = start[1] + speed[1] * t;
        points[i][2] = start[2] + speed[2] * t - 0.5f * gravity * t * t;
        if (odd) {
            points[i][0] = arc_cases_odd(points[i][0]);
            points[i][1] = arc_cases_odd(points[i][1]);
            points[i][2] = arc_cases_odd(points[i][2]);
        }
    }
    (void)n;
}

// arcs within a hair of 10,000,000, where each of the plugin's checks in the bend decides (found by trying)
static const uint32_t arc_cases_edges[][8 * 3 + 3] = {
    { 0xcb18bb3a, 0xc6ee84d1, 0xc5c8c288, 0xcb1883ee, 0xc66601fa, 0xc431a220, 0xcb1846b9, 0x4661959c, 0xc602f9f6,
      0xcb18a902, 0xc6375528, 0x467e51b8, 0xcb189164, 0xcb1873f3, 0x4642776e, 0xcb18ac19, 0xc66eea9a, 0xc5c1f110,
      0xcb188f7f, 0xc64297d2, 0xc63f11e2, 0xcb18ad22, 0x44435d20, 0x457bcc10, 0xcb18649f, 0xcb1862f7, 0x4663e35a },
    { 0xcb186f70, 0xbf1d53a4, 0xcb18967f, 0xcb186f6f, 0xbd804d70, 0xbdfdbaa0, 0xcb18967e, 0xbf536f8c, 0xbef7cc60,
      0xcb186f70, 0x3dc64550, 0x3e65de40, 0xcb186f6f, 0x3cccbc40, 0xbf36b5b0, 0xcb18967f, 0x3da39450, 0xcb18967f,
      0xcb186f6e, 0xcb18967f, 0xcb18967f, 0xcb186f6f, 0xcb18967f, 0xcb18967f, 0xcb186f6f, 0xcb18967f, 0xbe9dc134 },
    { 0xcb18961d, 0xcb18967f, 0xc2a3bda6, 0xcb189614, 0xc2348d84, 0xc01008c0, 0xcb189619, 0x429f5218, 0xc2b659bc,
      0xcb189623, 0x429d25c4, 0x42cabef6, 0xcb18964d, 0xc2e77668, 0xc230bcd0, 0xcb189660, 0xc2df1e44, 0xcb18964b,
      0xcb186f63, 0xcb18961b, 0x41b42990, 0xcb189604, 0xcb18963f, 0x42b387e2, 0xcb189676, 0x418a5120, 0xc1e32fc8 },
    { 0x4b18967f, 0x4b18967f, 0x421367a4, 0x4b189671, 0x3fe19420, 0x41376c38, 0x4b189675, 0xc1992206, 0x41f8cda8,
      0x4b186f83, 0x418ae15a, 0xc1bf3da4, 0x4b186f5c, 0x4b189661, 0x418bd210, 0x4b186f7a, 0x4b18967d, 0x4b18966e,
      0x4b186f72, 0x4b189672, 0x4152f904, 0x4b186f4f, 0x4b18966a, 0x4078d9a0, 0x4b186f57, 0x4b189665, 0x4b18967b },
    { 0xcb18967f, 0xcb18967f, 0xcb18967f, 0xcb27e037, 0xca9609c2, 0x4a7c2ee0, 0xcb026a6a, 0xcaa7e72a, 0xcafe5147,
      0xcb3d93d6, 0x499090c8, 0xca1d77ec, 0xcb0ad041, 0xcb0167d0, 0x4a2f5060, 0xcafd2525, 0xc9f2f9c8, 0x4ad68334,
      0xcae666f8, 0xcad5eedd, 0xc9c178d8, 0xcb87b545, 0xcadaf10d, 0x4a429488, 0xcb7c0c28, 0x4a9255c4, 0xca031ea4 },
    { 0x4b18967f, 0x4946d7e0, 0x4b18967f, 0x4b789aa9, 0xcad78f4a, 0x4b163b48, 0x4b0a46f4, 0x4a8c660c, 0x4affcfc6,
      0x4a0355f4, 0x4a687cb4, 0x4a6b0740, 0x49ed8068, 0x4ab96ea2, 0xca9e9554, 0x4b11c7b6, 0xca958e96, 0x4a527254,
      0x4adbd9f1, 0x4a847e26, 0x4a8cffc9, 0x4b850b96, 0x4a7dfad8, 0x4a1e2608, 0x4a8d3028, 0x4af85a3d, 0x4b08938a },
};

static void arc_cases_bends(void) {
    float points[Arc_Cases_Points][3];
    // made-up throws: the start near where the arc begins, every count of points
    for (int round = 0; round < 40; round++) {
        for (int n = 0; n <= 62; n++) {
            float start[3] = { arc_cases_between(-4000.0f, 4000.0f), arc_cases_between(-4000.0f, 4000.0f),
                               arc_cases_between(-500.0f, 500.0f) };
            arc_cases_throw(points, start, n, round >= 30);
            float anchor[3] = { start[0] + arc_cases_between(-30.0f, 30.0f),
                                start[1] + arc_cases_between(-30.0f, 30.0f),
                                start[2] + arc_cases_between(-30.0f, 10.0f) };
            if (round >= 35) {
                anchor[0] = arc_cases_odd(anchor[0]);
                anchor[1] = arc_cases_odd(anchor[1]);
                anchor[2] = arc_cases_odd(anchor[2]);
            }
            arc_cases_bend_one(points, anchor, n);
        }
    }
    // the start far from the arc, on it, at point k, at the arc's far end, and the arc going back towards it
    for (int round = 0; round < 60; round++) {
        float start[3] = { 10.0f, 20.0f, 30.0f };
        int n = 8 + (int)(arc_cases_next() % 53);
        arc_cases_throw(points, start, n, 0);
        int k = (n + 3) / 4 < 4 ? 4 : (n + 3) / 4 > 16 ? 16 : (n + 3) / 4;
        float anchor[3];
        switch (round % 6) {
        case 0: memcpy(anchor, points[k], sizeof(anchor)); break;
        case 1: memcpy(anchor, points[0], sizeof(anchor)); break;
        case 2:
            anchor[0] = points[k][0] + arc_cases_between(-0.0006f, 0.0006f);
            anchor[1] = points[k][1], anchor[2] = points[k][2];
            break;
        case 3:
            anchor[0] = arc_cases_between(-90000.0f, 90000.0f), anchor[1] = arc_cases_between(-90000.0f, 90000.0f);
            anchor[2] = arc_cases_between(-9000.0f, 9000.0f);
            break;
        case 4: memcpy(anchor, points[n - 1], sizeof(anchor)); break;
        default:
            for (int i = 0; i < Arc_Cases_Points; i++)
                points[i][0] = points[k][0] - (points[i][0] - points[k][0]);
            memcpy(anchor, points[2], sizeof(anchor));
            break;
        }
        arc_cases_bend_one(points, anchor, n);
    }
    // points beyond what the plugin takes, at point k and at point k + 1
    for (int round = 0; round < 24; round++) {
        float start[3] = { 0.0f, 0.0f, 0.0f };
        int n = 20;
        arc_cases_throw(points, start, n, 0);
        static const uint32_t beyond[] = { 0x4b189680, 0x4b18967f, 0xcb189680, 0x7f800000, 0x7fc00000, 0x7f7fffff };
        points[round < 12 ? 5 : 6][round % 3] = arc_cases_from_bits(beyond[(round / 3) % 6]);
        float anchor[3] = { 1.0f, 2.0f, 3.0f };
        arc_cases_bend_one(points, anchor, n);
    }
}

static void arc_cases_edge(void) {
    float points[Arc_Cases_Points][3];
    for (size_t e = 0; e < sizeof(arc_cases_edges) / sizeof(arc_cases_edges[0]); e++) {
        memset(points, 0, sizeof(points));
        float anchor[3];
        for (int c = 0; c < 3; c++)
            anchor[c] = arc_cases_from_bits(arc_cases_edges[e][c]);
        for (int i = 0; i < 8; i++)
            for (int c = 0; c < 3; c++)
                points[i][c] = arc_cases_from_bits(arc_cases_edges[e][3 + 3 * i + c]);
        arc_cases_bend_one(points, anchor, 20);
    }
}

static void arc_cases_usable(void) {
    static const uint32_t special[] = { 0x00000000, 0x80000000, 0x3f800000, 0x4b189680, 0x4b18967f, 0xcb189680,
                                        0xcb18967f, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f7fffff,
                                        0x00000001, 0x7f800001, 0x4b189681 };
    const int count = (int)(sizeof(special) / sizeof(special[0]));
    for (int x = 0; x < count; x++) {
        for (int y = 0; y < count; y += 3) {
            for (int z = 0; z < count; z += 5) {
                float point[3] = { arc_cases_from_bits(special[x]), arc_cases_from_bits(special[y]),
                                   arc_cases_from_bits(special[z]) };
                char line[64];
                snprintf(line, sizeof(line), "u %08x %08x %08x %d", special[x], special[y], special[z],
                         side_usable(point) != 0);
                arc_cases_line(line);
            }
        }
    }
}

static void arc_cases_run(void) {
    arc_cases_seed = 20261112;
    arc_cases_bends();
    arc_cases_edge();
    arc_cases_usable();
}
