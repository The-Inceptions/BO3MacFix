// Made-up cases for the client's side of a shot in aae_freeaim.c (the plugin's client_endpos): queued shots and the
// turned shots kept for their tracers, the same ones for the plugin's own function that finds the kept shot a queued
// one is (FUN_180023bd0) and for BO3MacFix's, so that what comes out can be compared bit for bit. A line goes out for
// each case.
//
// The plugin's side isn't part of the tests: wip/agents/endpos2/compare (not in the repository) maps AAEUnified.dll
// under Wine without running anything of it and calls the function by address. What that gave is in
// test_aae_freeaim.c as a line count and a checksum.
//
// Whoever includes this gives side_match and endpos_cases_line.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// every side is to make the same cases: no multiplication and addition folded into one operation
#pragma STDC FP_CONTRACT OFF

// one of the 32 places of a local client's ring of kept shots
typedef struct {
    float start[3], flash[3], target[3];
    uint64_t made_at;
    int used;
} endpos_case_shot;
typedef struct {
    endpos_case_shot shots[32];
    uint32_t count;
    int pending;
} endpos_case_ring;

// A client's shot from the second call in the client's fire function: everything the plugin's hook (FUN_18001eff0)
// looks at, for both local clients where it is theirs
typedef struct {
    uint64_t now;
    int frame;               // 1: the fire function's frame with the shooting entity in it; 0: no frame; 2: no entity
    uint32_t number;         // the shooting entity's number
    int seen[2];             // the crosshair code was at work for the local client (its cg_t is known)
    uint32_t cg_number[2];   // the client number in the local client's cg_t
    int on[2];               // free aim is on for it
    float start[3], directions[9];
    int has_target[2];       // the point under the crosshair can be read
    int32_t target[2][3];    // whole numbers, which read back exactly
    int tag_ready;           // the game has made the name "tag_flash"
    int has_model[2], has_tag[2];
    float flash[2][3];       // where the tag is on the local client's model of the entity
    float cache[2][3];       // where it was found last, and when
    uint64_t cache_at[2];
    uint32_t count[2];       // how many shots were kept so far
} endpos_shot_case;
// what came out: the directions the game's function was called with, and what the plugin kept
typedef struct {
    int calls;
    float directions[9];
    uint32_t count[2];
    int pending[2];
    endpos_case_shot kept[2]; // the place count - 1 of each client's ring
    float cache[2][3];
    uint64_t cache_at[2];
} endpos_shot_result;

static void endpos_cases_line(const char *line);
// The plugin's hook for the client's copy of a shot, with everything as the case has it.
static void side_client_shot(const endpos_shot_case *c, endpos_shot_result *r);
// A queued shot of local client 0 (fired from origin, starting at start, its tracer from muzzle), at now, against the
// kept shots in ring: whether one was found. ring (used, pending) and the three points are as the side left them.
static int side_match(endpos_case_ring *ring, uint64_t now, float origin[3], float start[3], float muzzle[3]);

