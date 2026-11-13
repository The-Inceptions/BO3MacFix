// aae_move_slide.c: T7Overcharged's slide push against the cases it was compared on with T7Overcharged's own code,
// worked cases, the handlers of the five places against a made-up game (with the game's instructions behind each place
// run here in C), what is installed where, and the debug lines the game checks look for.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/aae_move_slide.c"
#include "slide_cases.h"

// ---- what the code asks of the rest of BO3MacFix
// the switches (aae_movement.h), as each test sets them
static bool test_enabled = true, test_started = true;
static bool test_patches[Move_Patch_Count];
bool aae_movement_enabled(void) {
    return test_enabled;
}
bool aae_movement_started(void) {
    return test_started;
}
bool aae_movement_patch_on(move_patch patch) {
    return patch >= 0 && patch < Move_Patch_Count && test_patches[patch];
}
static void switches(bool started, int only) {
    test_started = started;
    for (int i = 0; i < Move_Patch_Count; i++)
        test_patches[i] = only < 0 || i == only;
}
static void all_but(int patch) {
    test_started = true;
    for (int i = 0; i < Move_Patch_Count; i++)
        test_patches[i] = i != patch;
}
// which places the pre-patched executable has stubs for
static bool have_stubs = true;
static uint32_t stub_missing_for;
bool prepatch_has_hook(uint32_t offset) {
    return have_stubs && offset != stub_missing_for;
}
static struct {
    void *address, *replacement, **original;
} hooked[16];
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 16)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    return 0;
}
// what a detour hooks a place with: on x86_64 its thunk, elsewhere the handler itself (detour.h)
#if defined(__x86_64__)
#define DETOUR_ENTRY(name) ((void *)thunk_##name)
#else
#define DETOUR_ENTRY(name) ((void *)handler_##name)
#endif
static bool was_hooked(uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 16; i++)
        if (hooked[i].address == (void *)(0x100000000 + (uint64_t)place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
}

static uint32_t bits_of(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}
static float float_of(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static void lane_set(detour_registers_t *registers, int xmm, float value) {
    memcpy(registers->xmm[xmm], &value, sizeof(value));
}
static float lane(const detour_registers_t *registers, int xmm) {
    float value;
    memcpy(&value, registers->xmm[xmm], sizeof(value));
    return value;
}
static bool same(const detour_registers_t *a, const detour_registers_t *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

// ---- the debug lines, kept
static char said[8192];
__attribute__((format(printf, 1, 2))) static int test_say(const char *format, ...) {
    size_t used = strlen(said);
    va_list arguments;
    va_start(arguments, format);
    int n = vsnprintf(said + used, sizeof(said) - used, format, arguments);
    va_end(arguments);
    return n;
}

// ---- the made-up game's PM_Accelerate and PM_DoSlideAdjustments
static struct {
    int calls;
    uint8_t *ps, *pml;
} game_slide;
static void game_slide_adjustments(uint8_t *ps, uint8_t *pml) {
    game_slide.calls++;
    game_slide.ps = ps, game_slide.pml = pml;
}

// T7Overcharged's constants as they are in its DLL, and what the game has as floats
static void test_constants(void) {
    uint64_t third, share;
    double t = Flagged_Third, s = Flagged_Velocity_Share;
    memcpy(&third, &t, sizeof(third));
    memcpy(&share, &s, sizeof(share));
    CHECK(third == 0x3fd54fdf3b645a1dULL && share == 0x3f7b4e81be11bb42ULL);
    CHECK(bits_of((float)Flagged_Third) == 0x3eaa7efa && bits_of((float)Flagged_Velocity_Share) == 0x3bda740e);
    CHECK(Surface_Wish_Speed == 300.0f && Surface_Acceleration == 2.0f && Flagged_Wish_Speed == 1000.0f &&
          Flagged_Acceleration == 0.25f && Undo_Above == 90000.0f);
}

// ---- the cases compared with T7Overcharged's own function (tests/slide_cases.h)
//
// T7Overcharged's side gave these (wip/agents/movec/wine/compare.sh, 2026-11-13): 102116 lines, the same as
// BO3MacFix's x86_64 and arm64 builds; with every NaN's bits, the same as the x86_64 build. This test is built for
// x86_64 as well (tests/test_aae_move_slide_x86), the game's own code, and there it compares the NaNs' bits too: which
// NaN comes out where two meet is x86's rule, and arm64 makes other NaNs than x86 from numbers (0 times infinity).
#define T7O_Lines 102116
#if defined(__x86_64__)
#define T7O_Exact 1
#define T7O_Checksum 0x80bd32c144c9a0faULL
#else
#define T7O_Exact 0
#define T7O_Checksum 0xef2ee65615413629ULL
#endif
static struct {
    uint64_t checksum;
    unsigned lines;
} cases_out;
static void cases_line(const char *line) {
    for (const char *c = line; *c != '\0'; c++)
        cases_out.checksum = (cases_out.checksum ^ (uint8_t)*c) * 0x100000001b3ULL;
    cases_out.checksum = (cases_out.checksum ^ '\n') * 0x100000001b3ULL;
    cases_out.lines++;
}
static void side_push(uint8_t *ps, uint8_t *pml) {
    fake_PM_DoSlideAdjustments(ps, pml);
}
static void test_like_t7o(void) {
    switches(true, Move_SlidePreserveMomentum);
    PM_Accelerate = cases_accelerate;
    cases_exact = T7O_Exact;
    cases_out.checksum = 0xcbf29ce484222325ULL, cases_out.lines = 0;
    cases_run();
    CHECK(cases_out.lines == T7O_Lines);
    CHECK(cases_out.checksum == T7O_Checksum);
    if (cases_out.checksum != T7O_Checksum)
        printf("test_aae_move_slide: the cases' checksum is %#018llx, not T7Overcharged's\n",
               (unsigned long long)cases_out.checksum);
}

// ---- worked cases: T7Overcharged's sums done by hand (wip/agents/movec/handcases.py, floats rounded after each step)
static void push_of(bool surface, uint64_t flags, int32_t flagged, uint32_t x, uint32_t y, float vx, float vy,
                    aae_move_slide_push_t *push, aae_move_slide_push_kind *kind) {
    const float along[2] = { float_of(x), float_of(y) }, velocity[3] = { vx, vy, 12.0f };
    memset(push, 0x5a, sizeof(*push));
    *kind = aae_move_slide_push(surface, flags, flagged, along, velocity, push);
}
static bool push_is(const aae_move_slide_push_t *push, uint32_t x, uint32_t y, uint32_t z, float wish, float accel) {
    return bits_of(push->direction[0]) == x && bits_of(push->direction[1]) == y && bits_of(push->direction[2]) == z &&
           push->wish_speed == wish && push->acceleration == accel;
}
static void test_by_hand(void) {
    aae_move_slide_push_t push;
    aae_move_slide_push_kind kind;
    const uint64_t flag51 = 1ull << 51;
    // on a slide surface: the direction of length 1, 300, 2
    push_of(true, 0, 0, 0x40400000, 0x40800000, 0.0f, 0.0f, &push, &kind); // 3, 4
    CHECK(kind == Slide_Push_Surface && push_is(&push, 0x3f19999a, 0x3f4ccccd, 0, 300.0f, 2.0f));
    // a direction of length 0 counts as 1: nothing
    push_of(true, 0, 0, 0, 0x80000000, 0.0f, 0.0f, &push, &kind);
    CHECK(kind == Slide_Push_Surface && push_is(&push, 0, 0x80000000, 0, 300.0f, 2.0f));
    // the surface goes before the other push
    push_of(true, flag51, 5, 0x40400000, 0x40800000, 312.5f, -147.25f, &push, &kind);
    CHECK(kind == Slide_Push_Surface);
    // the other push: (2 dir + v * 0.0066666668) * 0.333 in T7Overcharged's doubles
    push_of(false, flag51, 5, 0x3f19999a, 0xbf4ccccd, 312.5f, -147.25f, &push, &kind); // 0.6, -0.8
    CHECK(kind == Slide_Push_Flagged && push_is(&push, 0x3f8bf2e5, 0xbf5c14f9, 0, 1000.0f, 0.25f));
    // where the game's floats come out a bit apart: no direction (the game: 0x3f31999b) ...
    push_of(false, ~0ull, -1, 0, 0, 312.5f, -147.25f, &push, &kind);
    CHECK(kind == Slide_Push_Flagged && push_is(&push, 0x3f31999a, 0xbea75ec8, 0, 1000.0f, 0.25f));
    // ... and an ordinary one (the game: 0x3ec6841f)
    push_of(false, flag51, 1, 0x3e827ddb, 0x3f653a1b, float_of(0x42b90c0d), float_of(0xc2f7f784), &push, &kind);
    CHECK(kind == Slide_Push_Flagged && bits_of(push.direction[0]) == 0x3ec6841e);
    // neither: the flag without the move's int, the int without the flag, the surface's other bits
    push_of(false, flag51, 0, 0x3f19999a, 0xbf4ccccd, 1.0f, 1.0f, &push, &kind);
    CHECK(kind == Slide_Push_None);
    push_of(false, ~flag51, 7, 0x3f19999a, 0xbf4ccccd, 1.0f, 1.0f, &push, &kind);
    CHECK(kind == Slide_Push_None);
    // which NaN comes out where two meet: the first operand's, made quiet, as on x86. The square of y comes first,
    // then the inverse length before the direction ...
    push_of(true, 0, 0, 0x7fc00000, 0xffa00001, 1.0f, 1.0f, &push, &kind);
    CHECK(bits_of(push.direction[0]) == 0xffe00001 && bits_of(push.direction[1]) == 0xffe00001);
    // ... and in the other push the doubled direction before the velocity's share
    push_of(false, flag51, 1, 0x7fd23456, 0xffa00001, float_of(0x7f800001), 2.0f, &push, &kind);
    CHECK(bits_of(push.direction[0]) == 0xffe00001 && bits_of(push.direction[1]) == 0xffe00001 &&
          bits_of(push.direction[2]) == 0xffe00001);
    push_of(false, flag51, 1, 0x7fd23456, 0x3f800000, float_of(0xffa00001), 2.0f, &push, &kind);
    CHECK(bits_of(push.direction[0]) == 0x7fd23456 && bits_of(push.direction[1]) == 0x7fd23456);
    // the velocity from before the other push comes back above a speed of 90000, not at it, nor for a NaN
    CHECK(!aae_move_slide_push_undone((float[3]){ 90000.0f, 0.0f, 0.0f }));
    CHECK(aae_move_slide_push_undone((float[3]){ float_of(0x47afc801), 0.0f, 0.0f }));
    CHECK(aae_move_slide_push_undone((float[3]){ 0.0f, 0.0f, -95000.0f }));
    CHECK(!aae_move_slide_push_undone((float[3]){ 60000.0f, 60000.0f, 30000.0f })); // 90000 exactly
    CHECK(aae_move_slide_push_undone((float[3]){ 60000.0f, 60000.0f, 31000.0f }));
    CHECK(!aae_move_slide_push_undone((float[3]){ NAN, 1e30f, 0.0f }));
    CHECK(aae_move_slide_push_undone((float[3]){ INFINITY, 0.0f, 0.0f }));
}

// ---- the hook on PM_DoSlideAdjustments
static void test_hook(void) {
    uint8_t ps[0x400], pml[0x100];
    orig_PM_DoSlideAdjustments = game_slide_adjustments;
    PM_Accelerate = cases_accelerate;
    memset(ps, 0, sizeof(ps));
    memset(pml, 0, sizeof(pml));
    pml[Pml_Flags] = 2;
    const float along[2] = { 3.0f, 4.0f }, velocity[3] = { 100.0f, 0.0f, 0.0f };
    memcpy(pml + Pml_Along, along, sizeof(along));
    memcpy(ps + Ps_Velocity, velocity, sizeof(velocity));
    // off, or not started: the game's own function, and nothing else
    for (int round = 0; round < 2; round++) {
        if (round == 0)
            all_but(Move_SlidePreserveMomentum);
        else
            switches(false, -1);
        game_slide.calls = 0, cases_called.calls = 0;
        fake_PM_DoSlideAdjustments(ps, pml);
        CHECK(game_slide.calls == 1 && game_slide.ps == ps && game_slide.pml == pml && cases_called.calls == 0);
    }
    // on: T7Overcharged's instead
    switches(true, Move_SlidePreserveMomentum);
    game_slide.calls = 0, cases_called.calls = 0, cases_called.write_after = 0;
    fake_PM_DoSlideAdjustments(ps, pml);
    CHECK(game_slide.calls == 0 && cases_called.calls == 1 && cases_called.ps == ps && cases_called.pml == pml);
    CHECK(bits_of(cases_called.direction[0]) == 0x3f19999a && cases_called.wish_speed == 300.0f &&
          cases_called.acceleration == 2.0f);
    // the other push: the velocity is put back when PM_Accelerate left it above 90000, else kept
    pml[Pml_Flags] = 0xfd;
    const uint64_t flags = 1ull << 51;
    const int32_t flagged = 3;
    memcpy(ps + Ps_Flags, &flags, sizeof(flags));
    memcpy(pml + Pml_Flagged, &flagged, sizeof(flagged));
    const float far[3] = { 1e5f, 0.0f, 0.0f }, near[3] = { 400.0f, 0.0f, 1.0f };
    float now[3];
    cases_called.calls = 0, cases_called.write_after = 1;
    memcpy(cases_called.after, far, sizeof(far));
    fake_PM_DoSlideAdjustments(ps, pml);
    memcpy(now, ps + Ps_Velocity, sizeof(now));
    CHECK(cases_called.calls == 1 && cases_called.wish_speed == 1000.0f && memcmp(now, velocity, sizeof(now)) == 0);
    memcpy(cases_called.after, near, sizeof(near));
    fake_PM_DoSlideAdjustments(ps, pml);
    memcpy(now, ps + Ps_Velocity, sizeof(now));
    CHECK(cases_called.calls == 2 && memcmp(now, near, sizeof(now)) == 0);
    // a velocity above 90000 after the surface push stays: only the other push is undone
    memcpy(ps + Ps_Velocity, velocity, sizeof(velocity));
    pml[Pml_Flags] = 2;
    memcpy(cases_called.after, far, sizeof(far));
    fake_PM_DoSlideAdjustments(ps, pml);
    memcpy(now, ps + Ps_Velocity, sizeof(now));
    CHECK(cases_called.calls == 3 && memcmp(now, far, sizeof(now)) == 0);
    // neither push: nothing at all
    pml[Pml_Flags] = 0;
    memset(ps + Ps_Flags, 0, 8);
    cases_called.calls = 0;
    fake_PM_DoSlideAdjustments(ps, pml);
    CHECK(cases_called.calls == 0 && game_slide.calls == 0);
    cases_called.write_after = 0;
}

// ---- slideslowdown: the ramp's compare, "sub ecx, [rdx + 0x844] ; cmp ecx, eax ; jge over"
static bool ramp_over(const detour_registers_t *registers) {
    uint32_t end;
    memcpy(&end, (const uint8_t *)registers->rdx + Ps_Slide_Time, sizeof(end));
    int32_t since = (int32_t)((uint32_t)registers->rcx - end);
    return since >= (int32_t)(uint32_t)registers->rax;
}
static void test_ramp(void) {
    uint8_t ps[0x900];
    memset(ps, 0, sizeof(ps));
    static const int32_t lengths[] = { 1, 300, 2000, INT32_MAX };
    static const int64_t times[] = { INT32_MIN, -5000, -1, 0, 1, 299, 300, 301, 1999, 2000, 100000, INT32_MAX };
    static const uint32_t ends[] = { 0, 123456, 0x7fffff00u, 0x80000010u };
    for (int round = 0; round < 3; round++) {
        if (round == 0)
            switches(true, Move_SlideSlowdown);
        else if (round == 1)
            all_but(Move_SlideSlowdown);
        else
            switches(false, -1);
        bool on_now = round == 0;
        for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++)
            for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++)
                for (size_t e = 0; e < sizeof(ends) / sizeof(ends[0]); e++) {
                    detour_registers_t registers, before;
                    memset(&registers, 0x5a, sizeof(registers));
                    memcpy(ps + Ps_Slide_Time, &ends[e], sizeof(ends[e]));
                    registers.rdx = (uint64_t)(uintptr_t)ps;
                    registers.rax = (uint32_t)lengths[l];
                    registers.rcx = 0xabcd000000000000ULL | (uint32_t)(ends[e] + (uint32_t)times[t]);
                    before = registers;
                    bool game = ramp_over(&registers);
                    handler_Slide_RampCompare(&registers);
                    CHECK(ramp_over(&registers) == (on_now || game));
                    before.rax = registers.rax;
                    CHECK(same(&before, &registers)); // nothing else is changed
                    if (!on_now)
                        CHECK(registers.rax == (uint32_t)lengths[l]);
                }
    }
}

// ---- slideslowdown: the extra friction on a slide surface, "mulss xmm0, xmm2 ; mulss xmm0, [r14 + 0x24]"
static void test_surface_friction(void) {
    uint8_t ps[0x900];
    memset(ps, 0, sizeof(ps));
    for (int round = 0; round < 3; round++) {
        if (round == 0)
            switches(true, Move_SlideSlowdown);
        else if (round == 1)
            all_but(Move_SlideSlowdown);
        else
            switches(false, -1);
        detour_registers_t registers, before;
        memset(&registers, 0x33, sizeof(registers));
        lane_set(&registers, 0, 1.5f);
        lane_set(&registers, 2, 420.0f);
        registers.rbx = (uint64_t)(uintptr_t)ps;
        before = registers;
        handler_Slide_SurfaceFriction(&registers);
        float drop = lane(&registers, 0) * lane(&registers, 2) * 0.016f;
        if (round == 0)
            CHECK(bits_of(lane(&registers, 0)) == 0 && drop == 0.0f);
        else
            CHECK(lane(&registers, 0) == 1.5f && drop == 1.5f * 420.0f * 0.016f);
        memcpy(before.xmm[0], registers.xmm[0], 4);
        CHECK(same(&before, &registers)); // nothing else is changed
    }
}

// ---- instant_slide: "ucomiss xmm0, xmm2 ; jbe on": below the least speed the slide is refused
static bool slide_may_start(const detour_registers_t *registers) {
    return !(lane(registers, 0) > lane(registers, 2));
}
static void test_speed_test(void) {
    static const uint32_t speeds[] = { 0x00000000, 0x80000000, 0x42c80000, 0x43160000, 0x4315ffff, 0x43160001,
                                       0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x00000001, 0x7f7fffff };
    for (int round = 0; round < 3; round++) {
        if (round == 0)
            switches(true, Move_InstantSlide);
        else if (round == 1)
            all_but(Move_InstantSlide);
        else
            switches(false, -1);
        for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
            detour_registers_t registers, before;
            memset(&registers, 0x44, sizeof(registers));
            lane_set(&registers, 0, 150.0f);
            lane_set(&registers, 2, float_of(speeds[i]));
            before = registers;
            bool game = slide_may_start(&registers);
            handler_Slide_SpeedTest(&registers);
            CHECK(slide_may_start(&registers) == (round == 0 || game));
            if (round != 0)
                CHECK(lane(&registers, 0) == 150.0f);
            memcpy(before.xmm[0], registers.xmm[0], 4);
            CHECK(same(&before, &registers)); // nothing else is changed
        }
    }
}

