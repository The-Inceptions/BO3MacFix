// aae_move_fastjump.c: T7Overcharged's rules for its patch fastjump, the table of times in the air (also from two
// threads at once), the hooks and detours against a made-up game, the debug lines, and the same cases as
// T7Overcharged's own code under Wine.
#include <limits.h>
#include <pthread.h>
#include <regex.h>
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

#include "../source/aae_move_fastjump.c"

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_THREAD_SANITIZER 1
#endif
#endif

#define Game_Base 0x100000000ULL

// ---- what the code asks of the rest of BO3MacFix: the switches (aae_movement.h) and the pre-patched executable
static bool setting = true, started = true, switched_on = true;
static int asked_for = -1;
bool aae_movement_enabled(void) {
    return setting;
}
bool aae_movement_started(void) {
    return started;
}
bool aae_movement_patch_on(move_patch patch) {
    asked_for = (int)patch;
    return switched_on && patch == Move_FastJump;
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
static void switches(bool on) {
    setting = started = switched_on = on;
}

static bool have_stubs = true;
static uint32_t stub_missing_for;
bool prepatch_has_hook(uint32_t offset) {
    return have_stubs && offset != stub_missing_for;
}

// ---- the made-up game, laid out as the game's disassembly has it (the source's own names for these offsets are
// what is tested): the pmove begins with the player state's address and has the command's time at +8; the player
// state has its movement flags at +0x10 (bit 7 sliding), its velocity at +0x3c and its jump time at +0x110
static uint8_t player[3][0xc00], pmove[3][0x80], pml[0x80];
static void set_player(int which, uint8_t flags, float x, float y) {
    uint8_t *pm = pmove[which], *ps = player[which];
    memcpy(pm, &ps, sizeof(ps));
    ps[0x10] = flags;
    memcpy(ps + 0x3c, &x, sizeof(x));
    memcpy(ps + 0x40, &y, sizeof(y));
}
static void set_times(int which, int32_t command_time, int32_t jump_time) {
    memcpy(pmove[which] + 0x8, &command_time, sizeof(command_time));
    memcpy(player[which] + 0x110, &jump_time, sizeof(jump_time));
}

// PM_Accelerate: what it got
static struct {
    int calls;
    uint8_t *ps, *pml;
    const float *wishdir;
    float direction[3], wish_speed, acceleration;
} accelerated;
static void game_accelerate(uint8_t *ps, uint8_t *pml_, const float *wishdir, float wish_speed, float acceleration) {
    accelerated.calls++;
    accelerated.ps = ps, accelerated.pml = pml_, accelerated.wishdir = wishdir;
    memcpy(accelerated.direction, wishdir, sizeof(accelerated.direction));
    accelerated.wish_speed = wish_speed, accelerated.acceleration = acceleration;
}

// PM_Friction: what it got, and the friction it goes on with at its main call, where the detour is
static struct {
    int calls;
    uint8_t *pml, *pm;
    float friction;
    detour_registers_t before, after;
} frictioned;
static void game_friction(uint8_t *pml_, uint8_t *pm) {
    frictioned.calls++;
    frictioned.pml = pml_, frictioned.pm = pm;
    detour_registers_t *r = &frictioned.after;
    memset(r, 0x5a, sizeof(*r));
    memcpy(&r->rbx, pm, sizeof(r->rbx)); // the player state
    r->r15 = (uint64_t)pm, r->r14 = (uint64_t)pml_;
    const float game = 5.5f;
    memset(r->xmm[0], 0, sizeof(r->xmm[0]));
    memcpy(r->xmm[0], &game, sizeof(game));
    frictioned.before = *r;
    handler_PM_Friction_GotFriction(r);
    memcpy(&frictioned.friction, r->xmm[0], sizeof(frictioned.friction));
}

// PM_IsInAir: what it was asked, and its answer
static struct {
    int calls;
    uint8_t *pm, *pml;
} asked_in_air;
static bool in_air_answer;
static bool game_is_in_air(uint8_t *pm, uint8_t *pml_) {
    asked_in_air.calls++;
    asked_in_air.pm = pm, asked_in_air.pml = pml_;
    return in_air_answer;
}

static uint64_t test_now = 100000;
static uint64_t test_clock(void) {
    return test_now;
}

// the hooking: what was hooked, and the made-up game's functions as the originals
static struct {
    void *address, *replacement, **original;
} hooked[8];
static int hooks, hook_answer;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 8)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    if (out_origin_func == (void **)&orig_PM_Accelerate)
        *out_origin_func = (void *)game_accelerate;
    if (out_origin_func == (void **)&orig_PM_Friction)
        *out_origin_func = (void *)game_friction;
    return hook_answer;
}
static void *replacement_at(uint32_t place) {
    for (int i = 0; i < hooks && i < 8; i++)
        if (hooked[i].address == (void *)(Game_Base + place))
            return hooked[i].replacement;
    return NULL;
}