static uint64_t endpos_cases_seed;
static uint32_t endpos_cases_next(void) {
    endpos_cases_seed = endpos_cases_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(endpos_cases_seed >> 33);
}
static float endpos_cases_between(float low, float high) {
    return low + (high - low) * (float)(endpos_cases_next() & 0xffffff) / 16777216.0f;
}
static uint32_t endpos_cases_bits(float value) {
    uint32_t b;
    memcpy(&b, &value, sizeof(b));
    return b;
}
static float endpos_cases_from_bits(uint32_t b) {
    float value;
    memcpy(&value, &b, sizeof(value));
    return value;
}
// now and then something that isn't a usual coordinate
static float endpos_cases_odd(float usual) {
    switch (endpos_cases_next() % 80) {
    case 0: return endpos_cases_from_bits(0x7fc00000);
    case 1: return endpos_cases_from_bits(0x7f800000);
    case 2: return endpos_cases_from_bits(0xff800000);
    case 3: return 10000000.0f;
    case 4: return -10000000.0f;
    case 5: return 9999999.0f;
    case 6: return 3.0e7f;
    default: return usual;
    }
}
// a point this far from p along a made-up direction, or exactly along one axis
static void endpos_cases_away(const float p[3], float distance, float out[3]) {
    int axis = (int)(endpos_cases_next() % 4);
    if (axis < 3) {
        memcpy(out, p, 3 * sizeof(float));
        out[axis] = p[axis] + ((endpos_cases_next() & 1) ? distance : -distance);
        return;
    }
    float d[3] = { endpos_cases_between(-1, 1), endpos_cases_between(-1, 1), endpos_cases_between(-1, 1) };
    float length = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (length < 0.01f)
        d[0] = 1, d[1] = 0, d[2] = 0, length = 1;
    // (no square root, to keep the sums the same everywhere: the distance is only about right)
    float scale = distance / (length > 1 ? length : 1);
    for (int i = 0; i < 3; i++)
        out[i] = p[i] + d[i] * scale;
}
// a distance near one of the bounds the plugin tests (squared: 64, 16384; and 1 and 131072 for the point)
static float endpos_cases_distance(float bound) {
    switch (endpos_cases_next() % 6) {
    case 0: return bound;
    case 1: return bound * 0.99999f;
    case 2: return bound * 1.00001f;
    case 3: return endpos_cases_between(0, bound);
    case 4: return endpos_cases_between(bound, bound * 3);
    default: return endpos_cases_between(0, bound * 1.2f);
    }
}

static void endpos_cases_match_one(endpos_case_ring *ring, uint64_t now, float origin[3], float start[3],
                                   float muzzle[3]) {
    char line[600];
    int found = side_match(ring, now, origin, start, muzzle);
    uint32_t used = 0;
    for (int i = 0; i < 32; i++)
        if (ring->shots[i].used)
            used |= 1u << i;
    snprintf(line, sizeof(line), "m %d %08x %d %08x %08x %08x %08x %08x %08x %08x %08x %08x", found, used,
             ring->pending, endpos_cases_bits(origin[0]), endpos_cases_bits(origin[1]), endpos_cases_bits(origin[2]),
             endpos_cases_bits(start[0]), endpos_cases_bits(start[1]), endpos_cases_bits(start[2]),
             endpos_cases_bits(muzzle[0]), endpos_cases_bits(muzzle[1]), endpos_cases_bits(muzzle[2]));
    endpos_cases_line(line);
}

// one kept shot that fits a queued shot from origin with its tracer from muzzle, made at now - age
static void endpos_cases_fitting(endpos_case_shot *shot, const float origin[3], const float muzzle[3], uint64_t now) {
    endpos_cases_away(origin, endpos_cases_between(0, 7.9f), shot->start);
    float middle[3] = { (origin[0] + muzzle[0]) * 0.5f, (origin[1] + muzzle[1]) * 0.5f,
                        (origin[2] + muzzle[2]) * 0.5f };
    endpos_cases_away(middle, endpos_cases_between(0, 40), shot->flash);
    endpos_cases_away(shot->flash, endpos_cases_between(2, 5000), shot->target);
    shot->made_at = now - endpos_cases_next() % 500;
    shot->used = 0;
}
// one thing about a kept shot and the queued one changed, so that it fits or not by that alone, mostly at a bound
static void endpos_cases_change(endpos_case_shot *shot, float origin[3], float start[3], float muzzle[3],
                                uint64_t now) {
    float *points[6] = { shot->start, shot->flash, shot->target, origin, start, muzzle };
    switch (endpos_cases_next() % 14) {
    case 0: endpos_cases_away(origin, endpos_cases_distance(8.0f), shot->start); break;
    case 1: endpos_cases_away(muzzle, endpos_cases_distance(128.0f), shot->flash); break;
    case 2: endpos_cases_away(origin, endpos_cases_distance(128.0f), shot->flash); break;
    case 3: endpos_cases_away(origin, endpos_cases_distance(128.0f), muzzle); break;
    case 4: endpos_cases_away(shot->flash, endpos_cases_distance(1.0f), shot->target); break;
    case 5: endpos_cases_away(shot->flash, endpos_cases_distance(131072.0f), shot->target); break;
    case 6:
        switch (endpos_cases_next() % 6) {
        case 0: shot->made_at = 0; break;
        case 1: shot->made_at = now + 1 + endpos_cases_next() % 3; break;
        case 2: shot->made_at = now - 500; break;
        case 3: shot->made_at = now - 501; break;
        case 4: shot->made_at = UINT64_MAX - endpos_cases_next() % 300; break;
        default: shot->made_at = now; break;
        }
        break;
    case 7: shot->used = 1; break;
    case 8: case 9: case 10: {
        // one coordinate of one of the six points that isn't a usual one
        float *p = points[endpos_cases_next() % 6];
        float was = p[endpos_cases_next() % 3];
        p[endpos_cases_next() % 3] = endpos_cases_odd(was);
        static const uint32_t odd[] = { 0x7fc00000, 0x7f800000, 0xff800000, 0x4b189680, 0xcb189680, 0x4b18967f,
                                        0x4bea8000, 0xffc00000 };
        p[endpos_cases_next() % 3] = endpos_cases_from_bits(odd[endpos_cases_next() % 8]);
        break;
    }
    case 11: endpos_cases_away(origin, endpos_cases_between(0, 400), start); break;
    default: break; // as it is
    }
}

