// aae_move_sprint.c: T7Overcharged's rules for sprinting against what its own code gave for the same cases
// (move_sprint_cases.h), and the handlers against a made-up game. The three compares are gone through for the values
// their register can hold (-2 to 2000 ms, both ends of the range and every 65521st value between), with a model of
// x86's compare and conditional jumps standing for the Mac's instructions and for the Windows ones T7Overcharged
// makes of them; omni-movement against a model of T7Overcharged's instructions, one by one. The thunks that call the
// handlers in the game are x86_64 code and have a test of their own (test_detour.c).
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/aae_move_sprint.c"

// ---- what the code asks of the rest of BO3MacFix

// the switches (aae_movement.h), as each test sets them
static struct {
    bool enabled, started, omni;
    bool patch[Move_Patch_Count];
    float strafe;
} switches;
bool aae_movement_enabled(void) {
    return switches.enabled;
}
bool aae_movement_started(void) {
    return switches.started;
}
bool aae_movement_patch_on(move_patch patch) {
    return (unsigned)patch < Move_Patch_Count && switches.patch[patch];
}
bool aae_movement_omni(void) {
    return switches.omni;
}
float aae_movement_strafe_scale(void) {
    return switches.strafe;
}
float aae_movement_fastads_multiplier(void) {
    return 1.4f;
}

// The ways the switches can be, for a change switched by patch (Move_Patch_Count: omni-movement and the strafe
// scale): whether the change is to act then. The one that acts is the component at work with only that switch on.
enum { Nothing_On, At_Work, Others_On, Setting_Off, Not_Started, Switches_Ways };
static bool switch_ways(int way, move_patch patch) {
    memset(&switches, 0, sizeof(switches));
    switches.strafe = 0.667f;
    switches.enabled = way != Setting_Off;
    switches.started = way != Not_Started;
    if (way == Nothing_On) {
        switches.enabled = switches.started = false;
        return false;
    }
    for (int i = 0; i < Move_Patch_Count; i++)
        switches.patch[i] = way == Others_On ? i != (int)patch : i == (int)patch;
    switches.omni = way != Others_On || patch != Move_Patch_Count;
    switches.strafe = 0.9f;
    return way == At_Work;
}

// which places the pre-patched executable has stubs for
static bool have_stubs;
static uint32_t stubs_missing_for; // one of them missing: its place
bool prepatch_has_hook(uint32_t offset) {
    return have_stubs && offset != stubs_missing_for;
}
static struct {
    void *address, *handler, **original;
} hooked[8];
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 8)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    return 0;
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

// ---- x86's compare and conditional jumps, from the flags as the processor sets them

typedef struct {
    bool zero, sign, overflow;
} flags_t;
// "cmp a, b" on 32-bit registers
static flags_t compare(int32_t a, int32_t b) {
    uint32_t left = (uint32_t)a, right = (uint32_t)b, result = left - right;
    return (flags_t){ result == 0, result >> 31, ((left ^ right) & (left ^ result)) >> 31 };
}
// "test a, a"
static flags_t test(int32_t a) {
    return (flags_t){ a == 0, (uint32_t)a >> 31, false };
}
static bool jg(flags_t flags) {
    return !flags.zero && flags.sign == flags.overflow;
}
static bool jge(flags_t flags) {
    return flags.sign == flags.overflow;
}
static bool jl(flags_t flags) {
    return flags.sign != flags.overflow;
}
static bool jle(flags_t flags) {
    return flags.zero || flags.sign != flags.overflow;
}
static bool je(flags_t flags) {
    return flags.zero;
}

// T7Overcharged's Windows instructions: Jump_Start's two "cmp eax, 1500 ; jg" become "cmp eax, 500 ; jg", the move
// speed's "cmp ecx, 350 ; jge" becomes "cmp ecx, 10 ; jge", and the "je" behind "cmp [rbx + 0x8c], 3" becomes nops
static bool windows_leap_full(int32_t elapsed, bool fast) {
    return jg(compare(elapsed, fast ? 500 : 1500));
}
static bool windows_lerp_done(int32_t elapsed, bool fast) {
    return jge(compare(elapsed, fast ? 10 : 350));
}
static bool windows_slide_blocks(int32_t state, bool fast) {
    return !fast && je(compare(state, 3));
}
// the Mac's own instructions
static bool mac_leap_full(int32_t elapsed) {
    return jg(compare(elapsed, 1500));
}
static bool mac_lerp_done(int32_t elapsed) {
    return jg(compare(elapsed, 349));
}
static bool mac_slide_blocks(int32_t state) {
    return je(compare(state, 3));
}