// the debug lines
static char said[64][300];
static int lines;
static void capture(const char *line) {
    if (lines < 64)
        snprintf(said[lines], sizeof(said[0]), "movement: fastjump: %s", line);
    lines++;
}
static int said_like(const char *pattern) {
    regex_t expression;
    int found = 0;
    if (regcomp(&expression, pattern, REG_EXTENDED | REG_NOSUB) != 0)
        return -1;
    for (int i = 0; i < lines && i < 64; i++)
        found += regexec(&expression, said[i], 0, NULL, 0) == 0;
    regfree(&expression);
    return found;
}

// everything as at the game's start, then init with the made-up game in place
static void start(void) {
    memset(&air_times, 0, sizeof(air_times));
    memset(&watched, 0, sizeof(watched));
    atomic_store(&said_on, -1);
    atomic_store(&said_ground, false);
    memset(hooked, 0, sizeof(hooked));
    hooks = hook_answer = 0;
    dropped = 0;
    orig_PM_Accelerate = NULL, orig_PM_Friction = NULL;
    memset(&accelerated, 0, sizeof(accelerated));
    memset(&frictioned, 0, sizeof(frictioned));
    memset(&asked_in_air, 0, sizeof(asked_in_air));
    switches(true);
    have_stubs = true, stub_missing_for = 0;
    unsetenv("BO3MACFIX_MOVEDEBUG");
    aae_move_fastjump_init(Game_Base);
    PM_IsInAir = game_is_in_air;
    now_ms = test_clock;
    say_line = capture;
    lines = 0;
    for (int i = 0; i < 3; i++)
        set_player(i, 0, 0, 0);
}

