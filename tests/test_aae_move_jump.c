// aae_move_jump.c: T7Overcharged's jumpslowdown rules, and its hooks' and detours' handlers against a made-up game
// (player states, the game's own functions as they are, and a small model of a jump and a landing). The thunks that
// call the detours' handlers in the game are x86_64 code and have a test of their own (test_detour.c).
#include <pthread.h>
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

#include "../source/aae_move_jump.c"
#include "move_jump_cases.h"

// ---- what the code asks of the rest of BO3MacFix: the switches (aae_movement.h)

static bool setting = true, started, switches[Move_Patch_Count];
static int asked_for = -1; // the last patch whose switch was asked for
bool aae_movement_enabled(void) {
    return setting;
}
bool aae_movement_started(void) {
    return started;
}
bool aae_movement_patch_on(move_patch patch) {
    asked_for = patch;
    return patch >= 0 && patch < Move_Patch_Count && switches[patch];
}
bool aae_movement_omni(void) {
    return false;
}
float aae_movement_strafe_scale(void) {
    return 0.667f;
}
float aae_movement_fastads_multiplier(void) {
    return 2.15f;
}
// T7Overcharged started and its patches as All-around Enhancement leaves them (all but fastswap), or switched
static void set_movement(bool is_started, bool jump_slowdown) {
    started = is_started;
    for (int i = 0; i < Move_Patch_Count; i++)
        switches[i] = i != Move_FastSwap;
    switches[Move_JumpSlowdown] = jump_slowdown;
}

// the pre-patched executable's stubs, and what was hooked
static bool have_stubs = true;
static int64_t stub_missing_for = -1;
bool prepatch_has_hook(uint32_t offset) {
    return have_stubs && (int64_t)offset != stub_missing_for;
}
static struct {
    void *address, *handler, **original;
} hooked[16];
static int hooks;

// ---- the made-up game: its functions as they are (the Mac's code, read instruction by instruction), and how
// often each was called
static struct {
    int view_dip, slow_down, reduce_friction;
    uint8_t *ps;
    uint32_t fall_bits;
} calls;
static bool gadget13; // the player has the gadget that spares the jump penalties
static int game_view_dip(uint8_t *ps, float fall_height) {
    calls.view_dip++, calls.ps = ps;
    memcpy(&calls.fall_bits, &fall_height, sizeof(calls.fall_bits));
    int dip = 0;
    if (!(12.0f >= fall_height)) // ucomiss 12, fall ; jae: not a number goes on
        dip = (int)((fall_height + -12.0f) / 26.0f * 4.0f + 4.0f);
    int capped = dip < 16 ? dip : 15;
    uint64_t flags = flags_of(ps);
    if (ps[0x162] & 4)
        return (flags >> 55) & 1 ? 0 : 0;
    dip = (flags & 2) ? (capped >= 10 ? 10 : dip) : capped;
    if (flags & 0x400000000080ull)
        dip = 0;
    return (flags >> 55) & 1 ? 0 : dip;
}
static void game_slow_down(uint8_t *ps) {
    calls.slow_down++, calls.ps = ps;
    int32_t time = int_at(ps + Move_Ps_Time);
    float scale;
    if (time >= 1001) {
        ps[Move_Ps_Flags + 2] &= (uint8_t)~4;
        memset(ps + Move_Ps_JumpOriginZ, 0, 4);
        scale = 0.8f;
    } else if (time == 0) {
        int32_t thousand = 1000;
        memcpy(ps + Move_Ps_Time, &thousand, 4);
        scale = 0.8f;
    } else {
        scale = 1.0f;
    }
    if (!gadget13) {
        float v[3];
        memcpy(v, ps + Move_Ps_Velocity, sizeof(v));
        v[0] = v[0] * scale, v[1] = v[1] * scale, v[2] = scale * v[2];
        memcpy(ps + Move_Ps_Velocity, v, sizeof(v));
    }
}
static float game_reduce_friction(uint8_t *ps) {
    calls.reduce_friction++, calls.ps = ps;
    if (int_at(ps + Move_Ps_Time) > 1000) {
        ps[Move_Ps_Flags + 2] &= (uint8_t)~4;
        memset(ps + Move_Ps_JumpOriginZ, 0, 4);
        return 1.0f;
    }
    if (gadget13)
        return 1.0f;
    int32_t time = int_at(ps + Move_Ps_Time);
    if (time > 900)
        return 2.0f;
    return (float)time * 0.0011098779f + 1.0f;
}

