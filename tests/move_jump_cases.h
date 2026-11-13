// Made-up cases for aae_move_jump.c: player states going through jumpslowdown's six places, the same ones for
// T7Overcharged's own functions and for BO3MacFix's, so that what comes out can be compared bit for bit. A line goes
// out for each case: what the function gave, and the player state afterwards (every byte of it, as a hash, and the
// fields that matter as bits).
//
// T7Overcharged's side isn't part of the tests: wip/windows/t7o (not in the repository) maps its DLL under Wine
// without running anything of it and calls its functions by address, with the game's functions they wrap being the
// made-up ones below (see compare_move_jump.sh there). What that gave is in test_aae_move_jump.c as a line count and
// a checksum.
//
// Whoever includes this gives the side_ functions and cases_line, with jumpslowdown on.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// every side is to make the same cases: no multiplication and addition folded into one operation
#pragma STDC FP_CONTRACT OFF

static void cases_line(const char *line);
// BG_GetViewDip's replacement: the camera's dip
static int side_view_dip(uint8_t *ps, float fall_height);
// Jump_SlowDown's replacement
static void side_slow_down(uint8_t *ps);
// Jump_ReduceFriction's replacement: the friction multiplier
static float side_reduce_friction(uint8_t *ps);
// the landing with T7Overcharged's wrapper around cases_game_crash_land
static void side_crash_land(uint8_t *pm, uint8_t *pml);
// Jump_Start with T7Overcharged's wrapper, whose clamp of the horizontal speed is cases_game_clamp with cases_cap
static void side_jump_start(uint8_t *pm, uint8_t *pml, float height);

#define Cases_Ps_Size 0x1000
#define Cases_Expected_Lines 2200
#define Cases_Expected_Hash 0xb527de3066ae448cull // what T7Overcharged's side gave (2026-11-13), see above

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
static float cases_between(float low, float high) {
    return low + (high - low) * (float)(cases_next() & 0xffffff) / 16777216.0f;
}
// a float that is sometimes an odd one
static float cases_odd(float usual) {
    switch (cases_next() % 40) {
    case 0: return cases_from_bits(0x7fc00000);
    case 1: return cases_from_bits(0xffc12345);
    case 2: return cases_from_bits(0x7f800000);
    case 3: return cases_from_bits(0xff800000);
    case 4: return cases_from_bits(0x80000000);
    case 5: return 0.0f;
    case 6: return cases_from_bits(0x00000001);
    case 7: return 3.0e38f;
    default: return usual;
    }
}
static uint64_t cases_hash_of(const uint8_t *data, size_t size) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++)
        h = (h ^ data[i]) * 0x100000001b3ull;
    return h;
}
static void cases_fill(uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i++)
        data[i] = (uint8_t)cases_next();
}
static uint32_t cases_u32_at(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static uint64_t cases_u64_at(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static void cases_put_u32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}
static void cases_put_u64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, sizeof(v));
}

// the made-up game: a pmove_t whose first field is the player state, a pml_t, and the game's landing and clamp
static uint8_t cases_ps[Cases_Ps_Size], cases_pm[0x300], cases_pml[0x100];
static float cases_cap;
// how often the game's landing was called (the game's clamp isn't counted: T7Overcharged's side leaves it out where
// the Mac's runs it with an infinite cap, which does nothing)
static int cases_crash_lands;

// The game's landing, made up: what it does depends on the pml_t's byte 0 like the real one's ways do. Way 0 is the
// early return (nothing changes), 1 the 0.67 of an ordinary landing, 2 a damage landing (pm_time and flag 24, the
// velocity scaled), 3 velocity set to odd values, and it always writes the landing's time and a flag elsewhere.
static void cases_game_crash_land(uint8_t *pm, uint8_t *pml) {
    uint8_t *ps;
    memcpy(&ps, pm, sizeof(ps));
    cases_crash_lands++;
    float v[3];
    memcpy(v, ps + 0x3c, sizeof(v));
    switch (pml[0] & 3) {
    case 0:
        return;
    case 1:
        v[0] = v[0] * 0.67f, v[1] = v[1] * 0.67f, v[2] = v[2] * 0.67f;
        break;
    case 2: {
        // finite and not 0, so that nothing makes a new not-a-number: x86 and arm64 give such ones other bits
        static const float scales[] = { 0.5f, 0.2f, 0.35f, 1.25f };
        float scale = scales[pml[4] & 3];
        v[0] = v[0] * scale, v[1] = scale * v[1], v[2] = v[2] * scale;
        cases_put_u32(ps + 0x24, cases_u32_at(pml + 8));
        cases_put_u64(ps + 0x10, cases_u64_at(ps + 0x10) | (1ull << 24));
        break;
    }
    default:
        v[0] = cases_from_bits(cases_u32_at(pml + 12)), v[1] = cases_from_bits(cases_u32_at(pml + 16));
        v[2] = cases_from_bits(cases_u32_at(pml + 20));
        break;
    }
    memcpy(ps + 0x3c, v, sizeof(v));
    cases_put_u32(ps + 0x86c, cases_u32_at(pm + 8));
    ps[0x908] = (uint8_t)(ps[0x908] & 0xf9);
}