static void endpos_cases_matches(void) {
    static const uint32_t counts[] = { 0, 1, 2, 5, 31, 32, 33, 64, 1000, 0xffffffffu, 0xfffffffeu, 0x80000000u };
    endpos_cases_seed = 20261112;
    for (int n = 0; n < 30000; n++) {
        endpos_case_ring ring;
        memset(&ring, 0, sizeof(ring));
        uint64_t now = 100000 + endpos_cases_next() % 100000;
        if (endpos_cases_next() % 50 == 0)
            now = 500 + endpos_cases_next() % 600;
        float origin[3] = { endpos_cases_between(-5000, 5000), endpos_cases_between(-5000, 5000),
                            endpos_cases_between(-500, 500) };
        if (endpos_cases_next() % 8 == 0) // whole numbers, where the bounds come out exactly
            for (int i = 0; i < 3; i++)
                origin[i] = (float)(int)origin[i];
        if (endpos_cases_next() % 40 == 0) // far out, near the bound of 1e7
            origin[endpos_cases_next() % 3] = (endpos_cases_next() & 1) ? 9999990.0f : -9999990.0f;
        float start[3], muzzle[3];
        memcpy(start, origin, sizeof(start));
        endpos_cases_away(origin, endpos_cases_between(0, 60), muzzle);
        ring.count = counts[endpos_cases_next() % (sizeof(counts) / sizeof(counts[0]))];
        if (endpos_cases_next() % 3 == 0)
            ring.count = endpos_cases_next();
        ring.pending = (int)(endpos_cases_next() % 2);
        // a few kept shots, the newest places first mostly; each fits, then has one thing changed or not
        int kept = 1 + (int)(endpos_cases_next() % 4);
        for (int k = 0; k < kept; k++) {
            int slot = (int)((ring.count - 1 - (endpos_cases_next() % 4)) & 31);
            if (endpos_cases_next() % 5 == 0)
                slot = (int)(endpos_cases_next() % 32);
            endpos_case_shot *shot = &ring.shots[slot];
            endpos_cases_fitting(shot, origin, muzzle, now);
            if (endpos_cases_next() % 4 != 0)
                endpos_cases_change(shot, origin, start, muzzle, now);
        }
        endpos_cases_match_one(&ring, now, origin, start, muzzle);
        // and once more with what is left, as the next queued shot like it would find it
        if (endpos_cases_next() % 4 == 0)
            endpos_cases_match_one(&ring, now + endpos_cases_next() % 3, origin, start, muzzle);
    }
}