int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 16)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    if (out_origin_func == (void **)&orig_BG_GetViewDip)
        *out_origin_func = (void *)game_view_dip;
    else if (out_origin_func == (void **)&orig_Jump_SlowDown)
        *out_origin_func = (void *)game_slow_down;
    else if (out_origin_func == (void **)&orig_Jump_ReduceFriction)
        *out_origin_func = (void *)game_reduce_friction;
    return 0;
}
static bool was_hooked(uint64_t base, uint32_t offset, void *handler, void *original) {
    for (int i = 0; i < hooks && i < 16; i++)
        if (hooked[i].address == (void *)(base + offset))
            return hooked[i].handler == handler && hooked[i].original == original;
    return false;
}

// player states with something in every byte, so that a write to the wrong place shows
static void fill(uint8_t *ps, size_t size, uint32_t seed) {
    for (size_t i = 0; i < size; i++) {
        seed = seed * 1103515245u + 12345u;
        ps[i] = (uint8_t)(seed >> 16);
    }
}
#define Ps_Size 0x1000
static void set_int(uint8_t *ps, size_t at, int32_t value) {
    memcpy(ps + at, &value, sizeof(value));
}
static void set_float(uint8_t *ps, size_t at, float value) {
    memcpy(ps + at, &value, sizeof(value));
}
static void set_flags(uint8_t *ps, uint64_t flags) {
    memcpy(ps + Move_Ps_Flags, &flags, sizeof(flags));
}
static uint32_t bits_of(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// ---- the rules

// the player state's layout, as both games' code reads it (Jump_Start, PM_CrashLand, Jump_SlowDown, BG_GetViewDip),
// and the pmove_t's and pml_t's fields the handlers read
static void test_layout(void) {
    CHECK(Move_Ps_Flags == 0x10 && Move_Ps_Time == 0x24 && Move_Ps_Origin == 0x30 && Move_Ps_Velocity == 0x3c);
    CHECK(Move_Ps_Gravity == 0xc4 && Move_Ps_JumpTime == 0x110 && Move_Ps_JumpOriginZ == 0x114);
    CHECK(Move_Flag_Sliding == 0x80 && Move_Flag_SlowdownPending == 0x40000);
    CHECK(Pm_PlayerState == 0 && Pm_CommandTime == 8 && Pml_FallSpeed == 0xa8);
    CHECK(Slowdown_Ms == 1000 && Divisor_Full_Ms == 901 && bits_of(Divisor_Per_Ms) == 0x3a917953);
}

static void test_on(void) {
    CHECK(!aae_move_jump_on(false, false));
    CHECK(!aae_move_jump_on(false, true));
    CHECK(!aae_move_jump_on(true, false));
    CHECK(aae_move_jump_on(true, true));
    CHECK(aae_move_jump_full_height(true));
    CHECK(!aae_move_jump_full_height(false));
}

static void test_reduce_friction(void) {
    static const int32_t times[] = { 0, 1, -1, 900, 901, 999, 1000, 1001, 1002, 5000, INT32_MAX, INT32_MIN };
    static const uint64_t flag_sets[] = { 0, Move_Flag_SlowdownPending, ~0ull, ~Move_Flag_SlowdownPending,
                                          0x8000000000040080ull, 0x123456789abcdef0ull };
    for (size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++) {
        for (size_t f = 0; f < sizeof(flag_sets) / sizeof(flag_sets[0]); f++) {
            uint8_t ps[Ps_Size], before[Ps_Size];
            fill(ps, sizeof(ps), (uint32_t)(t * 31 + f));
            set_int(ps, Move_Ps_Time, times[t]);
            set_flags(ps, flag_sets[f]);
            memcpy(before, ps, sizeof(ps));
            float multiplier = aae_move_jump_reduce_friction(ps);
            CHECK(bits_of(multiplier) == 0x3f800000); // T7O's constant at 0x1806129f4
            bool over = times[t] > 1000;
            CHECK(flags_of(ps) == (over ? flag_sets[f] & ~Move_Flag_SlowdownPending : flag_sets[f]));
            uint32_t origin_z;
            memcpy(&origin_z, ps + Move_Ps_JumpOriginZ, 4);
            CHECK(over ? origin_z == 0 : memcmp(ps + Move_Ps_JumpOriginZ, before + Move_Ps_JumpOriginZ, 4) == 0);
            // nothing else changes
            memcpy(before + Move_Ps_Flags, ps + Move_Ps_Flags, 8);
            memcpy(before + Move_Ps_JumpOriginZ, ps + Move_Ps_JumpOriginZ, 4);
            CHECK(memcmp(before, ps, sizeof(ps)) == 0);
        }
    }
}

static void test_landing_rule(void) {
    static const uint32_t odd[] = { 0x7fc00001, 0xff800000, 0x80000000, 0x00000001, 0x43480000, 0xc3fa0000 };
    for (size_t i = 0; i < sizeof(odd) / sizeof(odd[0]); i++) {
        uint8_t ps[Ps_Size], before[Ps_Size];
        aae_move_jump_landing_t landing = { 0 };
        fill(ps, sizeof(ps), (uint32_t)i + 100);
        uint32_t x = odd[i], y = odd[(i + 1) % 6];
        memcpy(ps + Move_Ps_Velocity, &x, 4);
        memcpy(ps + Move_Ps_Velocity + 4, &y, 4);
        aae_move_jump_landing_begin(&landing, true, ps);
        // the game's landing: the velocity scaled, other things changed
        set_float(ps, Move_Ps_Velocity, 1.5f), set_float(ps, Move_Ps_Velocity + 4, -2.5f);
        set_float(ps, Move_Ps_Velocity + 8, -300.0f);
        set_int(ps, Move_Ps_Time, 1700);
        ps[0x908] ^= 0x55;
        memcpy(before, ps, sizeof(ps));
        CHECK(aae_move_jump_landing_end(&landing, ps));
        CHECK(memcmp(ps + Move_Ps_Velocity, &x, 4) == 0 && memcmp(ps + Move_Ps_Velocity + 4, &y, 4) == 0);
        CHECK(bits_of(float_at(ps + Move_Ps_Velocity + 8)) == 0); // +0, not -0
        memcpy(before + Move_Ps_Velocity, ps + Move_Ps_Velocity, 12);
        CHECK(memcmp(before, ps, sizeof(ps)) == 0); // what the game's landing did to the rest stays
        CHECK(landing.ps == NULL);
        // an end once more: nothing (the landing was used)
        set_float(ps, Move_Ps_Velocity, 9.0f);
        CHECK(!aae_move_jump_landing_end(&landing, ps) && float_at(ps + Move_Ps_Velocity) == 9.0f);
    }
    uint8_t a[Ps_Size], b[Ps_Size], a_before[Ps_Size], b_before[Ps_Size];
    aae_move_jump_landing_t landing = { 0 };
    fill(a, sizeof(a), 1), fill(b, sizeof(b), 2);
    // switched off at the begin: the game's landing stays as it is, also when it is switched on before the end
    aae_move_jump_landing_begin(&landing, false, a);
    memcpy(a_before, a, sizeof(a));
    CHECK(!aae_move_jump_landing_end(&landing, a) && memcmp(a, a_before, sizeof(a)) == 0);
    // an end without a begin
    landing = (aae_move_jump_landing_t){ 0 };
    CHECK(!aae_move_jump_landing_end(&landing, a) && memcmp(a, a_before, sizeof(a)) == 0);
    // the begin's player state is another one than the end's: neither is written
    aae_move_jump_landing_begin(&landing, true, a);
    set_float(a, Move_Ps_Velocity, 77.0f);
    memcpy(a_before, a, sizeof(a)), memcpy(b_before, b, sizeof(b));
    CHECK(!aae_move_jump_landing_end(&landing, b));
    CHECK(memcmp(a, a_before, sizeof(a)) == 0 && memcmp(b, b_before, sizeof(b)) == 0);
    // a landing switched on, then one switched off before an end: the second one's end does nothing
    aae_move_jump_landing_begin(&landing, true, a);
    aae_move_jump_landing_begin(&landing, false, a);
    set_float(a, Move_Ps_Velocity, 5.0f);
    memcpy(a_before, a, sizeof(a));
    CHECK(!aae_move_jump_landing_end(&landing, a) && memcmp(a, a_before, sizeof(a)) == 0);
    // the latest begin counts
    set_float(a, Move_Ps_Velocity, 1.0f);
    aae_move_jump_landing_begin(&landing, true, a);
    set_float(a, Move_Ps_Velocity, 2.0f);
    aae_move_jump_landing_begin(&landing, true, a);
    set_float(a, Move_Ps_Velocity, 3.0f);
    CHECK(aae_move_jump_landing_end(&landing, a) && float_at(a + Move_Ps_Velocity) == 2.0f);
}

static void test_speed_cap(void) {
    static const float caps[] = { 280.0f, 336.0f, 0.0f, -1.0f, INFINITY };
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        float cap = caps[i];
        CHECK(isinf(aae_move_jump_speed_cap(true, 0, cap)) && aae_move_jump_speed_cap(true, 0, cap) > 0.0f);
        CHECK(isinf(aae_move_jump_speed_cap(true, ~Move_Flag_Sliding, cap)));
        CHECK(aae_move_jump_speed_cap(true, Move_Flag_Sliding, cap) == cap);
        CHECK(aae_move_jump_speed_cap(true, ~0ull, cap) == cap);
        CHECK(aae_move_jump_speed_cap(false, 0, cap) == cap);
        CHECK(aae_move_jump_speed_cap(false, Move_Flag_Sliding, cap) == cap);
        CHECK(aae_move_jump_speed_cap(true, 1ull << 6, cap) != cap || isinf(cap));
        CHECK(aae_move_jump_speed_cap(true, 1ull << 8, cap) != cap || isinf(cap));
    }
    CHECK(isnan(aae_move_jump_speed_cap(false, 0, NAN)) && isnan(aae_move_jump_speed_cap(true, 0x80, NAN)));
    // what the game does with the cap: "ucomiss speed squared, cap squared ; jbe <leave it>", so it caps only when
    // the speed's square is above the cap's; never with +infinity, whatever the speed (not a number included)
    static const float speeds[] = { 0.0f, 1.0f, 300.0f, 1e18f, 1e19f, INFINITY, NAN };
    float none = aae_move_jump_speed_cap(true, 0, 280.0f);
    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++)
        CHECK(!(speeds[i] * speeds[i] > none * none));
}