// T7O 0x1800f2b17 to 0x1800f2b52, instruction by instruction: what its wrapper gives PM_UpdateSprint
static void windows_omni(int8_t forward, int8_t right, uint32_t buttons, int8_t *seen_forward, uint32_t *seen_buttons) {
    int32_t esi = forward, eax = right, edx = esi, ecx = eax;
    uint32_t edi = buttons;
    ecx = (int32_t)(0u - (uint32_t)ecx); // neg ecx
    if (test(ecx).sign)                  // cmovs ecx, eax
        ecx = eax;
    edx = (int32_t)(0u - (uint32_t)edx); // neg edx
    eax = 0x7f;                          // mov eax, 0x7f
    if (test(edx).sign)                  // cmovs edx, esi
        edx = esi;
    if (jl(compare(edx, ecx))) // cmp edx, ecx ; cmovl edx, ecx
        edx = ecx;
    if (jg(compare(edx, eax))) // cmp edx, eax ; cmovg edx, eax
        edx = eax;
    *seen_forward = (int8_t)(uint8_t)edx; // mov [rbx + 0x40], dl
    *seen_buttons = buttons;
    if ((edi >> 18 & 1) != 0 && !jle(test(edx))) // bt edi, 0x12 ; jae ; test edx, edx ; jle
        *seen_buttons = edi | 1u << 30;          // bts eax, 0x1e ; mov [rbx + 0xc], eax
}

// the values a compared register is tried with
static int32_t values[2003 + 16 + 65552]; // the last part: 2^32 / 65521, rounded up
static int value_count;
static void make_values(void) {
    static const int32_t ends[] = { INT32_MIN, INT32_MIN + 1, -1501, -1500, -501, -500, -350, -349, -11, -10, -3,
                                    2001, 65535, 1 << 30, INT32_MAX - 1, INT32_MAX };
    value_count = 0;
    for (int32_t value = -2; value <= 2000; value++)
        values[value_count++] = value;
    for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++)
        values[value_count++] = ends[i];
    for (int64_t value = INT32_MIN; value <= INT32_MAX; value += 65521)
        values[value_count++] = (int32_t)value;
}

// ---- the made-up game

// the game's layout and bits, written out here rather than taken from the file, so that a wrong one there shows
#define Game_Pm_ServerTime 0x8
#define Game_Pm_Buttons 0xc
#define Game_Pm_Forward 0x40
#define Game_Pm_Right 0x41
#define Game_Ps_Flags 0x10
#define Game_Ps_Velocity 0x3c
#define Game_Ps_MoveState 0x8c
#define Game_Sprinting 0x20
#define Game_Sprint_Key (1u << 18)
#define Game_Sprint_Button (1u << 30)
#define Game_Cg_Size 0x342a80

static uint8_t player[0x900], pm_buffer[0x60], pml_buffer[0x80];
// the first local client's cg_t, a whole one: what is in its range counts as the client's
static uint8_t *game_cg;
static void set_pm(uint8_t *ps, int8_t forward, int8_t right, uint32_t buttons) {
    memset(pm_buffer, 0x33, sizeof(pm_buffer));
    memcpy(pm_buffer, &ps, sizeof(ps));
    pm_buffer[Game_Pm_Forward] = (uint8_t)forward;
    pm_buffer[Game_Pm_Right] = (uint8_t)right;
    memcpy(pm_buffer + Game_Pm_Buttons, &buttons, sizeof(buttons));
}
static uint32_t pm_buttons(void) {
    uint32_t buttons;
    memcpy(&buttons, pm_buffer + Game_Pm_Buttons, sizeof(buttons));
    return buttons;
}
static uint8_t *game_player_of(const uint8_t *pm) {
    uint8_t *ps;
    memcpy(&ps, pm, sizeof(ps));
    return ps;
}
static bool game_sprinting(const uint8_t *ps) {
    uint64_t flags;
    memcpy(&flags, ps + Game_Ps_Flags, sizeof(flags));
    return (flags & Game_Sprinting) != 0;
}
static void set_flags(uint8_t *ps, uint64_t flags) {
    memcpy(ps + Game_Ps_Flags, &flags, sizeof(flags));
}

// the game's sprint decision: notes what it was given, sprints for a forward stick above 105 with the sprint button
// (as the game's does, among other things) and changes the sticks and the buttons, as the game's may
static struct {
    int calls;
    int8_t forward, right;
    uint32_t buttons;
    uint8_t *pml;
} decided;
static void game_decision(uint8_t *pm, uint8_t *pml) {
    uint8_t *ps = game_player_of(pm);
    uint64_t flags;
    uint32_t buttons;
    decided.calls++;
    decided.forward = (int8_t)pm[Game_Pm_Forward];
    decided.right = (int8_t)pm[Game_Pm_Right];
    memcpy(&buttons, pm + Game_Pm_Buttons, sizeof(buttons));
    decided.buttons = buttons;
    decided.pml = pml;
    memcpy(&flags, ps + Game_Ps_Flags, sizeof(flags));
    flags = decided.forward > 105 && (buttons & Game_Sprint_Button) ? flags | Game_Sprinting : flags & ~(uint64_t)Game_Sprinting;
    memcpy(ps + Game_Ps_Flags, &flags, sizeof(flags));
    pm[Game_Pm_Forward] ^= 0x0f;
    pm[Game_Pm_Right] ^= 0x11;
    buttons ^= 0x5;
    memcpy(pm + Game_Pm_Buttons, &buttons, sizeof(buttons));
}