static float from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static uint32_t bits_of(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// ---- T7Overcharged's rules

static void accelerate_case(aae_move_fastjump_call call, uint8_t flags, float x, float y, float *wish, float *accel) {
    aae_move_fastjump_accelerate(call, flags, x, y, wish, accel);
}
static bool raised(aae_move_fastjump_call call, uint8_t flags, float x, float y) {
    float wish = 190.0f, accel = 1.0f;
    accelerate_case(call, flags, x, y, &wish, &accel);
    return wish == 360.0f && accel == 4.0f;
}

static void test_accelerate_rule(void) {
    // in the air below 150, from both sides, either axis and both (90 and 120 make exactly 150)
    CHECK(raised(Fastjump_Air_Call, 0, 0, 0));
    CHECK(raised(Fastjump_Air_Call, 0, 149.99998f, 0));
    CHECK(!raised(Fastjump_Air_Call, 0, 150.0f, 0));
    CHECK(!raised(Fastjump_Air_Call, 0, 150.00002f, 0));
    CHECK(raised(Fastjump_Air_Call, 0, 0, -149.99998f));
    CHECK(!raised(Fastjump_Air_Call, 0, 0, -150.0f));
    CHECK(!raised(Fastjump_Air_Call, 0, 90.0f, 120.0f));
    CHECK(raised(Fastjump_Air_Call, 0, 89.99998f, 120.0f));
    CHECK(!raised(Fastjump_Air_Call, 0, 89.99999f, 120.0f)); // its square root rounds to 150
    CHECK(raised(Fastjump_Air_Call, 0, 106.0f, 106.0f));
    CHECK(!raised(Fastjump_Air_Call, 0, 106.1f, 106.1f));
    // not a number, endless: no change (its compare says no)
    CHECK(!raised(Fastjump_Air_Call, 0, from_bits(0x7fc00000), 0));
    CHECK(!raised(Fastjump_Air_Call, 0, 0, from_bits(0xff800000)));
    CHECK(!raised(Fastjump_Air_Call, 0, 3.0e19f, 0)); // its square is too big for a float
    // sliding is bit 7 of the flags, and only that bit
    CHECK(!raised(Fastjump_Air_Call, 0x80, 0, 0));
    CHECK(!raised(Fastjump_Air_Call, 0xff, 0, 0));
    CHECK(raised(Fastjump_Air_Call, 0x7f, 0, 0));
    CHECK(raised(Fastjump_Air_Call, 0x20, 0, 0));
    // the sums: ×4 and +170 in floats
    float wish = 100.1f, accel = 0.25f;
    accelerate_case(Fastjump_Air_Call, 0, 10, 10, &wish, &accel);
    CHECK(bits_of(wish) == bits_of(100.1f + 170.0f) && accel == 1.0f);
    wish = from_bits(0x7fc00000), accel = -2.5f;
    accelerate_case(Fastjump_Air_Call, 0, 10, 10, &wish, &accel);
    CHECK(wish != wish && accel == -10.0f);
    // on the ground: ×1.1111112 (0x3f8e38e4), whatever the speed and flags; the wish speed stays
    wish = 190.0f, accel = 9.0f;
    accelerate_case(Fastjump_Ground_Call, 0x80, 0, 0, &wish, &accel);
    CHECK(wish == 190.0f && bits_of(accel) == bits_of(9.0f * from_bits(0x3f8e38e4)));
    CHECK(accel == 10.0f);
    wish = 7.0f, accel = 12.0f;
    accelerate_case(Fastjump_Ground_Call, 0, 400, 400, &wish, &accel);
    CHECK(wish == 7.0f && bits_of(accel) == bits_of(12.0f * 1.1111112f));
    // any other call: nothing
    wish = 190.0f, accel = 1.0f;
    accelerate_case(Fastjump_Other_Call, 0, 0, 0, &wish, &accel);
    CHECK(wish == 190.0f && accel == 1.0f);
}

static void test_friction_rule(void) {
    // 100 from both sides (its square against 10000), either axis, both
    CHECK(aae_move_fastjump_fast_enough(0, 100.0f, 0));
    CHECK(aae_move_fastjump_fast_enough(0, 0, -100.0f));
    CHECK(aae_move_fastjump_fast_enough(0, 60.0f, 80.0f));
    CHECK(!aae_move_fastjump_fast_enough(0, 99.99999f, 0));
    CHECK(!aae_move_fastjump_fast_enough(0, 59.99999f, 80.0f));
    CHECK(aae_move_fastjump_fast_enough(0, 100.00001f, 0));
    CHECK(!aae_move_fastjump_fast_enough(0, 0, 0));
    // what isn't a number passes (T7Overcharged's "10000 > speed²" says no for it)
    CHECK(aae_move_fastjump_fast_enough(0, from_bits(0x7fc00000), 0));
    CHECK(aae_move_fastjump_fast_enough(0, 3.0e19f, 0));
    // sliding: never
    CHECK(!aae_move_fastjump_fast_enough(0x80, 300.0f, 0));
    CHECK(!aae_move_fastjump_fast_enough(0xff, 300.0f, 0));
    CHECK(aae_move_fastjump_fast_enough(0x7f, 300.0f, 0));
    // 67 ms from both sides
    CHECK(aae_move_fastjump_just_landed(1000, 1000));
    CHECK(aae_move_fastjump_just_landed(1067, 1000));
    CHECK(!aae_move_fastjump_just_landed(1068, 1000));
    // unsigned: across the clock's wrap, and a time ahead of the clock counts as long ago
    CHECK(aae_move_fastjump_just_landed(5, UINT64_MAX - 10));
    CHECK(aae_move_fastjump_just_landed(51, UINT64_MAX - 15));
    CHECK(!aae_move_fastjump_just_landed(52, UINT64_MAX - 15));
    CHECK(!aae_move_fastjump_just_landed(999, 1000));
    CHECK(!aae_move_fastjump_just_landed(0, 1));
}

static void test_delay_rule(void) {
    CHECK(aae_move_fastjump_delay(1) == 500);
    CHECK(aae_move_fastjump_delay(250) == 500);
    CHECK(aae_move_fastjump_delay(499) == 500);
    CHECK(aae_move_fastjump_delay(500) == 500);
    CHECK(aae_move_fastjump_delay(501) == 501);
    CHECK(aae_move_fastjump_delay(0) == 0);
    CHECK(aae_move_fastjump_delay(-1) == -1);
    CHECK(aae_move_fastjump_delay(INT32_MIN) == INT32_MIN);
    CHECK(aae_move_fastjump_delay(INT32_MAX) == INT32_MAX);
#if !defined(UNDER_THREAD_SANITIZER)
    // every time there is: the game's "jl" after "cmp ecx, 500" then refuses exactly what T7Overcharged's
    // "cmp eax, 1" refuses, and only 1 to 499 are changed
    uint64_t wrong = 0;
    for (int64_t t = INT32_MIN; t <= INT32_MAX; t++) {
        int32_t seen = aae_move_fastjump_delay((int32_t)t);
        wrong += (seen < 500) != (t < 1);
        wrong += (t < 1 || t >= 500) && seen != (int32_t)t;
    }
    CHECK(wrong == 0);
#endif
}

static void test_air_table(void) {
    static aae_move_fastjump_air_t air;
    static uint8_t states[Fastjump_Air_Places + 2];
    uint64_t at = 0;
    memset(&air, 0, sizeof(air));
    CHECK(!aae_move_fastjump_air_last(&air, &states[0], &at));
    CHECK(aae_move_fastjump_air_note(&air, &states[0], 500));
    CHECK(aae_move_fastjump_air_last(&air, &states[0], &at) && at == 500);
    CHECK(aae_move_fastjump_air_note(&air, &states[0], 600)); // the same one again: its time, no new place
    CHECK(aae_move_fastjump_air_last(&air, &states[0], &at) && at == 600 && air.count == 1);
    CHECK(!aae_move_fastjump_air_last(&air, &states[1], &at));
    // full: every one seen within the last 67 ms but one, which makes room without loss
    memset(&air, 0, sizeof(air));
    for (int i = 0; i < Fastjump_Air_Places; i++)
        CHECK(aae_move_fastjump_air_note(&air, &states[i], i == 17 ? 1000 : 2000 + (uint64_t)i));
    CHECK(air.count == Fastjump_Air_Places);
    CHECK(aae_move_fastjump_air_note(&air, &states[Fastjump_Air_Places], 2064));
    CHECK(!aae_move_fastjump_air_last(&air, &states[17], &at));
    CHECK(aae_move_fastjump_air_last(&air, &states[Fastjump_Air_Places], &at) && at == 2064);
    for (int i = 0; i < Fastjump_Air_Places; i++)
        if (i != 17)
            CHECK(aae_move_fastjump_air_last(&air, &states[i], &at) && at == 2000 + (uint64_t)i);
    // full of fresh times: the oldest goes, and that is said
    CHECK(!aae_move_fastjump_air_note(&air, &states[Fastjump_Air_Places + 1], 2065));
    CHECK(!aae_move_fastjump_air_last(&air, &states[0], &at));
    CHECK(aae_move_fastjump_air_last(&air, &states[Fastjump_Air_Places + 1], &at) && at == 2065);
    // "oldest" counted unsigned, as the rule: a time ahead of the clock is the one to go
    memset(&air, 0, sizeof(air));
    for (int i = 0; i < Fastjump_Air_Places; i++)
        aae_move_fastjump_air_note(&air, &states[i], 3000 + (uint64_t)i);
    air.at[40] = 9000;
    CHECK(aae_move_fastjump_air_note(&air, &states[Fastjump_Air_Places], 3070));
    CHECK(!aae_move_fastjump_air_last(&air, &states[40], &at));
    CHECK(aae_move_fastjump_air_last(&air, &states[0], &at) && at == 3000);
}

// two threads, as the server's movement and the client's prediction: each notes and reads its own player state and
// one they share. The thread sanitizer's build tells when that isn't under the lock.
static uint8_t shared_state;
typedef struct {
    const void *own;
    int rounds, wrong;
} worker_t;
static void *worker(void *argument) {
    worker_t *w = argument;
    for (int i = 0; i < w->rounds; i++) {
        uint64_t at = 0;
        note_in_air(w->own, 10000 + (uint64_t)i);
        note_in_air(&shared_state, 10000 + (uint64_t)i);
        if (!last_in_air(w->own, &at) || at != 10000 + (uint64_t)i)
            w->wrong++;
        if (!last_in_air(&shared_state, &at) || at < 10000)
            w->wrong++;
    }
    return NULL;
}
static void test_threads(void) {
    start();
    worker_t a = { &player[0], 20000, 0 }, b = { &player[1], 20000, 0 };
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, worker, &a) == 0);
    worker(&b);
    pthread_join(thread, NULL);
    CHECK(a.wrong == 0 && b.wrong == 0);
    CHECK(air_times.count == 3);
    // and the handlers themselves, the friction's hook and place, from both threads at once
    start();
    switches(true);
    in_air_answer = true;
    worker_t c = { &player[2], 2000, 0 };
    CHECK(pthread_create(&thread, NULL, worker, &c) == 0);
    for (int i = 0; i < 2000; i++) {
        set_player(0, 0, 300, 0);
        in_air_answer = i % 2 == 0;
        test_now = 50000 + (uint64_t)i;
        fake_PM_Friction(pml, pmove[0]);
        CHECK(frictioned.friction == 0.0f);
    }
    pthread_join(thread, NULL);
    CHECK(c.wrong == 0);
}