// ---- slideangles: "movss xmm0, [rax + 0x318]", then "ucomiss xmm0, 10.05 ; jbe": above it the view is pulled to 10
static bool view_pulled(const detour_registers_t *registers) {
    float pitch;
    memcpy(&pitch, (const uint8_t *)(uintptr_t)registers->rax + Ps_View_Pitch, sizeof(pitch));
    return pitch > 10.05f;
}
static void test_start_pitch(void) {
    uint8_t ps[0x900];
    static const uint32_t pitches[] = { 0x00000000, 0x4120cccd, 0x4120ccce, 0x420c0000, 0x42b20000, 0xc20c0000,
                                        0x7fc00000, 0x7f800000, 0xff800000, 0x43b40000 };
    for (int round = 0; round < 3; round++) {
        if (round == 0)
            switches(true, Move_SlideAngles);
        else if (round == 1)
            all_but(Move_SlideAngles);
        else
            switches(false, -1);
        for (size_t i = 0; i < sizeof(pitches) / sizeof(pitches[0]); i++) {
            detour_registers_t registers, before;
            memset(&registers, 0x55, sizeof(registers));
            memset(ps, 0, sizeof(ps));
            float pitch = float_of(pitches[i]);
            memcpy(ps + Ps_View_Pitch, &pitch, sizeof(pitch));
            registers.rax = (uint64_t)(uintptr_t)ps;
            before = registers;
            bool game = view_pulled(&registers);
            handler_Slide_StartPitch(&registers);
            CHECK(view_pulled(&registers) == (round != 0 && game));
            float compared;
            memcpy(&compared, (const uint8_t *)(uintptr_t)registers.rax + Ps_View_Pitch, sizeof(compared));
            if (round != 0)
                CHECK(registers.rax == (uint64_t)(uintptr_t)ps);
            else
                CHECK(bits_of(compared) == 0xff800000); // what the instruction reads is minus infinity
            CHECK(memcmp(ps + Ps_View_Pitch, &pitch, sizeof(pitch)) == 0); // the player state isn't touched
            before.rax = registers.rax;
            CHECK(same(&before, &registers)); // nothing else is changed
        }
    }
}