// ---- the hooks' and detours' handlers

static void test_hooks(void) {
    uint8_t ps[Ps_Size], before[Ps_Size];
    fill(ps, sizeof(ps), 7);
    ps[0x162] = 0; // not in the state that has no dip at all
    set_flags(ps, Move_Flag_SlowdownPending);
    set_float(ps, Move_Ps_Velocity, 200.0f), set_float(ps, Move_Ps_Velocity + 4, -100.0f);
    // the view dip: 0 while on, the game's own otherwise (asked with what came)
    for (int state = 0; state < 4; state++) {
        set_movement(state & 1, state & 2);
        memset(&calls, 0, sizeof(calls));
        asked_for = -1;
        int dip = fake_BG_GetViewDip(ps, 300.0f);
        uint32_t fall_seen = calls.fall_bits; // what the game's own was asked with
        if (state == 3) {
            // (with debug lines the game's own is asked for the landing's line, its answer not used)
            CHECK(dip == 0 && calls.view_dip == (debug ? 1 : 0) && asked_for == Move_JumpSlowdown);
        } else {
            CHECK(calls.view_dip == 1 && calls.ps == ps && fall_seen == bits_of(300.0f));
            CHECK(dip == game_view_dip(ps, 300.0f) && dip == 15);
        }
    }
    // only the jumpslowdown switch counts
    set_movement(true, false);
    for (int i = 0; i < Move_Patch_Count; i++)
        switches[i] = true;
    switches[Move_JumpSlowdown] = false;
    memset(&calls, 0, sizeof(calls));
    CHECK(fake_BG_GetViewDip(ps, 300.0f) == 15 && calls.view_dip == 1);
    // the landing slowdown: nothing while on
    set_movement(true, true);
    set_int(ps, Move_Ps_Time, 0);
    memcpy(before, ps, sizeof(ps));
    memset(&calls, 0, sizeof(calls));
    fake_Jump_SlowDown(ps);
    CHECK(calls.slow_down == 0 && memcmp(ps, before, sizeof(ps)) == 0);
    for (int state = 0; state < 3; state++) {
        set_movement(state & 1, state & 2);
        set_int(ps, Move_Ps_Time, 0);
        memset(&calls, 0, sizeof(calls));
        fake_Jump_SlowDown(ps);
        CHECK(calls.slow_down == 1 && int_at(ps + Move_Ps_Time) == 1000 && float_at(ps + Move_Ps_Velocity) == 160.0f);
        set_float(ps, Move_Ps_Velocity, 200.0f), set_float(ps, Move_Ps_Velocity + 4, -100.0f);
    }
    // the friction multiplier: T7O's while on, the game's otherwise
    set_movement(true, true);
    set_int(ps, Move_Ps_Time, 500);
    memset(&calls, 0, sizeof(calls));
    CHECK(fake_Jump_ReduceFriction(ps) == 1.0f && calls.reduce_friction == 0);
    set_int(ps, Move_Ps_Time, 1500);
    CHECK(fake_Jump_ReduceFriction(ps) == 1.0f && calls.reduce_friction == 0);
    CHECK(!(flags_of(ps) & Move_Flag_SlowdownPending) && int_at(ps + Move_Ps_JumpOriginZ) == 0);
    set_flags(ps, Move_Flag_SlowdownPending);
    for (int state = 0; state < 3; state++) {
        set_movement(state & 1, state & 2);
        set_int(ps, Move_Ps_Time, 500);
        memset(&calls, 0, sizeof(calls));
        float game = (float)500 * 0.0011098779f + 1.0f;
        CHECK(fake_Jump_ReduceFriction(ps) == game && calls.reduce_friction == 1);
    }
}