// ---- the hooks and detours

static void test_init(void) {
    start();
    CHECK(hooks == 4);
    // the places by their addresses in the game (offsets.h's names have them)
    CHECK(replacement_at(0x1147ed9) == (void *)fake_PM_Accelerate);
    CHECK(replacement_at(0x113c32e) == (void *)fake_PM_Friction);
    CHECK(replacement_at(0x113c500) == (void *)handler_PM_Friction_GotFriction);
    CHECK(replacement_at(0x14dd451) == (void *)handler_PM_CheckJump_DelayCmp);
    CHECK(orig_PM_Accelerate == game_accelerate && orig_PM_Friction == game_friction);
    // the setting off: nothing
    memset(hooked, 0, sizeof(hooked));
    hooks = 0;
    setting = false;
    aae_move_fastjump_init(Game_Base);
    CHECK(hooks == 0);
    // ... unless the debug lines are asked for, which only watch
    setenv("BO3MACFIX_MOVEDEBUG", "1", 1);
    aae_move_fastjump_init(Game_Base);
    CHECK(hooks == 4 && debug);
    unsetenv("BO3MACFIX_MOVEDEBUG");
    // any stub missing: nothing
    setting = true;
    for (int missing = 0; missing < 4; missing++) {
        static const uint32_t places[] = { ADDR_PM_Accelerate, ADDR_PM_Friction, ADDR_PM_Friction_GotFriction,
                                           ADDR_PM_CheckJump_DelayCmp };
        memset(hooked, 0, sizeof(hooked));
        hooks = 0;
        stub_missing_for = places[missing];
        aae_move_fastjump_init(Game_Base);
        CHECK(hooks == 0);
    }
    stub_missing_for = 0;
    // the call sites and PM_IsInAir, from the base
    aae_move_fastjump_init(Game_Base);
    CHECK(air_call_returns == (const void *)(Game_Base + 0x1147411));
    CHECK(ground_call_returns == (const void *)(Game_Base + 0x1147096));
    CHECK(PM_IsInAir == (void *)(Game_Base + 0x113d293));
}