// what a handler's run left of the game's registers, besides what it may change
static detour_registers_t registers_before(void) {
    detour_registers_t registers;
    memset(&registers, 0x5a, sizeof(registers));
    return registers;
}

// ---- the rules

static void test_compare_rules(void) {
    for (int i = 0; i < value_count; i++) {
        int32_t value = values[i];
        for (int fast = 0; fast <= 1; fast++) {
            CHECK(aae_move_sprint_leap_full(value, fast) == windows_leap_full(value, fast));
            CHECK(aae_move_sprint_lerp_done(value, fast) == windows_lerp_done(value, fast));
            CHECK(aae_move_sprint_slide_state_blocks(value, fast) == windows_slide_blocks(value, fast));
            // the Mac's own compare on what the register is made to hold comes to T7Overcharged's answer, and where
            // the game goes on with the register it holds what it held
            int32_t leap = aae_move_sprint_leap_register(value, fast), lerp = aae_move_sprint_lerp_register(value, fast);
            CHECK(mac_leap_full(leap) == windows_leap_full(value, fast));
            CHECK(mac_leap_full(leap) || leap == value);
            CHECK(mac_lerp_done(lerp) == windows_lerp_done(value, fast));
            CHECK(mac_lerp_done(lerp) || lerp == value);
            // with the patch off nothing is changed at all
            if (!fast)
                CHECK(leap == value && lerp == value);
        }
    }
    // the edges, in words: T7Overcharged's 500 and 10, the game's 1500 and 350
    CHECK(!aae_move_sprint_leap_full(500, true) && aae_move_sprint_leap_full(501, true));
    CHECK(!aae_move_sprint_leap_full(1500, false) && aae_move_sprint_leap_full(1501, false));
    CHECK(!aae_move_sprint_lerp_done(9, true) && aae_move_sprint_lerp_done(10, true));
    CHECK(!aae_move_sprint_lerp_done(349, false) && aae_move_sprint_lerp_done(350, false));
    CHECK(aae_move_sprint_slide_state_blocks(3, false) && !aae_move_sprint_slide_state_blocks(3, true));
}

static void test_omni_rules(void) {
    static const uint32_t patterns[] = { 0, Game_Sprint_Key, Game_Sprint_Button, Game_Sprint_Key | Game_Sprint_Button,
                                         0xffffffffu, 0xffffffffu & ~Game_Sprint_Key, 0x12345678u };
    for (int forward = -128; forward <= 127; forward++) {
        for (int right = -128; right <= 127; right++) {
            int8_t seen = aae_move_sprint_omni_forward((int8_t)forward, (int8_t)right), windows_seen;
            for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
                uint32_t windows_buttons;
                windows_omni((int8_t)forward, (int8_t)right, patterns[p], &windows_seen, &windows_buttons);
                CHECK(seen == windows_seen);
                CHECK(aae_move_sprint_omni_buttons(patterns[p], seen) == windows_buttons);
            }
        }
    }
    // in words: sideways and back count as forward, -128 as 127; the sprint key needs a stick that is moved
    CHECK(aae_move_sprint_omni_forward(0, -127) == 127 && aae_move_sprint_omni_forward(-100, 30) == 100);
    CHECK(aae_move_sprint_omni_forward(-128, 0) == 127 && aae_move_sprint_omni_forward(0, -128) == 127);
    CHECK(aae_move_sprint_omni_forward(0, 0) == 0 && aae_move_sprint_omni_forward(5, -6) == 6);
    CHECK(aae_move_sprint_omni_buttons(Game_Sprint_Key, 1) == (Game_Sprint_Key | Game_Sprint_Button));
    CHECK(aae_move_sprint_omni_buttons(Game_Sprint_Key, 0) == Game_Sprint_Key);
    CHECK(aae_move_sprint_omni_buttons(0, 127) == 0);
}