// the registers the game has at a detoured place
static detour_registers_t registers_with(uint64_t r12, uint64_t r13, uint64_t r14, uint64_t r15) {
    detour_registers_t registers;
    fill((uint8_t *)&registers, sizeof(registers), (uint32_t)r12);
    registers.r12 = r12, registers.r13 = r13, registers.r14 = r14, registers.r15 = r15;
    return registers;
}

static void test_detours(void) {
    uint8_t ps[Ps_Size], pm[0x300], pml[0x100];
    fill(ps, sizeof(ps), 11), fill(pm, sizeof(pm), 12), fill(pml, sizeof(pml), 13);
    uint8_t *ps_pointer = ps;
    memcpy(pm + Pm_PlayerState, &ps_pointer, sizeof(ps_pointer));
    set_float(ps, Move_Ps_Velocity, 120.0f), set_float(ps, Move_Ps_Velocity + 4, 160.0f);
    set_float(ps, Move_Ps_Velocity + 8, -260.0f);
    // the landing: the begin finds the player state through the pmove_t in r12, the end has it in r15
    set_movement(true, true);
    detour_registers_t begin = registers_with((uint64_t)pm, (uint64_t)pml, 0, (uint64_t)ps);
    detour_registers_t begin_before = begin;
    handler_Landing_Begin(&begin);
    CHECK(memcmp(&begin, &begin_before, sizeof(begin)) == 0); // no register changed
    set_float(ps, Move_Ps_Velocity, 120.0f * 0.67f), set_float(ps, Move_Ps_Velocity + 4, 160.0f * 0.67f);
    set_movement(true, false); // switched off during the landing: the landing that began on is finished on
    detour_registers_t end = registers_with(0x1234, 0, 0, (uint64_t)ps), end_before = end;
    handler_Landing_End(&end);
    CHECK(memcmp(&end, &end_before, sizeof(end)) == 0);
    CHECK(float_at(ps + Move_Ps_Velocity) == 120.0f && float_at(ps + Move_Ps_Velocity + 4) == 160.0f);
    CHECK(bits_of(float_at(ps + Move_Ps_Velocity + 8)) == 0);
    // a landing that began off isn't changed at its end, also when switched on in between
    set_float(ps, Move_Ps_Velocity + 8, -260.0f);
    handler_Landing_Begin(&begin);
    set_movement(true, true);
    set_float(ps, Move_Ps_Velocity, 3.0f);
    handler_Landing_End(&end);
    CHECK(float_at(ps + Move_Ps_Velocity) == 3.0f && float_at(ps + Move_Ps_Velocity + 8) == -260.0f);
    // not started: the switch alone does nothing
    set_movement(false, true);
    handler_Landing_Begin(&begin);
    set_float(ps, Move_Ps_Velocity, 4.0f);
    handler_Landing_End(&end);
    CHECK(float_at(ps + Move_Ps_Velocity) == 4.0f && float_at(ps + Move_Ps_Velocity + 8) == -260.0f);

    // the jump's speed cap: xmm0's first float, the player state in r12
    static const struct {
        bool started, on;
        uint64_t flags;
        bool none;
    } caps[] = { { true, true, 0, true },
                 { true, true, Move_Flag_SlowdownPending | 0x20, true },
                 { true, true, Move_Flag_Sliding, false },
                 { true, true, Move_Flag_Sliding | 0x20, false },
                 { true, false, 0, false },
                 { false, true, 0, false },
                 { false, false, Move_Flag_Sliding, false } };
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        set_movement(caps[i].started, caps[i].on);
        set_flags(ps, caps[i].flags);
        detour_registers_t registers = registers_with((uint64_t)ps, 0, (uint64_t)pm, 0), registers_before;
        set_float((uint8_t *)registers.xmm[0], 0, 336.0f);
        registers_before = registers;
        handler_Jump_SpeedCap(&registers);
        float given = float_at(registers.xmm[0]);
        CHECK(caps[i].none ? isinf(given) && given > 0.0f : given == 336.0f);
        // nothing else: the other three floats of xmm0 and every other register stay
        memcpy(registers.xmm[0], registers_before.xmm[0], 4);
        CHECK(memcmp(&registers, &registers_before, sizeof(registers)) == 0);
    }

    // the jump height test: yes as soon as T7O's movement started, whatever the switch
    set_movement(true, false);
    CHECK(handler_Jump_HeightTest());
    set_movement(true, true);
    CHECK(handler_Jump_HeightTest());
    set_movement(false, true);
    CHECK(!handler_Jump_HeightTest());
    set_movement(false, false);
    CHECK(!handler_Jump_HeightTest());
    height_test = Height_Not_Reached; // (in the game the speed cap's handler always follows)
}