// A call site like the game's: a call to a function's start, which jumps on (as the pre-patched executable's
// function start and its stub do), so that the hook returns right behind the call, at game_call_returns.
void *test_target;
void call_like_the_game(uint8_t *ps, uint8_t *pml, const float *wishdir, float wish_speed, float acceleration);
extern const char game_call_returns[];
#if defined(__aarch64__)
__asm__(".text\n"
        ".p2align 2\n"
        ".globl _call_like_the_game\n"
        ".private_extern _call_like_the_game\n"
        "_call_like_the_game:\n"
        "    stp x29, x30, [sp, #-16]!\n"
        "    mov x29, sp\n"
        "    bl 1f\n"
        ".globl _game_call_returns\n"
        ".private_extern _game_call_returns\n"
        "_game_call_returns:\n"
        "    ldp x29, x30, [sp], #16\n"
        "    ret\n"
        "1:  adrp x16, _test_target@PAGE\n"
        "    ldr x16, [x16, _test_target@PAGEOFF]\n"
        "    br x16\n");
#elif defined(__x86_64__)
__asm__(".text\n"
        ".globl _call_like_the_game\n"
        ".private_extern _call_like_the_game\n"
        "_call_like_the_game:\n"
        "    push %rbp\n"
        "    mov %rsp, %rbp\n"
        "    call 1f\n"
        ".globl _game_call_returns\n"
        ".private_extern _game_call_returns\n"
        "_game_call_returns:\n"
        "    pop %rbp\n"
        "    ret\n"
        "1:  jmp *_test_target(%rip)\n");
#endif