static void test_omni_wrapper(void) {
    uint8_t before[sizeof(pm_buffer)];
    // off: the decision gets the move as it is, and what it does to it stays
    set_pm(player, 0, -127, Game_Sprint_Key);
    memcpy(before, pm_buffer, sizeof(before));
    memset(&decided, 0, sizeof(decided));
    aae_move_sprint_omni(pm_buffer, pml_buffer, false, game_decision);
    CHECK(decided.calls == 1 && decided.pml == pml_buffer);
    CHECK(decided.forward == 0 && decided.right == -127 && decided.buttons == Game_Sprint_Key);
    CHECK(pm_buffer[Game_Pm_Forward] == 0x0f && (int8_t)pm_buffer[Game_Pm_Right] == (-127 ^ 0x11));
    CHECK(pm_buttons() == (Game_Sprint_Key ^ 0x5));
    // on: the decision sees forward 127 and the sprint button; afterwards the forward stick and the buttons are as
    // they were, the side stick as the decision left it
    set_pm(player, 0, -127, Game_Sprint_Key);
    memset(&decided, 0, sizeof(decided));
    aae_move_sprint_omni(pm_buffer, pml_buffer, true, game_decision);
    CHECK(decided.calls == 1 && decided.pml == pml_buffer);
    CHECK(decided.forward == 127 && decided.right == -127 && decided.buttons == (Game_Sprint_Key | Game_Sprint_Button));
    CHECK(pm_buffer[Game_Pm_Forward] == 0 && pm_buttons() == Game_Sprint_Key);
    CHECK((int8_t)pm_buffer[Game_Pm_Right] == (-127 ^ 0x11));
    for (size_t i = 0; i < sizeof(pm_buffer); i++)
        if (i != Game_Pm_Right)
            CHECK(pm_buffer[i] == before[i]);
    // every stick: the decision sees T7Overcharged's, the move gets back its own
    for (int forward = -128; forward <= 127; forward += 3) {
        for (int right = -128; right <= 127; right++) {
            int8_t windows_seen;
            uint32_t windows_buttons, buttons = (uint32_t)(forward + 128) * 0x01010101u ^ (right & 1 ? Game_Sprint_Key : 0);
            windows_omni((int8_t)forward, (int8_t)right, buttons, &windows_seen, &windows_buttons);
            set_pm(player, (int8_t)forward, (int8_t)right, buttons);
            aae_move_sprint_omni(pm_buffer, pml_buffer, true, game_decision);
            CHECK(decided.forward == windows_seen && decided.buttons == windows_buttons);
            CHECK((int8_t)pm_buffer[Game_Pm_Forward] == forward && pm_buttons() == buttons);
        }
    }
}