// Two threads land at once, as the server's and the client's prediction do: each one's end puts back what its own
// begin kept. The threads take turns in this order: 1 begins, 2 begins, 1 ends, 2 ends.
static pthread_mutex_t turn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t turn_changed = PTHREAD_COND_INITIALIZER;
static int turn;
static void wait_for(int wanted) {
    pthread_mutex_lock(&turn_lock);
    while (turn != wanted)
        pthread_cond_wait(&turn_changed, &turn_lock);
    pthread_mutex_unlock(&turn_lock);
}
static void next_turn(void) {
    pthread_mutex_lock(&turn_lock);
    turn++;
    pthread_cond_broadcast(&turn_changed);
    pthread_mutex_unlock(&turn_lock);
}
typedef struct {
    int first_turn;
    uint8_t ps[Ps_Size], pm[0x300];
    float x, y;
} lander_t;
static void *land(void *argument) {
    lander_t *lander = argument;
    uint8_t *ps_pointer = lander->ps;
    memcpy(lander->pm, &ps_pointer, sizeof(ps_pointer));
    set_float(lander->ps, Move_Ps_Velocity, lander->x), set_float(lander->ps, Move_Ps_Velocity + 4, lander->y);
    detour_registers_t begin = registers_with((uint64_t)lander->pm, 0, 0, (uint64_t)lander->ps);
    detour_registers_t end = registers_with(0, 0, 0, (uint64_t)lander->ps);
    wait_for(lander->first_turn);
    handler_Landing_Begin(&begin);
    set_float(lander->ps, Move_Ps_Velocity, 0.0f), set_float(lander->ps, Move_Ps_Velocity + 4, 0.0f);
    next_turn();
    wait_for(lander->first_turn + 2);
    handler_Landing_End(&end);
    next_turn();
    return NULL;
}
static void test_threads(void) {
    static lander_t server = { .first_turn = 0, .x = 100.0f, .y = 50.0f };
    static lander_t client = { .first_turn = 1, .x = -30.0f, .y = 250.0f };
    set_movement(true, true);
    turn = 0;
    pthread_t one, two;
    pthread_create(&one, NULL, land, &server);
    pthread_create(&two, NULL, land, &client);
    pthread_join(one, NULL);
    pthread_join(two, NULL);
    CHECK(float_at(server.ps + Move_Ps_Velocity) == 100.0f && float_at(server.ps + Move_Ps_Velocity + 4) == 50.0f);
    CHECK(float_at(client.ps + Move_Ps_Velocity) == -30.0f && float_at(client.ps + Move_Ps_Velocity + 4) == 250.0f);
}