static void test_accelerate_hook(void) {
    start();
    const float direction[3] = { 0.6f, -0.8f, 0.0f };
    void (*hook)(uint8_t *, uint8_t *, const float *, float, float) =
        (void (*)(uint8_t *, uint8_t *, const float *, float, float))replacement_at(ADDR_PM_Accelerate);
    test_target = (void *)hook;
    set_player(0, 0, 30, 40);
    // from the air move's call: raised; the game's function gets the rest as it came, and a copy of the direction
    air_call_returns = game_call_returns;
    memset(&accelerated, 0, sizeof(accelerated));
    call_like_the_game(player[0], pml, direction, 190.0f, 1.0f);
    CHECK(accelerated.calls == 1 && accelerated.ps == player[0] && accelerated.pml == pml);
    CHECK(accelerated.wish_speed == 360.0f && accelerated.acceleration == 4.0f);
    CHECK(accelerated.wishdir != direction && memcmp(accelerated.direction, direction, sizeof(direction)) == 0);
    CHECK(asked_for == Move_FastJump);
    // sliding, or 150 and faster: as it came
    set_player(0, 0x80, 30, 40);
    call_like_the_game(player[0], pml, direction, 190.0f, 1.0f);
    CHECK(accelerated.wish_speed == 190.0f && accelerated.acceleration == 1.0f);
    set_player(0, 0, 150, 0);
    call_like_the_game(player[0], pml, direction, 190.0f, 2.0f);
    CHECK(accelerated.wish_speed == 190.0f && accelerated.acceleration == 2.0f);
    // from the ground move's call
    air_call_returns = (const void *)(Game_Base + 0x1147411);
    ground_call_returns = game_call_returns;
    call_like_the_game(player[0], pml, direction, 190.0f, 9.0f);
    CHECK(accelerated.wish_speed == 190.0f && accelerated.acceleration == 10.0f);
    // from anywhere else (BO3MacFix's slide code, say): as it came, with a copy of the direction
    ground_call_returns = (const void *)(Game_Base + 0x1147096);
    set_player(0, 0, 30, 40);
    call_like_the_game(player[0], pml, direction, 190.0f, 9.0f);
    CHECK(accelerated.wish_speed == 190.0f && accelerated.acceleration == 9.0f && accelerated.wishdir != direction);
    hook(player[0], pml, direction, 190.0f, 1.0f);
    CHECK(accelerated.wish_speed == 190.0f && accelerated.acceleration == 1.0f);
    // the patch off, or T7Overcharged's movement not started, or the setting off: the game's own call
    air_call_returns = game_call_returns;
    for (int off = 0; off < 3; off++) {
        switches(true);
        if (off == 0)
            switched_on = false;
        if (off == 1)
            started = false;
        if (off == 2)
            setting = false;
        memset(&accelerated, 0, sizeof(accelerated));
        call_like_the_game(player[0], pml, direction, from_bits(0x43350001), from_bits(0x3f800001));
        CHECK(accelerated.calls == 1 && accelerated.wishdir == direction);
        CHECK(bits_of(accelerated.wish_speed) == 0x43350001 && bits_of(accelerated.acceleration) == 0x3f800001);
    }
}

static void test_friction_hook(void) {
    start();
    set_player(0, 0, 300, 0);
    // in the air: the time is noted for the player state, then the game's friction runs, with its arguments
    in_air_answer = true;
    test_now = 700000;
    fake_PM_Friction(pml, pmove[0]);
    CHECK(asked_in_air.pm == pmove[0] && asked_in_air.pml == pml);
    CHECK(frictioned.calls == 1 && frictioned.pml == pml && frictioned.pm == pmove[0]);
    uint64_t at = 0;
    CHECK(last_in_air(player[0], &at) && at == 700000);
    // on the ground: none for 67 ms
    in_air_answer = false;
    for (uint64_t ms = 0; ms <= 70; ms++) {
        test_now = 700000 + ms;
        fake_PM_Friction(pml, pmove[0]);
        CHECK(frictioned.friction == (ms <= 67 ? 0.0f : 5.5f));
    }
    CHECK(last_in_air(player[0], &at) && at == 700000);
    // only that register changed, and only its float
    test_now = 700010;
    fake_PM_Friction(pml, pmove[0]);
    detour_registers_t expected = frictioned.before;
    memset(expected.xmm[0], 0, 4);
    CHECK(memcmp(&expected, &frictioned.after, sizeof(expected)) == 0);
    // slower than 100, or sliding: the game's
    set_player(0, 0, 99.0f, 0);
    fake_PM_Friction(pml, pmove[0]);
    CHECK(frictioned.friction == 5.5f);
    set_player(0, 0x80, 300.0f, 0);
    fake_PM_Friction(pml, pmove[0]);
    CHECK(frictioned.friction == 5.5f);
    // another player state, never in the air: the game's
    set_player(1, 0, 300.0f, 0);
    fake_PM_Friction(pml, pmove[1]);
    CHECK(frictioned.friction == 5.5f);
    // the patch off: nothing noted, nothing asked, the game's friction
    set_player(0, 0, 300.0f, 0);
    switched_on = false;
    asked_in_air.calls = 0;
    in_air_answer = true;
    test_now = 800000;
    fake_PM_Friction(pml, pmove[0]);
    CHECK(asked_in_air.calls == 0 && frictioned.friction == 5.5f);
    CHECK(last_in_air(player[0], &at) && at == 700000);
    in_air_answer = false;
    test_now = 700010;
    fake_PM_Friction(pml, pmove[0]);
    CHECK(frictioned.friction == 5.5f);
    detour_registers_t untouched = frictioned.before;
    CHECK(memcmp(&untouched, &frictioned.after, sizeof(untouched)) == 0);
    // a debug run with the patch off watches, and notes nothing: switched on 10 ms later, the game's friction (T7O
    // notes nothing while its hook is off)
    debug = true;
    say_line = capture;
    in_air_answer = true;
    set_player(2, 0, 300.0f, 0);
    test_now = 810000;
    fake_PM_Friction(pml, pmove[2]);
    CHECK(asked_in_air.calls == 1 && !last_in_air(player[2], &at));
    switched_on = true;
    in_air_answer = false;
    test_now = 810010;
    fake_PM_Friction(pml, pmove[2]);
    CHECK(frictioned.friction == 5.5f);
    debug = false;
}