// ---- the client's copy of a shot (the plugin's FUN_18001eff0)

static void endpos_cases_shot_one(const endpos_shot_case *c) {
    endpos_shot_result r;
    memset(&r, 0, sizeof(r));
    side_client_shot(c, &r);
    char line[1000];
    int at = snprintf(line, sizeof(line), "s %d", r.calls);
    for (int i = 0; i < 9; i++)
        at += snprintf(line + at, sizeof(line) - (size_t)at, " %08x", endpos_cases_bits(r.directions[i]));
    for (int k = 0; k < 2; k++) {
        const endpos_case_shot *shot = &r.kept[k];
        at += snprintf(line + at, sizeof(line) - (size_t)at, " | %u %d %llu", r.count[k], r.pending[k],
                       (unsigned long long)shot->made_at);
        for (int i = 0; i < 3; i++)
            at += snprintf(line + at, sizeof(line) - (size_t)at, " %08x %08x %08x", endpos_cases_bits(shot->start[i]),
                           endpos_cases_bits(shot->flash[i]), endpos_cases_bits(shot->target[i]));
        at += snprintf(line + at, sizeof(line) - (size_t)at, " %08x %08x %08x %llu", endpos_cases_bits(r.cache[k][0]),
                       endpos_cases_bits(r.cache[k][1]), endpos_cases_bits(r.cache[k][2]),
                       (unsigned long long)r.cache_at[k]);
    }
    endpos_cases_line(line);
}