// ---- a jump and a landing in a small model of the game, from the Mac's code: Jump_Start's height test and its
// cap, the landing, the first frame on the ground. With jumpslowdown on, five jumps in a row are as high and as fast
// as the first; with the game's own rules each one right after a landing is lower and every landing slower.
typedef struct {
    float up, horizontal;
} jump_result_t;
static uint8_t model_ps[Ps_Size], model_pm[0x300], model_pml[0x100];
static jump_result_t model_jump(void) {
    uint8_t *ps = model_ps;
    float squared = (float)int_at(ps + Move_Ps_Gravity) * (39.0f + 39.0f);
    // the height test (0x1014dd129..): flag 18 pending and pm_time at most 1000
    if ((flags_of(ps) & Move_Flag_SlowdownPending) && int_at(ps + Move_Ps_Time) <= 1000 && !handler_Jump_HeightTest()) {
        int32_t time = int_at(ps + Move_Ps_Time);
        float divisor = gadget13 ? 1.0f : time <= 900 ? (float)time * 0.0011098779f + 1.0f : 2.0f;
        squared = squared / divisor;
    }
    set_float(ps, Move_Ps_Velocity + 8, sqrtf(squared));
    // the cap: 280 (both scales 1, no sprint leap)
    detour_registers_t registers = registers_with((uint64_t)ps, 0, (uint64_t)model_pm, 0);
    set_float((uint8_t *)registers.xmm[0], 0, 280.0f);
    handler_Jump_SpeedCap(&registers);
    float cap = float_at(registers.xmm[0]), speed = horizontal_speed(ps);
    if (speed * speed > cap * cap) {
        set_float(ps, Move_Ps_Velocity, float_at(ps + Move_Ps_Velocity) / speed * cap);
        set_float(ps, Move_Ps_Velocity + 4, float_at(ps + Move_Ps_Velocity + 4) / speed * cap);
    }
    set_flags(ps, (flags_of(ps) & ~0x3040000ull) | Move_Flag_SlowdownPending);
    set_int(ps, Move_Ps_Time, 0);
    return (jump_result_t){ float_at(ps + Move_Ps_Velocity + 8), horizontal_speed(ps) };
}
static void model_land(void) {
    uint8_t *ps = model_ps;
    detour_registers_t begin = registers_with((uint64_t)model_pm, (uint64_t)model_pml, 0, 0);
    handler_Landing_Begin(&begin);
    // an ordinary landing (falling at more than 10, no damage): the whole velocity times 0.67, and the dip's event
    float v[3];
    memcpy(v, ps + Move_Ps_Velocity, sizeof(v));
    if (fake_BG_GetViewDip(ps, v[2] * v[2] / (2.0f * 800.0f)) > 0) // the fall as a height (39 here)
        set_int(ps, 0x86c, 1); // stands for the landing event
    v[0] = v[0] * 0.67f, v[1] = v[1] * 0.67f, v[2] = v[2] * 0.67f;
    memcpy(ps + Move_Ps_Velocity, v, sizeof(v));
    detour_registers_t end = registers_with(0, 0, 0, (uint64_t)ps);
    handler_Landing_End(&end);
    // PM_WalkMove's start, and the friction
    if (flags_of(ps) & Move_Flag_SlowdownPending) {
        fake_Jump_SlowDown(ps);
        fake_Jump_ReduceFriction(ps);
    }
}
static void model_wait(int ms) {
    int32_t time = int_at(model_ps + Move_Ps_Time);
    set_int(model_ps, Move_Ps_Time, time > ms ? time - ms : 0);
}
static void model_run(bool is_started, bool jump_slowdown, jump_result_t jumps[5], int *events) {
    memset(model_ps, 0, sizeof(model_ps));
    uint8_t *ps = model_ps;
    memcpy(model_pm, &ps, sizeof(ps));
    set_int(model_ps, Move_Ps_Gravity, 800);
    set_float(model_ps, Move_Ps_Velocity, 190.0f);
    set_movement(is_started, jump_slowdown);
    *events = 0;
    for (int i = 0; i < 5; i++) {
        jumps[i] = model_jump();
        model_land();
        *events += int_at(model_ps + 0x86c);
        set_int(model_ps, 0x86c, 0);
        model_wait(50); // the next jump 50 ms after the landing
    }
}
static void test_model(void) {
    jump_result_t on[5], off[5], permanent[5];
    int events_on, events_off, events_permanent;
    gadget13 = false;
    model_run(true, true, on, &events_on);
    model_run(false, false, off, &events_off);
    model_run(true, false, permanent, &events_permanent);
    float full = sqrtf(800.0f * 78.0f);
    for (int i = 0; i < 5; i++) {
        CHECK(on[i].up == full && on[i].horizontal == 190.0f); // as high and as fast as the first, each time
        CHECK(permanent[i].up == full);                         // the permanent part alone: as high
        if (i > 0) {
            CHECK(off[i].up < off[0].up * 0.75f);                // the game's: much lower right after a landing
            CHECK(off[i].horizontal < off[i - 1].horizontal);    // and slower each time
            CHECK(permanent[i].horizontal < permanent[i - 1].horizontal);
        }
    }
    CHECK(off[0].up == full && off[0].horizontal == 190.0f);
    CHECK(events_on == 0 && events_off == 5 && events_permanent == 5); // no dip, no landing event while on
}