// ---- slideangles: the 20 degree clamp while sliding is left out
static void test_pitch_clamp(void) {
    uint8_t ps[0x900], command[0x40];
    memset(ps, 0, sizeof(ps));
    switches(true, Move_SlideAngles);
    CHECK(handler_Slide_PitchClamp((uint64_t)(uintptr_t)ps, (uint64_t)(uintptr_t)command));
    all_but(Move_SlideAngles);
    CHECK(!handler_Slide_PitchClamp((uint64_t)(uintptr_t)ps, (uint64_t)(uintptr_t)command));
    switches(false, -1);
    CHECK(!handler_Slide_PitchClamp((uint64_t)(uintptr_t)ps, (uint64_t)(uintptr_t)command));
}

// ---- what is installed where
static void init_with(bool enabled, bool stubs, uint32_t missing) {
    test_enabled = enabled, have_stubs = stubs, stub_missing_for = missing;
    hooks = 0;
    PM_Accelerate = NULL;
    said[0] = '\0';
    say = test_say;
    aae_move_slide_init(0x100000000);
    say = printf;
}
static void test_install(void) {
    const uint64_t base = 0x100000000;
    unsetenv("BO3MACFIX_MOVEDEBUG");
    init_with(true, true, 1);
    CHECK(hooks == 6);
    CHECK(was_hooked(ADDR_PM_DoSlideAdjustments, (void *)fake_PM_DoSlideAdjustments,
                     (void **)&orig_PM_DoSlideAdjustments));
    CHECK(was_hooked(ADDR_PM_CmdScale_SlideRampCmp, DETOUR_ENTRY(Slide_RampCompare), &orig_Slide_RampCompare));
    CHECK(was_hooked(ADDR_PM_Friction_SlideFriction, DETOUR_ENTRY(Slide_SurfaceFriction),
                     &orig_Slide_SurfaceFriction));
    CHECK(was_hooked(ADDR_CanSlide_SpeedTest, DETOUR_ENTRY(Slide_SpeedTest), &orig_Slide_SpeedTest));
    CHECK(was_hooked(ADDR_StartSlide_PitchLoad, DETOUR_ENTRY(Slide_StartPitch), &orig_Slide_StartPitch));
    CHECK(was_hooked(ADDR_PM_UpdateViewAngles_SlideClamp, DETOUR_ENTRY(Slide_PitchClamp), &orig_Slide_PitchClamp) &&
          other_Slide_PitchClamp == (void *)(base + ADDR_PM_UpdateViewAngles_SlideClampDone));
    CHECK(PM_Accelerate == (void *)(base + ADDR_PM_Accelerate) && cg_clients == (void *)(base + ADDR_cgArray));
    CHECK(said[0] == '\0');
    // AAEMovement off: nothing
    init_with(false, true, 1);
    CHECK(hooks == 0 && said[0] == '\0');
    // not from the pre-patched executable, or a copy without one of the places: nothing, and a line that says so
    init_with(true, false, 1);
    CHECK(hooks == 0 && strstr(said, "movement: not running from the pre-patched executable") == said);
    static const uint32_t places[] = { ADDR_PM_DoSlideAdjustments, ADDR_PM_CmdScale_SlideRampCmp,
                                       ADDR_PM_Friction_SlideFriction, ADDR_CanSlide_SpeedTest,
                                       ADDR_StartSlide_PitchLoad, ADDR_PM_UpdateViewAngles_SlideClamp };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        init_with(true, true, places[i]);
        CHECK(hooks == 0);
    }
    test_enabled = true, have_stubs = true, stub_missing_for = 1;
    // the places, as the research found and this piece checked them in the game's file
    CHECK(ADDR_PM_DoSlideAdjustments == 0x11488f0 && ADDR_PM_Accelerate == 0x1147ed9 &&
          ADDR_PM_CmdScale_SlideRampCmp == 0x11486ac && ADDR_PM_Friction_SlideFriction == 0x113c526 &&
          ADDR_CanSlide_SpeedTest == 0xe814f5 && ADDR_StartSlide_PitchLoad == 0xe82226 &&
          ADDR_PM_UpdateViewAngles_SlideClamp == 0x113e4b3 && ADDR_PM_UpdateViewAngles_SlideClampDone == 0x113e4c3);
    CHECK(Ps_Flags == 0x10 && Ps_Velocity == 0x3c && Ps_View_Pitch == 0x318 && Ps_Slide_Time == 0x844 &&
          Pml_Along == 0x40 && Pml_Flags == 0x54 && Pml_Flagged == 0x64 && Cg_PlayerState == 0x11a8d0);
}