static void test_strafe_rule(void) {
    static const struct {
        uint32_t dvar, scale;
    } cases[] = {
        { 0x3f666666, 0x3f666666 }, // 0.9: the mod's
        { 0x3f7851ec, 0x3f7851ec }, // 0.97: the dvar's top
        { 0x3f2ac083, 0x3f2ac083 }, // 0.667 itself
        { 0x3f2ac084, 0x3f2ac084 }, // just above
        { 0x3f2ac082, 0x3f2ac083 }, // just below
        { 0x3f000000, 0x3f2ac083 }, // 0.5
        { 0x00000000, 0x3f2ac083 }, { 0x80000000, 0x3f2ac083 }, { 0xbf800000, 0x3f2ac083 },
        { 0x7f800000, 0x7f800000 }, // an infinity is above
        { 0xff800000, 0x3f2ac083 },
        { 0x7fc00000, 0x3f2ac083 }, { 0xffc00000, 0x3f2ac083 }, // what isn't a number isn't
        { 0x7f800001, 0x3f2ac083 }, { 0x7fa00000, 0x3f2ac083 }, // signalling, too
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK(bits_of(aae_move_sprint_strafe_scale(float_of(cases[i].dvar))) == cases[i].scale);
}

// ---- the handlers

static void test_strafe_handler(void) {
    for (int way = 0; way < Switches_Ways; way++) {
        static const float dvars[] = { 0.9f, 0.667f, 0.97f, 0.5f };
        for (size_t d = 0; d < sizeof(dvars) / sizeof(dvars[0]); d++) {
            switch_ways(way, Move_Patch_Count);
            // the scale has no switch of its own: T7Overcharged's is in for good from its start, by the dvar's value
            bool acts = way == At_Work || way == Others_On;
            switches.strafe = dvars[d];
            for (int right = -128; right <= 127; right++) {
                // the game's value: its 0.667, or another one to show that it is left alone
                float game_value = right & 1 ? 0.667f : 0.25f, frame_right = (float)right, scale;
                uint8_t frame[0x40];
                detour_registers_t registers = registers_before(), before;
                memcpy(registers.xmm[0], &game_value, sizeof(game_value));
                memcpy(frame + 0x40 - 0x2c, &frame_right, sizeof(frame_right));
                set_pm(player, 0, (int8_t)right, 0);
                registers.rbx = (uint64_t)pm_buffer;
                registers.rbp = (uint64_t)(frame + 0x40);
                before = registers;
                handler_PM_WalkMove_StrafeScaled(&registers);
                memcpy(&scale, registers.xmm[0], sizeof(scale));
                // only xmm0's first float may have changed
                CHECK(memcmp(registers.xmm[0] + 4, before.xmm[0] + 4, 12) == 0);
                CHECK(memcmp(registers.xmm[1], before.xmm[1], sizeof(registers) - sizeof(registers.xmm[0])) == 0);
                float expected = acts ? aae_move_sprint_strafe_scale(dvars[d]) : game_value;
                CHECK(bits_of(scale) == bits_of(expected));
                // the game goes on: mulss xmm0, [rbp - 0x2c] ; cvttss2si eax, xmm0 ; mov [rbx + 0x41], al. That is
                // T7Overcharged's sum on Windows: its scale times the side stick, cut to a whole number.
                float frame_value;
                memcpy(&frame_value, (uint8_t *)registers.rbp - 0x2c, sizeof(frame_value));
                int8_t side = (int8_t)(int32_t)(scale * frame_value);
                if (acts)
                    CHECK(side == (int8_t)(int32_t)(aae_move_sprint_strafe_scale(dvars[d]) * (float)right));
                else
                    CHECK(side == (int8_t)(int32_t)(game_value * (float)right));
            }
        }
    }
    // in words: the mod's 0.9 makes a full side stick 114 instead of the game's 84
    switch_ways(At_Work, Move_Patch_Count);
    detour_registers_t registers = registers_before();
    float game_value = 0.667f, scale;
    memcpy(registers.xmm[0], &game_value, sizeof(game_value));
    set_pm(player, 0, 127, 0);
    registers.rbx = (uint64_t)pm_buffer;
    handler_PM_WalkMove_StrafeScaled(&registers);
    memcpy(&scale, registers.xmm[0], sizeof(scale));
    CHECK((int32_t)(scale * 127.0f) == 114 && (int32_t)(game_value * 127.0f) == 84);
    CHECK((int32_t)(scale * -127.0f) == -114);
}

// a compare's handler on every value: what the Mac's compare then decides, against T7Overcharged's instruction
typedef void (*handler_t)(detour_registers_t *);
static void try_compare_handler(handler_t handler, bool use_rcx, move_patch patch, bool (*mac)(int32_t),
                                bool (*windows)(int32_t, bool)) {
    for (int way = 0; way < Switches_Ways; way++) {
        bool acts = switch_ways(way, patch);
        for (int i = 0; i < value_count; i++) {
            int32_t value = values[i];
            detour_registers_t registers = registers_before(), before;
            if (use_rcx)
                registers.rcx = (uint32_t)value;
            else
                registers.rax = (uint32_t)value;
            registers.r14 = (uint64_t)pm_buffer;
            registers.r15 = (uint64_t)player;
            before = registers;
            handler(&registers);
            uint64_t held = use_rcx ? registers.rcx : registers.rax;
            // nothing but the compared register changes, and it holds a 32-bit value as the game's sub left it
            if (use_rcx)
                registers.rcx = before.rcx;
            else
                registers.rax = before.rax;
            CHECK(memcmp(&registers, &before, sizeof(registers)) == 0);
            CHECK(held >> 32 == 0);
            bool taken = mac((int32_t)held);
            CHECK(taken == windows(value, acts));
            CHECK(taken || held == (uint32_t)value);
            if (!acts)
                CHECK(held == (uint32_t)value);
        }
    }
}

// the slide state's handler: yes goes on behind the je, no runs the game's compare and je
static void test_slide_handler(void) {
    for (int way = 0; way < Switches_Ways; way++) {
        bool acts = switch_ways(way, Move_CanSlideThenSprint);
        for (int i = 0; i < value_count; i++) {
            int32_t state = values[i];
            memcpy(player + Game_Ps_MoveState, &state, sizeof(state));
            bool yes = handler_PM_UpdateSprint_SlideStateCmp((uint64_t)player, 0x77);
            CHECK(yes == acts);
            bool blocked = !yes && mac_slide_blocks(state);
            CHECK(blocked == windows_slide_blocks(state, acts));
        }
    }
}

// the hook on PM_UpdateSprint: T7Overcharged's wrapper while it is at work with omni-movement on, the game's own
// decision otherwise
static void test_decision_hook(void) {
    orig_PM_UpdateSprint = game_decision;
    for (int way = 0; way < Switches_Ways; way++) {
        bool acts = switch_ways(way, Move_Patch_Count);
        set_flags(player, 0);
        set_pm(player, 0, -127, Game_Sprint_Key);
        memset(&decided, 0, sizeof(decided));
        fake_PM_UpdateSprint(pm_buffer, pml_buffer);
        CHECK(decided.calls == 1 && decided.pml == pml_buffer);
        CHECK(decided.forward == (acts ? 127 : 0) && decided.right == -127);
        CHECK(decided.buttons == (acts ? Game_Sprint_Key | Game_Sprint_Button : Game_Sprint_Key));
        // sprinting fully left with the sprint key: only with T7Overcharged's wrapper
        CHECK(game_sprinting(player) == acts);
        CHECK(pm_buffer[Game_Pm_Forward] == (acts ? 0 : 0x0f));
        CHECK(pm_buttons() == (acts ? Game_Sprint_Key : Game_Sprint_Key ^ 0x5));
    }
    // omni-movement's own switch, asked every time
    switch_ways(At_Work, Move_Patch_Count);
    switches.omni = false;
    set_pm(player, 0, -127, Game_Sprint_Key);
    fake_PM_UpdateSprint(pm_buffer, pml_buffer);
    CHECK(decided.forward == 0 && decided.buttons == Game_Sprint_Key);
    switches.omni = true;
    set_pm(player, -127, 0, Game_Sprint_Key);
    fake_PM_UpdateSprint(pm_buffer, pml_buffer);
    CHECK(decided.forward == 127 && decided.buttons == (Game_Sprint_Key | Game_Sprint_Button));
    CHECK((int8_t)pm_buffer[Game_Pm_Forward] == -127);
}

// ---- what is printed

static int capture_saved = -1, capture_pipe[2];
static void capture_begin(void) {
    fflush(stdout);
    if (pipe(capture_pipe) != 0)
        return;
    fcntl(capture_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(capture_pipe[0], F_SETFL, O_NONBLOCK);
    capture_saved = dup(1);
    dup2(capture_pipe[1], 1);
}
// what was printed since capture_begin
static const char *capture_end(void) {
    static char text[16384];
    ssize_t got = 0, part;
    fflush(stdout);
    dup2(capture_saved, 1);
    close(capture_saved);
    close(capture_pipe[1]);
    while ((part = read(capture_pipe[0], text + got, sizeof(text) - 1 - (size_t)got)) > 0)
        got += part;
    close(capture_pipe[0]);
    text[got] = '\0';
    return text;
}
static int lines_with(const char *text, const char *part) {
    int count = 0;
    for (const char *at = text; (at = strstr(at, part)) != NULL; at += strlen(part))
        count++;
    return count;
}

// ---- putting them in

static void test_init(void) {
    const uint64_t base = 0x100000000ULL;
    struct {
        uint32_t offset;
        void *handler, **original;
    } expected[] = {
        { ADDR_PM_UpdateSprint, (void *)fake_PM_UpdateSprint, (void **)&orig_PM_UpdateSprint },
        { ADDR_PM_WalkMove_StrafeScaled, (void *)handler_PM_WalkMove_StrafeScaled, &orig_PM_WalkMove_StrafeScaled },
        { ADDR_PM_CmdScale_SprintLerpCmp, (void *)handler_PM_CmdScale_SprintLerpCmp, &orig_PM_CmdScale_SprintLerpCmp },
        { ADDR_PM_UpdateSprint_SlideStateCmp, (void *)handler_PM_UpdateSprint_SlideStateCmp,
          &orig_PM_UpdateSprint_SlideStateCmp },
        { ADDR_Jump_Start_SprintLeapCmp1, (void *)handler_Jump_Start_SprintLeapCmp1, &orig_Jump_Start_SprintLeapCmp1 },
        { ADDR_Jump_Start_SprintLeapCmp2, (void *)handler_Jump_Start_SprintLeapCmp2, &orig_Jump_Start_SprintLeapCmp2 },
    };
    unsetenv("BO3MACFIX_MOVEDEBUG");
    // the setting off: nothing
    switch_ways(Setting_Off, Move_Patch_Count);
    have_stubs = true, hooks = 0;
    aae_move_sprint_init(base);
    CHECK(hooks == 0);
    // not from the pre-patched executable: nothing
    switch_ways(At_Work, Move_Patch_Count);
    have_stubs = false, hooks = 0;
    capture_begin();
    aae_move_sprint_init(base);
    CHECK(lines_with(capture_end(), "movement: not running from the pre-patched executable") == 1);
    CHECK(hooks == 0);
    // one stub missing: nothing
    have_stubs = true;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        stubs_missing_for = expected[i].offset, hooks = 0;
        capture_begin();
        aae_move_sprint_init(base);
        CHECK(lines_with(capture_end(), "movement: not running from the pre-patched executable") == 1);
        CHECK(hooks == 0);
    }
    stubs_missing_for = 0;
    // the setting on: all six, at their places
    have_stubs = true, hooks = 0;
    aae_move_sprint_init(base);
    CHECK(hooks == 6);
    for (int i = 0; i < 6 && i < hooks; i++) {
        CHECK(hooked[i].address == (void *)(uintptr_t)(base + expected[i].offset));
        CHECK(hooked[i].handler == expected[i].handler);
        CHECK(hooked[i].original == expected[i].original);
    }
    CHECK(other_PM_UpdateSprint_SlideStateCmp == (void *)(uintptr_t)(base + ADDR_PM_UpdateSprint_SlideStateCmp_Past));
    CHECK(ADDR_PM_UpdateSprint_SlideStateCmp_Past == ADDR_PM_UpdateSprint_SlideStateCmp + 13); // the cmp and the je
    CHECK(debug == 0);
    // a debug run with the setting off: in, to show the game's own numbers
    setenv("BO3MACFIX_MOVEDEBUG", "1", 1);
    switch_ways(Setting_Off, Move_Patch_Count);
    hooks = 0;
    capture_begin();
    aae_move_sprint_init(base);
    CHECK(lines_with(capture_end(), "movement: sprint: the hooks are in (AAEMovement is off: they only show the game's "
                                    "own numbers)") == 1);
    CHECK(hooks == 6 && debug == 1);
    CHECK(cg_clients == (uint8_t *const *)(uintptr_t)(base + ADDR_cgArray));
    unsetenv("BO3MACFIX_MOVEDEBUG");
    debug = 0;
    cg_clients = &game_cg;
}

// ---- the debug lines

static void test_debug_lines(void) {
    debug = 1;
    cg_clients = &game_cg;
    memset(said, 0, sizeof(said));
    orig_PM_UpdateSprint = game_decision;
    switch_ways(At_Work, Move_Patch_Count);
    for (int i = 0; i < Move_Patch_Count; i++)
        switches.patch[i] = true;
    set_flags(player, 0);
    capture_begin();
    // the decision: a line when the way the stick is held, the keys or the sprinting change, not for every move
    for (int i = 0; i < 3; i++) {
        set_pm(player, 0, -127, Game_Sprint_Key);
        fake_PM_UpdateSprint(pm_buffer, pml_buffer);
    }
    set_pm(player, 0, -126, Game_Sprint_Key);
    fake_PM_UpdateSprint(pm_buffer, pml_buffer);
    set_pm(player, 0, 0, Game_Sprint_Key);
    fake_PM_UpdateSprint(pm_buffer, pml_buffer);
    const char *text = capture_end();
    CHECK(lines_with(text, "movement: sprint decision") == 2);
    CHECK(lines_with(text, "movement: sprint decision (server): stick forward 0 side -127, sprint key held, sprint "
                           "button not held; seen as forward 127, sprint button held (omni-movement); sprinting: yes") == 1);
    CHECK(lines_with(text, "stick forward 0 side 0, sprint key held, sprint button not held; seen as forward 0, sprint "
                           "button not held (omni-movement); sprinting: no") == 1);
    // the game's own: no sprint fully sideways
    capture_begin();
    switches.omni = false;
    set_pm(player, 0, -127, Game_Sprint_Key);
    fake_PM_UpdateSprint(pm_buffer, pml_buffer);
    text = capture_end();
    CHECK(lines_with(text, "side -127, sprint key held, sprint button not held; seen as it is (omni-movement off); "
                           "sprinting: no") == 1);

    // the strafe: a line for each side stick and scale
    capture_begin();
    for (int i = 0; i < 3; i++) {
        detour_registers_t registers = registers_before();
        float game_value = 0.667f;
        memcpy(registers.xmm[0], &game_value, sizeof(game_value));
        set_pm(player, 127, i < 2 ? -127 : 127, 0);
        registers.rbx = (uint64_t)pm_buffer;
        handler_PM_WalkMove_StrafeScaled(&registers);
    }
    // the same side stick with another scale
    switches.strafe = 0.97f;
    detour_registers_t again = registers_before();
    float game_value = 0.667f;
    memcpy(again.xmm[0], &game_value, sizeof(game_value));
    again.rbx = (uint64_t)pm_buffer;
    handler_PM_WalkMove_StrafeScaled(&again);
    switches.strafe = 0.9f;
    text = capture_end();
    CHECK(lines_with(text, "movement: strafe while sprinting") == 3);
    CHECK(lines_with(text, "side stick 127 becomes 123, scale 0.970") == 1);
    CHECK(lines_with(text, "(server): side stick -127 becomes -114, scale 0.900 (T7Overcharged's), the game's own 0.667 "
                           "makes it -84") == 1);
    CHECK(lines_with(text, "side stick 127 becomes 114") == 1);

    // the speed: a line when the ramp ends, and when a new sprint begins with one
    capture_begin();
    static const int32_t times[] = { 0, 5, 12, 40, 0, 16 };
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        detour_registers_t registers = registers_before();
        set_pm(player, 127, 0, 0);
        registers.rax = (uint32_t)times[i];
        registers.r15 = (uint64_t)pm_buffer;
        handler_PM_CmdScale_SprintLerpCmp(&registers);
    }
    text = capture_end();
    CHECK(lines_with(text, "movement: sprint speed") == 4);
    CHECK(lines_with(text, "(server): 0 ms into the sprint still ramping up (fast_sprintlerpin: full from 10 ms)") == 2);
    CHECK(lines_with(text, "(server): 12 ms into the sprint at full speed (fast_sprintlerpin: full from 10 ms)") == 1);
    CHECK(lines_with(text, "16 ms into the sprint at full speed") == 1);

    // the slide state: a line when a sprint is wanted in it
    capture_begin();
    static const int32_t states[] = { 3, 3, 0, 3 };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        memcpy(player + Game_Ps_MoveState, &states[i], sizeof(states[i]));
        handler_PM_UpdateSprint_SlideStateCmp((uint64_t)player, 0);
    }
    switches.patch[Move_CanSlideThenSprint] = false;
    memcpy(player + Game_Ps_MoveState, &states[0], sizeof(states[0]));
    handler_PM_UpdateSprint_SlideStateCmp((uint64_t)player, 0);
    switches.patch[Move_CanSlideThenSprint] = true;
    text = capture_end();
    CHECK(lines_with(text, "movement: slide state (server): a sprint may start in it (canslidethensprint)") == 2);
    CHECK(lines_with(text, "movement: slide state (server): keeps a sprint from starting (the game's own)") == 1);

    // the leap: a line for each jump (its command's time), with the strength
    capture_begin();
    static const struct {
        int32_t time, elapsed;
        bool fast;
    } jumps[] = { { 1000, 750, true }, { 1000, 750, true }, { 2000, 750, false }, { 3000, 300, true } };
    float velocity[3] = { 200.0f, -10.0f, 282.8f };
    memcpy(player + Game_Ps_Velocity, velocity, sizeof(velocity));
    for (size_t i = 0; i < sizeof(jumps) / sizeof(jumps[0]); i++) {
        detour_registers_t registers = registers_before();
        switches.patch[Move_FastSprintLeap] = jumps[i].fast;
        set_pm(player, 127, 0, 0);
        memcpy(pm_buffer + Game_Pm_ServerTime, &jumps[i].time, sizeof(jumps[i].time));
        registers.rax = (uint32_t)jumps[i].elapsed;
        registers.r14 = (uint64_t)pm_buffer;
        registers.r15 = (uint64_t)player;
        handler_Jump_Start_SprintLeapCmp2(&registers);
    }
    text = capture_end();
    CHECK(lines_with(text, "movement: sprint leap") == 3);
    CHECK(lines_with(text, "(server): a jump 750 ms into the sprint at strength 1.000 (fast_sprintleap: full after "
                           "500 ms), the game's own 0.500; speed up 282.8, along 200.0 -10.0") == 1);
    CHECK(lines_with(text, "a jump 750 ms into the sprint at strength 0.500 (the game's own: full after 1500 ms), the "
                           "game's own 0.500") == 1);
    CHECK(lines_with(text, "a jump 300 ms into the sprint at strength 0.200") == 1);

    // the client's prediction: its player state is in the first local client's cg_t
    capture_begin();
    memset(said, 0, sizeof(said));
    set_pm(game_cg + 0x100, 0, 127, Game_Sprint_Key);
    fake_PM_UpdateSprint(pm_buffer, pml_buffer);
    text = capture_end();
    CHECK(lines_with(text, "movement: sprint decision (client)") == 1);
    debug = 0;
}