static void test_delay_place(void) {
    start();
    detour_registers_t r;
    for (int64_t t = -3; t <= 503; t++) {
        memset(&r, 0x77, sizeof(r));
        r.rcx = (uint32_t)(int32_t)t;
        detour_registers_t expected = r;
        expected.rcx = (uint32_t)(t >= 1 && t < 500 ? 500 : (int32_t)t);
        handler_PM_CheckJump_DelayCmp(&r);
        CHECK(memcmp(&r, &expected, sizeof(r)) == 0);
    }
    switched_on = false;
    memset(&r, 0x77, sizeof(r));
    r.rcx = 7;
    detour_registers_t expected = r;
    handler_PM_CheckJump_DelayCmp(&r);
    CHECK(memcmp(&r, &expected, sizeof(r)) == 0);
}

// ---- the debug lines, which the game's checks go by

// one move of player state 0 at this command time: PM_Friction first, then PM_Accelerate from the move's first call,
// and another call from elsewhere (BO3MacFix's slide code calls it too), which counts for nothing
static void move(int32_t time, bool in_air, float x, float y) {
    static const void *const elsewhere = &elsewhere;
    set_player(0, 0, x, y);
    memcpy(pmove[0] + 0x8, &time, sizeof(time));
    in_air_answer = in_air;
    test_now = 900000 + (uint64_t)time;
    fake_PM_Friction(pml, pmove[0]);
    const float direction[3] = { 1, 0, 0 };
    accelerate(player[0], pml, direction, 190.0f, in_air ? 1.0f : 9.0f, in_air ? air_call_returns : ground_call_returns);
    accelerate(player[0], pml, direction, 190.0f, 1.0f, elsewhere);
}

