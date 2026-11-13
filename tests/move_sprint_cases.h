// Made-up cases for two of aae_move_sprint.c's rules, the same ones for T7Overcharged's own functions and for
// BO3MacFix's, so that what comes out can be compared byte for byte:
//   T7O 0x1800f2af0, its omni-movement wrapper of PM_UpdateSprint(pmove_t *, pml_t *): every forward and side stick,
//     with buttons with and without the sprint key and the sprint button, with its dvar on, off and not there, and a
//     made-up sprint decision that notes what it was given and then changes the fields T7Overcharged puts back;
//   T7O 0x1800f2acc, its BG_GetSprintStrafeSpeedScale: dvar values around the game's 0.667, the odd ones (what isn't
//     a number, infinities, zeros, the smallest) and every 65521st bit pattern.
// A line goes out for each case, or for each forward stick with what all side sticks gave (a 64-bit sum of them).
//
// T7Overcharged's side isn't part of the tests: wip/agents/movea/wine (not in the repository) maps its DLL under Wine
// without running anything of it and calls the two functions by address (see compare.sh there). What that gave is in
// test_aae_move_sprint.c as a line count and a checksum.
//
// Whoever includes this gives the two side_ functions and cases_line.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// both sides are to make the same cases: no multiplication and addition folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Cases_No_Dvar (-1)
static void cases_line(const char *line);
// runs the omni-movement wrapper on the move pm with the dvar t7o_omnimovement as dvar says (Cases_No_Dvar: there is
// none; otherwise its byte), decision standing for the game's PM_UpdateSprint
static void side_omni(uint8_t *pm, uint8_t *pml, int dvar, void (*decision)(uint8_t *pm, uint8_t *pml));
// T7Overcharged's strafe speed scale while the dvar t7o_strafe_speed_scale holds this value
static float side_strafe(float dvar_value);

#define Cases_Pm_Size 0x60

static uint64_t cases_sum(uint64_t sum, const void *data, size_t size) {
    for (size_t i = 0; i < size; i++)
        sum = (sum ^ ((const uint8_t *)data)[i]) * 0x100000001b3ULL;
    return sum;
}
static uint32_t cases_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}
static float cases_from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

// ---- omni-movement

// what the made-up decision was given
static struct {
    int calls;
    int pml_right; // it got the pml it was meant to
    uint8_t forward, right;
    uint32_t buttons;
} cases_seen;
static uint8_t *cases_pml;

// stands for the game's sprint decision: notes what it was given, then changes the forward and side stick, the
// buttons and a byte nobody puts back, as a decision might
static void cases_decision(uint8_t *pm, uint8_t *pml) {
    uint32_t buttons;
    cases_seen.calls++;
    cases_seen.pml_right = pml == cases_pml;
    cases_seen.forward = pm[0x40];
    cases_seen.right = pm[0x41];
    memcpy(&cases_seen.buttons, pm + 0xc, sizeof(cases_seen.buttons));
    pm[0x40] = (uint8_t)(pm[0x40] * 3 + 1);
    pm[0x41] = (uint8_t)(pm[0x41] ^ 0xa5);
    buttons = cases_seen.buttons * 2654435761u + 7;
    memcpy(pm + 0xc, &buttons, sizeof(buttons));
    pm[0x20] ^= 0xff;
}

static void cases_omni_one(uint8_t pm[Cases_Pm_Size], int dvar, uint32_t buttons, int forward, int right) {
    for (int i = 0; i < Cases_Pm_Size; i++)
        pm[i] = (uint8_t)(i * 37 + 11);
    pm[0x40] = (uint8_t)forward;
    pm[0x41] = (uint8_t)right;
    memcpy(pm + 0xc, &buttons, sizeof(buttons));
    memset(&cases_seen, 0, sizeof(cases_seen));
    side_omni(pm, cases_pml, dvar, cases_decision);
}

static void cases_omni(void) {
    static const int dvars[] = { Cases_No_Dvar, 0, 1, 0x80 };
    static const uint32_t patterns[] = { 0, 1u << 18, 1u << 30, 1u << 18 | 1u << 30, 0xffffffffu,
                                         0xffffffffu & ~(1u << 18), 0xffffffffu & ~(1u << 30), 0x8afc1234u,
                                         0x0b3d5e7fu };
    static uint8_t pml[0x80];
    uint8_t pm[Cases_Pm_Size];
    char line[200];
    cases_pml = pml;
    for (size_t d = 0; d < sizeof(dvars) / sizeof(dvars[0]); d++) {
        for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
            for (int forward = -128; forward <= 127; forward++) {
                uint64_t sum = 0xcbf29ce484222325ULL;
                for (int right = -128; right <= 127; right++) {
                    uint32_t after;
                    uint8_t seen[8];
                    cases_omni_one(pm, dvars[d], patterns[p], forward, right);
                    memcpy(&after, pm + 0xc, sizeof(after));
                    seen[0] = (uint8_t)cases_seen.calls, seen[1] = (uint8_t)cases_seen.pml_right;
                    seen[2] = cases_seen.forward, seen[3] = cases_seen.right;
                    memcpy(seen + 4, &cases_seen.buttons, sizeof(cases_seen.buttons));
                    sum = cases_sum(sum, seen, sizeof(seen));
                    sum = cases_sum(sum, pm, sizeof(pm));
                    if (right == -128 || right == 0 || right == 127 || right == forward || right == -forward) {
                        snprintf(line, sizeof(line),
                                 "omni dvar %d buttons %08x forward %d right %d: saw %d %d %08x (%d calls, pml %d), "
                                 "after %d %d %08x %02x",
                                 dvars[d], patterns[p], forward, right, (int8_t)cases_seen.forward,
                                 (int8_t)cases_seen.right, cases_seen.buttons, cases_seen.calls, cases_seen.pml_right,
                                 (int8_t)pm[0x40], (int8_t)pm[0x41], after, pm[0x20]);
                        cases_line(line);
                    }
                }
                snprintf(line, sizeof(line), "omni dvar %d buttons %08x forward %d, every side stick: %08x%08x",
                         dvars[d], patterns[p], forward, (uint32_t)(sum >> 32), (uint32_t)sum);
                cases_line(line);
            }
        }
    }
}

// ---- the strafe speed scale

static void cases_strafe_one(uint32_t bits) {
    char line[80];
    snprintf(line, sizeof(line), "strafe %08x: %08x", bits, cases_bits(side_strafe(cases_from_bits(bits))));
    cases_line(line);
}

static void cases_strafe(void) {
    static const uint32_t odd[] = {
        0x00000000, 0x80000000,                         // zeros
        0x00000001, 0x007fffff, 0x80000001, 0x00800000, // the smallest
        0x3f800000, 0xbf800000, 0x3f666666, 0x3f7851ec, // 1, -1, 0.9, 0.97 (the dvar's top)
        0x3f2ac083, 0xbf2ac083, 0x3f2ac082, 0x3f2ac084, // 0.667 and its neighbours
        0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, // the largest, infinities
        0x7fc00000, 0xffc00000, 0x7f800001, 0xff800001, 0x7fffffff, 0x7fa00000, // what isn't a number
    };
    for (size_t i = 0; i < sizeof(odd) / sizeof(odd[0]); i++)
        cases_strafe_one(odd[i]);
    for (int32_t step = -300; step <= 300; step++)
        cases_strafe_one(0x3f2ac083u + (uint32_t)step);
    for (uint64_t bits = 0; bits <= 0xffffffffULL; bits += 65521)
        cases_strafe_one((uint32_t)bits);
}

static void cases_run(void) {
    cases_omni();
    cases_strafe();
}