// ---- the same cases T7Overcharged's own functions were given (see move_sprint_cases.h), and what that gave

#define T7O_Lines 121257
#define T7O_Checksum 0xa3566fc5d818c9b5ULL
static struct {
    uint64_t checksum;
    int lines;
} cases_out;
static void cases_byte(uint8_t byte) {
    cases_out.checksum = (cases_out.checksum ^ byte) * 0x100000001b3ULL;
}
static void cases_line(const char *line) {
    for (; *line != '\0'; line++)
        cases_byte((uint8_t)*line);
    cases_byte('\n');
    cases_out.lines++;
}
#include "move_sprint_cases.h"
static void side_omni(uint8_t *pm, uint8_t *pml, int dvar, void (*decision)(uint8_t *pm, uint8_t *pml)) {
    aae_move_sprint_omni(pm, pml, dvar != Cases_No_Dvar && dvar != 0, decision);
}
static float side_strafe(float dvar_value) {
    return aae_move_sprint_strafe_scale(dvar_value);
}
static void test_like_t7overcharged(void) {
    cases_out.checksum = 0xcbf29ce484222325ULL, cases_out.lines = 0;
    cases_run();
    CHECK(cases_out.lines == T7O_Lines);
    CHECK(cases_out.checksum == T7O_Checksum);
    if (cases_out.checksum != T7O_Checksum)
        printf("  the cases came out as %#018llx in %d lines: wip/agents/movea/wine/compare.sh shows where they differ "
               "from T7Overcharged's\n", (unsigned long long)cases_out.checksum, cases_out.lines);
}

int main(void) {
    make_values();
    game_cg = calloc(1, Game_Cg_Size);
    cg_clients = &game_cg;
    test_compare_rules();
    test_omni_rules();
    test_omni_wrapper();
    test_strafe_rule();
    test_strafe_handler();
    try_compare_handler(handler_PM_CmdScale_SprintLerpCmp, false, Move_FastSprintLerpIn, mac_lerp_done,
                        windows_lerp_done);
    try_compare_handler(handler_Jump_Start_SprintLeapCmp1, true, Move_FastSprintLeap, mac_leap_full, windows_leap_full);
    try_compare_handler(handler_Jump_Start_SprintLeapCmp2, false, Move_FastSprintLeap, mac_leap_full,
                        windows_leap_full);
    test_slide_handler();
    test_decision_hook();
    test_init();
    test_debug_lines();
    test_like_t7overcharged();
    if (failures != 0) {
        printf("test_aae_move_sprint: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_move_sprint: all passed\n");
    return 0;
}