// The game's cap of a jump's horizontal speed (the Mac's sums): above the cap, the speed is scaled to it. An infinite
// speed is left alone here (the game would make it not a number, with other bits on x86 than on arm64).
static void cases_game_clamp(uint8_t *pm, float cap) {
    uint8_t *ps;
    memcpy(&ps, pm, sizeof(ps));
    float v[2];
    memcpy(v, ps + 0x3c, sizeof(v));
    float squared = v[1] * v[1] + v[0] * v[0];
    if (squared > cap * cap && isfinite(squared)) {
        float speed = sqrtf(squared);
        float scale = speed <= 0.0f ? 1.0f : 1.0f / speed;
        v[0] = v[0] * scale * cap, v[1] = v[1] * scale * cap;
        memcpy(ps + 0x3c, v, sizeof(v));
    }
}

// The rest of the game's Jump_Start, made up: what it does before and after the clamp to the player state (not to
// its flag 7, which nothing between Jump_Start's start and its clamp changes in the game either).
static void cases_game_jump_before_clamp(uint8_t *pm, uint8_t *pml, float height) {
    uint8_t *ps;
    memcpy(&ps, pm, sizeof(ps));
    float up = sqrtf(height * 2.0f * 800.0f);
    memcpy(ps + 0x44, &up, sizeof(up));
    cases_put_u32(ps + 0x110, cases_u32_at(pm + 8));
    pml[0x2c] = 0;
}
static void cases_game_jump_after_clamp(uint8_t *pm) {
    uint8_t *ps;
    memcpy(&ps, pm, sizeof(ps));
    cases_put_u64(ps + 0x10, (cases_u64_at(ps + 0x10) & ~0x3040000ull) | 0x40000);
    cases_put_u32(ps + 0x24, 0);
}

// a player state with odd values where the places look
static void cases_new_state(void) {
    cases_fill(cases_ps, sizeof(cases_ps));
    cases_fill(cases_pm, sizeof(cases_pm));
    cases_fill(cases_pml, sizeof(cases_pml));
    uint8_t *ps = cases_ps;
    memcpy(cases_pm, &ps, sizeof(ps));
    static const int32_t times[] = { 0, 1, -1, 900, 901, 999, 1000, 1001, 1002, 2000, 0x7fffffff, (int32_t)0x80000000 };
    uint32_t pick = cases_next() % 16;
    int32_t time = pick < 12 ? times[pick] : (int32_t)(cases_next() % 3000) - 500;
    cases_put_u32(ps + 0x24, (uint32_t)time);
    uint64_t flags = (uint64_t)cases_next() << 32 | cases_next();
    switch (cases_next() % 4) {
    case 0: flags &= ~(1ull << 7); break;
    case 1: flags |= 1ull << 7; break;
    case 2: flags = (cases_next() & 1) ? 0x80 : 0x40000; break;
    default: break;
    }
    cases_put_u64(ps + 0x10, flags);
    float v[3] = { cases_odd(cases_between(-600.0f, 600.0f)), cases_odd(cases_between(-600.0f, 600.0f)),
                   cases_odd(cases_between(-900.0f, 400.0f)) };
    memcpy(ps + 0x3c, v, sizeof(v));
    cases_cap = cases_odd(cases_between(100.0f, 400.0f));
}

static void cases_say(const char *what, uint32_t result) {
    char line[300];
    const uint8_t *ps = cases_ps;
    snprintf(line, sizeof(line), "%s %08x: flags %016llx time %08x v %08x %08x %08x z0 %08x ps %016llx pm %016llx "
             "pml %016llx landings %d", what, result, (unsigned long long)cases_u64_at(ps + 0x10),
             cases_u32_at(ps + 0x24), cases_u32_at(ps + 0x3c), cases_u32_at(ps + 0x40), cases_u32_at(ps + 0x44),
             cases_u32_at(ps + 0x114), (unsigned long long)cases_hash_of(ps, Cases_Ps_Size),
             (unsigned long long)cases_hash_of(cases_pm + 8, sizeof(cases_pm) - 8),
             (unsigned long long)cases_hash_of(cases_pml, sizeof(cases_pml)), cases_crash_lands);
    cases_line(line);
}

static void cases_run(void) {
    cases_seed = 0x6a756d70ull; // "jump"
    for (int i = 0; i < 440; i++) {
        cases_new_state();
        float fall = cases_odd(cases_between(-100.0f, 600.0f));
        int dip = side_view_dip(cases_ps, fall);
        cases_say("dip", (uint32_t)dip);

        cases_new_state();
        side_slow_down(cases_ps);
        cases_say("slowdown", 0);

        cases_new_state();
        float multiplier = side_reduce_friction(cases_ps);
        cases_say("friction", cases_bits(multiplier));

        cases_new_state();
        cases_crash_lands = 0;
        side_crash_land(cases_pm, cases_pml);
        cases_say("landing", (uint32_t)cases_pml[0] & 3);

        cases_new_state();
        side_jump_start(cases_pm, cases_pml, cases_between(30.0f, 70.0f));
        cases_say("jump", (uint32_t)(cases_u64_at(cases_ps + 0x10) >> 7) & 1);
    }
}