// three directions at right angles, of length 1 (give or take what floats make of that)
static void endpos_cases_directions(float d[9]) {
    static const float straight[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    if (endpos_cases_next() % 3 == 0) {
        // the axes in another order, with signs
        int a = (int)(endpos_cases_next() % 3), b = (a + 1 + (int)(endpos_cases_next() % 2)) % 3;
        float sa = (endpos_cases_next() & 1) ? 1.0f : -1.0f, sb = (endpos_cases_next() & 1) ? 1.0f : -1.0f;
        memset(d, 0, 9 * sizeof(float));
        d[a] = sa, d[3 + b] = sb;
        d[6 + 0] = d[1] * d[5] - d[2] * d[4], d[6 + 1] = d[2] * d[3] - d[0] * d[5];
        d[6 + 2] = d[0] * d[4] - d[1] * d[3];
        return;
    }
    memcpy(d, straight, sizeof(straight));
    // forward anywhere, levelled a bit; right across it; up from both
    float f[3] = { endpos_cases_between(-1, 1), endpos_cases_between(-1, 1), endpos_cases_between(-0.6f, 0.6f) };
    float length = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (length < 0.1f)
        return;
    for (int i = 0; i < 3; i++)
        d[i] = f[i] / length;
    float r[3] = { d[1], -d[0], 0.0f };
    float side = sqrtf(r[0] * r[0] + r[1] * r[1]);
    if (side < 0.1f) {
        memcpy(d, straight, sizeof(straight));
        return;
    }
    for (int i = 0; i < 3; i++)
        d[3 + i] = r[i] / side;
    d[6] = d[4] * d[2] - d[5] * d[1], d[7] = d[5] * d[0] - d[3] * d[2], d[8] = d[3] * d[1] - d[4] * d[0];
}

static void endpos_cases_shots(void) {
    endpos_cases_seed = 4417;
    for (int n = 0; n < 12000; n++) {
        endpos_shot_case c;
        memset(&c, 0, sizeof(c));
        c.now = 100000 + endpos_cases_next() % 100000;
        c.frame = endpos_cases_next() % 25 == 0 ? (int)(endpos_cases_next() % 2) * 2 : 1;
        int whose = endpos_cases_next() % 5 == 0 ? 1 : 0; // the local client that shoots
        c.number = (uint32_t)whose;
        for (int k = 0; k < 2; k++) {
            c.seen[k] = endpos_cases_next() % 15 != 0;
            c.cg_number[k] = (uint32_t)k;
            c.on[k] = endpos_cases_next() % 8 != 0;
        }
        switch (endpos_cases_next() % 12) {
        case 0: c.number = 2 + endpos_cases_next() % 62; break;                       // nobody's here
        case 1: c.number = c.cg_number[whose] = 63; break;
        case 2: c.number = c.cg_number[whose] = 64; break;
        case 3: c.cg_number[0] = c.cg_number[1] = c.number; break;                    // both have the number
        case 4: c.number = c.cg_number[whose] = endpos_cases_next(); break;
        default: break;
        }
        for (int k = 0; k < 2; k++)
            if (!c.seen[k] || c.cg_number[k] > 63) // the rule says no for those whatever else
                c.on[k] = 0;
        // where the shot starts, and from where the point under the crosshair is
        float base[3] = { (float)(int)endpos_cases_between(-5000, 5000), (float)(int)endpos_cases_between(-5000, 5000),
                          (float)(int)endpos_cases_between(-500, 500) };
        if (endpos_cases_next() % 2)
            for (int i = 0; i < 3; i++)
                base[i] = base[i] + endpos_cases_between(0, 1);
        memcpy(c.start, base, sizeof(base));
        if (endpos_cases_next() % 15 == 0) {
            static const uint32_t odd[] = { 0x7fc00000, 0x7f800000, 0x4b189680, 0xcb189680, 0x4b18967f };
            c.start[endpos_cases_next() % 3] = endpos_cases_from_bits(odd[endpos_cases_next() % 5]);
        }
        endpos_cases_directions(c.directions);
        if (endpos_cases_next() % 20 == 0)
            c.directions[endpos_cases_next() % 9] *= 1.06f;
        for (int k = 0; k < 2; k++) {
            c.has_target[k] = endpos_cases_next() % 15 != 0;
            for (int i = 0; i < 3; i++)
                c.target[k][i] = (int32_t)base[i] + (int32_t)(endpos_cases_next() % 4001) - 2000;
            switch (endpos_cases_next() % 25) {
            case 0: for (int i = 0; i < 3; i++) c.target[k][i] = (int32_t)base[i]; break; // (where it starts, maybe)
            case 1: c.target[k][endpos_cases_next() % 3] = 10000000; break;
            case 2: c.target[k][endpos_cases_next() % 3] = 9999999; break;
            case 3: c.target[k][endpos_cases_next() % 3] = -10000000; break;
            default: break;
            }
            c.has_model[k] = endpos_cases_next() % 12 != 0;
            c.has_tag[k] = endpos_cases_next() % 12 != 0;
            endpos_cases_away(c.start, endpos_cases_distance(128.0f), c.flash[k]);
            if (endpos_cases_next() % 20 == 0) {
                static const uint32_t odd[] = { 0x7fc00000, 0xff800000, 0x4b189680, 0x4b18967f };
                c.flash[k][endpos_cases_next() % 3] = endpos_cases_from_bits(odd[endpos_cases_next() % 4]);
            }
            endpos_cases_away(c.start, endpos_cases_distance(128.0f), c.cache[k]);
            switch (endpos_cases_next() % 8) {
            case 0: c.cache_at[k] = 0; break;
            case 1: c.cache_at[k] = c.now - 250; break;
            case 2: c.cache_at[k] = c.now - 251; break;
            case 3: c.cache_at[k] = c.now + 1 + endpos_cases_next() % 5; break;
            case 4: c.cache_at[k] = UINT64_MAX; break;
            default: c.cache_at[k] = c.now - endpos_cases_next() % 300; break;
            }
            if (endpos_cases_next() % 20 == 0)
                c.cache[k][endpos_cases_next() % 3] = endpos_cases_from_bits(0x7fc00000);
            c.count[k] = endpos_cases_next() % 4 == 0 ? endpos_cases_next() : endpos_cases_next() % 40;
        }
        c.tag_ready = endpos_cases_next() % 30 != 0;
        endpos_cases_shot_one(&c);
    }
}

static void endpos_cases_run(void) {
    endpos_cases_matches();
    endpos_cases_shots();
}