// ---- the debug lines the game checks look for (wip/agents/movec/gamecheck)
static bool said_has(const char *text) {
    return strstr(said, text) != NULL;
}
static void test_debug_lines(void) {
    uint8_t ps[0x900];
    memset(ps, 0, sizeof(ps));
    say = test_say, debug = 1;
    // instant_slide, and the game's own answer
    detour_registers_t registers;
    memset(&registers, 0, sizeof(registers));
    lane_set(&registers, 0, 150.0f);
    lane_set(&registers, 2, 48.25f);
    said[0] = '\0';
    switches(true, Move_InstantSlide);
    handler_Slide_SpeedTest(&registers);
    CHECK(said_has("movement: instant_slide: lets a slide at a horizontal speed of 48.2 (the game needs 150)\n"));
    // a slide's start looking down
    float pitch = 35.0f, velocity[3] = { 450.0f, 0.0f, 0.0f };
    uint64_t flags = Flag_Sliding;
    memcpy(ps + Ps_View_Pitch, &pitch, sizeof(pitch));
    memcpy(ps + Ps_Velocity, velocity, sizeof(velocity));
    memcpy(ps + Ps_Flags, &flags, sizeof(flags));
    registers.rax = (uint64_t)(uintptr_t)ps;
    said[0] = '\0';
    switches(true, Move_SlideAngles);
    handler_Slide_StartPitch(&registers);
    CHECK(said_has("movement: a slide starts (flag 7 set), horizontal speed 450.0, view pitch 35.0: slideangles "
                   "leaves it (the game would pull it to 10)\n"));
    registers.rax = (uint64_t)(uintptr_t)ps;
    said[0] = '\0';
    switches(false, -1);
    handler_Slide_StartPitch(&registers);
    CHECK(said_has("view pitch 35.0: the game pulls it to 10\n"));
    // the clamp while sliding: once per slide, and once more past 20 degrees
    int32_t start = 5000;
    memcpy(ps + Ps_Slide_Time, &start, sizeof(start));
    said[0] = '\0';
    switches(true, Move_SlideAngles);
    handler_Slide_PitchClamp((uint64_t)(uintptr_t)ps, 0);
    handler_Slide_PitchClamp((uint64_t)(uintptr_t)ps, 0);
    CHECK(said_has("movement: slideangles: no 20 degree pitch clamp while sliding, view pitch 35.0\n"));
    CHECK(strstr(strstr(said, "clamp") + 1, "clamp") == NULL);
    // what the player does, by the predicted player state: asked in the air, a slide from 48, its end, after it
    watched_t w = { 0 };
    float still[3] = { 30.0f, 38.0f, 0.0f }, fast[3] = { 600.0f, 0.0f, 0.0f }, slow[3] = { 90.0f, 0.0f, 0.0f };
    said[0] = '\0';
    watch_step(&w, 1000, Flag_Slide_Asked, still, 12.0f);
    watch_step(&w, 1100, Flag_Sliding, fast, 40.0f);
    watch_step(&w, 1400, Flag_Sliding, fast, 70.0f);
    watch_step(&w, 1620, Flag_Sliding, fast, 60.0f);
    watch_step(&w, 1900, 0, slow, 60.0f);
    watch_step(&w, 2010, 0, slow, 60.0f);
    watch_step(&w, 2160, 0, slow, 60.0f);
    watch_step(&w, 2410, 0, fast, 60.0f);
    CHECK(said_has("movement: seen: a slide asked for in the air (flag 46), horizontal speed 48.4\n"));
    CHECK(said_has("movement: seen: a slide begins, horizontal speed 48.4 before it, 600.0 at its start, view pitch "
                   "40.0\n"));
    CHECK(said_has("movement: seen: the slide ended after 800 ms: view pitch 40.0 at its start, 60.0 at 0.5 s, 40.0 "
                   "to 70.0 in all; horizontal speed 600.0 at its start\n"));
    CHECK(said_has("movement: seen: horizontal speed 90.0, 90.0 and 600.0 at 0.1, 0.25 and 0.5 s after the slide\n"));
    say = printf, debug = 0;
}

int main(void) {
    test_constants();
    test_like_t7o();
    test_by_hand();
    test_hook();
    test_ramp();
    test_surface_friction();
    test_speed_test();
    test_start_pitch();
    test_pitch_clamp();
    test_install();
    test_debug_lines();
#if defined(__x86_64__)
    const char *name = "test_aae_move_slide (x86_64)";
#else
    const char *name = "test_aae_move_slide";
#endif
    if (failures != 0) {
        printf("%s: %d failed\n", name, failures);
        return 1;
    }
    printf("%s: all passed\n", name);
    return 0;
}