// ---- the debug lines' memory of what was said

static void test_first_time(void) {
    uint8_t a[4], b[4];
    CHECK(first_time(Seen_Jump, a, 100));
    CHECK(!first_time(Seen_Jump, a, 100));
    CHECK(first_time(Seen_Jump, b, 100));
    CHECK(first_time(Seen_Jump, a, 101));
    CHECK(first_time(Seen_Landing, a, 100)); // each kind of line its own
    for (int i = 0; i < Seen_Remembered; i++)
        CHECK(first_time(Seen_Friction, a, 1000 + i));
    for (int i = 0; i < Seen_Remembered; i++)
        CHECK(!first_time(Seen_Friction, a, 1000 + i)); // all of them remembered
    CHECK(first_time(Seen_Friction, a, 999));
    CHECK(first_time(Seen_Friction, a, 1000)); // the oldest one is forgotten
}

// ---- installing

static void test_init(void) {
    uint64_t base = 0x100000000ull;
    static const uint32_t places[] = { ADDR_BG_GetViewDip, ADDR_Jump_ReduceFriction, ADDR_Jump_SlowDown,
                                       ADDR_Jump_Start_ClampSpeed, ADDR_Jump_Start_SlowdownTest,
                                       ADDR_PM_GroundTrace_CrashLandBegin, ADDR_PM_GroundTrace_CrashLandEnd };
    // the setting off: nothing
    setting = false, have_stubs = true, hooks = 0;
    aae_move_jump_init(base);
    CHECK(hooks == 0);
    // not from the pre-patched executable, or one stub missing: nothing either
    setting = true, have_stubs = false;
    aae_move_jump_init(base);
    CHECK(hooks == 0);
    have_stubs = true;
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        stub_missing_for = places[i], hooks = 0;
        aae_move_jump_init(base);
        CHECK(hooks == 0);
    }
    stub_missing_for = -1, hooks = 0;
    aae_move_jump_init(base);
    CHECK(hooks == 7);
    CHECK(was_hooked(base, ADDR_BG_GetViewDip, (void *)fake_BG_GetViewDip, (void *)&orig_BG_GetViewDip));
    CHECK(was_hooked(base, ADDR_Jump_SlowDown, (void *)fake_Jump_SlowDown, (void *)&orig_Jump_SlowDown));
    CHECK(was_hooked(base, ADDR_Jump_ReduceFriction, (void *)fake_Jump_ReduceFriction,
                     (void *)&orig_Jump_ReduceFriction));
    CHECK(was_hooked(base, ADDR_PM_GroundTrace_CrashLandBegin, (void *)handler_Landing_Begin,
                     (void *)&orig_Landing_Begin));
    CHECK(was_hooked(base, ADDR_PM_GroundTrace_CrashLandEnd, (void *)handler_Landing_End, (void *)&orig_Landing_End));
    CHECK(was_hooked(base, ADDR_Jump_Start_ClampSpeed, (void *)handler_Jump_SpeedCap, (void *)&orig_Jump_SpeedCap));
    CHECK(was_hooked(base, ADDR_Jump_Start_SlowdownTest, (void *)handler_Jump_HeightTest,
                     (void *)&orig_Jump_HeightTest));
    CHECK(yes_Jump_HeightTest == (void *)(base + ADDR_Jump_Start_SlowdownTest_Yes));
    // the places, as read from the game's file (0x1014dd15b is where the test's yes goes on)
    CHECK(ADDR_BG_GetViewDip == 0x113caec && ADDR_Jump_SlowDown == 0x14dceb2 && ADDR_Jump_ReduceFriction == 0x14dcf3a);
    CHECK(ADDR_PM_GroundTrace_CrashLandBegin == 0x1146389 && ADDR_PM_GroundTrace_CrashLandEnd == 0x114698d);
    CHECK(ADDR_Jump_Start_ClampSpeed == 0x14dd2c0 && ADDR_Jump_Start_SlowdownTest == 0x14dd146);
    CHECK(ADDR_Jump_Start_SlowdownTest_Yes == 0x14dd15b);
}