static void test_debug_lines(void) {
    start();
    debug = true;
    set_times(0, 1000, 0);
    move(1000, false, 0, 0);
    // a jump at 1016, 13 moves and 200 ms in the air, faster and faster (150 and faster from the eighth on), landing
    // at 120: the jump time moves on
    set_times(0, 1016, 1016);
    static const float air_speeds[13] = { 50, 66, 82, 98, 114, 130, 146, 162, 178, 242, 210, 194, 170 };
    for (int32_t t = 1016; t < 1216; t += 16)
        move(t, true, air_speeds[(t - 1016) / 16], 0);
    // the client's prediction goes over the same commands again, and sets the jump time back: not counted
    set_times(0, 1016, 1000);
    for (int32_t t = 1016; t < 1216; t += 16)
        move(t, true, 999.0f, 0);
    set_times(0, 1016, 1016);
    // on the ground: 8, 24, 40 and 56 ms after the last move in the air are within the 67, and the line comes at 100;
    // the prediction goes over the first two again, which counts for nothing
    for (int32_t t = 1216; t < 1300; t += 16) {
        move(t, false, 120.0f, 0);
        if (t == 1232) {
            move(1216, false, 120.0f, 0);
            move(1232, false, 120.0f, 0);
        }
    }
    move(1316, false, 110.0f, 0);
    // the prediction sets the jump time back: no jump
    set_times(0, 1332, 1000);
    move(1332, false, 110.0f, 0);
    set_times(0, 1332, 1016);
    move(1340, false, 110.0f, 0);
    // the next jump 328 ms after the last
    set_times(0, 1344, 1344);
    move(1344, true, 120.0f, 0);
    CHECK(said_like("^movement: fastjump: the patch is on$") == 1);
    CHECK(said_like("^movement: fastjump: player state 0 jumped$") == 1);
    CHECK(said_like("^movement: fastjump: player state 0 landed after 200 ms in the air: 50 at the jump, 242 the "
                    "highest, 120 at the landing; the air control raised in 7 of 13 air moves$") == 1);
    CHECK(said_like("^movement: fastjump: player state 0, 100 ms after landing at 120: 110, without friction in 4 "
                    "of 6 ground moves$") == 1);
    CHECK(said_like("^movement: fastjump: player state 0 jumped 328 ms after its last jump$") == 1);
    CHECK(said_like("jumped") == 2);
    CHECK(said_like("^movement: fastjump: the ground acceleration 9 became 10$") == 1);
    // what the game's checks look for
    CHECK(said_like("jumped ([1-9]|[1-9][0-9]|[1-4][0-9][0-9]) ms after its last jump") == 1);
    CHECK(said_like("the air control raised in [1-9][0-9]* of") == 1);
    CHECK(said_like("without friction in [1-9][0-9]* of") == 1);
    // the patch off: the same moves give the game's numbers, and the lines say so
    start();
    debug = true;
    switched_on = false;
    set_times(0, 5000, 0);
    move(5000, false, 0, 0);
    set_times(0, 5016, 5016);
    for (int32_t t = 5016; t < 5216; t += 16)
        move(t, true, 50.0f, 0);
    for (int32_t t = 5216; t < 5340; t += 16)
        move(t, false, 120.0f, 0);
    CHECK(said_like("^movement: fastjump: the patch is off$") == 1);
    CHECK(said_like("the air control raised in 0 of 13 air moves$") == 1);
    CHECK(said_like("without friction in 0 of 7 ground moves$") == 1);
    CHECK(said_like("jumped") == 1);
    debug = false;
}

// ---- the same cases as T7Overcharged's own code (tests/fastjump_cases.h)

#include "fastjump_cases.h"

// what T7Overcharged's own hooks gave for the cases under Wine (wip/agents/movef/wine/compare.sh): the same on the
// Mac's x86_64 and arm64 builds
#define T7O_Lines 17717
#define T7O_Checksum 0xbec53d5f901db9ecULL

static struct {
    unsigned lines;
    uint64_t checksum;
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
static void side_accelerate(int place, uint8_t movement_flags, float velocity_x, float velocity_y,
                            const float direction[3], float wish_speed, float acceleration, float *got_wish_speed,
                            float *got_acceleration, int *copy) {
    static const void *const elsewhere = &elsewhere;
    set_player(0, movement_flags, velocity_x, velocity_y);
    memset(&accelerated, 0, sizeof(accelerated));
    accelerate(player[0], pml, direction, wish_speed, acceleration,
               place == 0 ? air_call_returns : place == 1 ? ground_call_returns : elsewhere);
    *got_wish_speed = accelerated.wish_speed;
    *got_acceleration = accelerated.acceleration;
    *copy = accelerated.wishdir == direction                                                 ? 0
            : memcmp(accelerated.direction, direction, sizeof(accelerated.direction)) == 0 ? 1
                                                                                           : 2;
}
static float side_friction(int which, int in_air, uint8_t movement_flags, float velocity_x, float velocity_y,
                           uint64_t now) {
    set_player(which, movement_flags, velocity_x, velocity_y);
    in_air_answer = in_air != 0;
    test_now = now;
    fake_PM_Friction(pml, pmove[which]);
    return frictioned.friction;
}

static void test_against_t7o(void) {
    start();
    cases_out.checksum = 0xcbf29ce484222325ULL, cases_out.lines = 0;
    cases_run();
    CHECK(cases_out.lines == T7O_Lines);
    CHECK(cases_out.checksum == T7O_Checksum);
    if (cases_out.lines != T7O_Lines || cases_out.checksum != T7O_Checksum)
        printf("  the cases gave %u lines, checksum %#018llx: compare with wip/agents/movef/wine/compare.sh\n",
               cases_out.lines, (unsigned long long)cases_out.checksum);
}

int main(void) {
    test_accelerate_rule();
    test_friction_rule();
    test_delay_rule();
    test_air_table();
    test_init();
    test_accelerate_hook();
    test_friction_hook();
    test_delay_place();
    test_debug_lines();
    test_threads();
    test_against_t7o();
    if (failures) {
        printf("test_aae_move_fastjump: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_move_fastjump: all passed\n");
    return 0;
}