// ---- the same cases as T7Overcharged's own code (wip/windows/t7o, not in the repository)

static uint64_t cases_hash = 0xcbf29ce484222325ull;
static int cases_lines;
static void cases_line(const char *line) {
    for (const char *c = line; *c != '\0'; c++)
        cases_hash = (cases_hash ^ (uint8_t)*c) * 0x100000001b3ull;
    cases_hash = (cases_hash ^ '\n') * 0x100000001b3ull;
    cases_lines++;
    if (getenv("MOVE_JUMP_CASES") != NULL)
        puts(line);
}
#include "move_jump_side.h"

int main(void) {
    test_init(); // first: the hooks' originals are the made-up game's from here on
    test_layout();
    test_on();
    test_reduce_friction();
    test_landing_rule();
    test_speed_cap();
    test_hooks();
    test_detours();
    test_threads();
    test_model();
    test_first_time();
    // debug lines: they don't change what is done
    debug = true;
    test_hooks();
    test_detours();
    test_model();
    debug = false;
    set_movement(true, true);
    cases_run();
    printf("cases: %d lines, checksum %#018llx\n", cases_lines, (unsigned long long)cases_hash);
    CHECK(cases_lines == Cases_Expected_Lines && cases_hash == Cases_Expected_Hash); // T7Overcharged's own
    if (failures) {
        printf("test_aae_move_jump: %d FAILED\n", failures);
        return 1;
    }
    printf("test_aae_move_jump: all passed\n");
    return 0;
}
