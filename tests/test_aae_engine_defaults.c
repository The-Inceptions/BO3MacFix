// Tests for aae_engine_defaults.c (AAEEngineDefaults): when it is active (always, on battery), each dvar's step (set
// to the game's own value, never raised, put back, let go of a value something else changed, ProMod's 0.5X left
// alone), the log's text, and the game's half against made-up dvars laid out like the engine's (the type at +0x1c, the
// value at +0x28): what it does every frame (a countdown, every 16th frame the clock, no dvar touched while off and
// nothing held, the engine called only to find and to set), the source of a set, set again after the mod's HUD resets
// the values at a match start, put back when the setting goes off, the battery choice, the test switch, a dvar that
// isn't there, a dvar that isn't an enum (left alone, never read).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fake_on_battery = false;
static int battery_asked = 0, asked_at_init = 0;
bool gpu_tweaks_on_battery(void) {
    battery_asked++;
    return fake_on_battery;
}

#include "../source/aae_engine_defaults.c"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_active(void) {
    CHECK(!aae_engine_defaults_active(0, false) && !aae_engine_defaults_active(0, true));
    CHECK(aae_engine_defaults_active(1, false) && aae_engine_defaults_active(1, true));
    CHECK(!aae_engine_defaults_active(2, false) && aae_engine_defaults_active(2, true));
    CHECK(!aae_engine_defaults_active(3, true) && !aae_engine_defaults_active(-1, true));
}

static void test_table(void) {
    CHECK(aae_engine_default_count == 2);
    CHECK(strcmp(aae_engine_defaults[0].name, "r_viewModelSpotOmniShadowResolution") == 0 && aae_engine_defaults[0].wanted == 2);
    CHECK(strcmp(aae_engine_defaults[1].name, "r_viewModelSunShadowResolution") == 0 && aae_engine_defaults[1].wanted == 1);
    // the hashes the game registers them under (Dvar_RegisterEnum's calls at 0x1013304bc and 0x101330497)
    CHECK(gsc_hash(aae_engine_defaults[0].name) == 0x5beebe9f && gsc_hash(aae_engine_defaults[1].name) == 0x93340312);
}

static void test_text(void) {
    char buffer[16];
    CHECK(strcmp(aae_engine_default_text(0, buffer, sizeof(buffer)), "4.0X") == 0);
    CHECK(strcmp(aae_engine_default_text(1, buffer, sizeof(buffer)), "2.0X") == 0);
    CHECK(strcmp(aae_engine_default_text(2, buffer, sizeof(buffer)), "1.0X") == 0);
    CHECK(strcmp(aae_engine_default_text(3, buffer, sizeof(buffer)), "0.5X") == 0);
    CHECK(strcmp(aae_engine_default_text(4, buffer, sizeof(buffer)), "4") == 0);
    CHECK(strcmp(aae_engine_default_text(-1, buffer, sizeof(buffer)), "-1") == 0);
}

static void test_step(void) {
    const engine_default_t *spot = &aae_engine_defaults[0], *sun = &aae_engine_defaults[1];
    int value = -9;
    // the mod's 4.0X (0) to the lamps' 1.0X (2); then nothing; off: back to 4.0X
    engine_default_state_t s = { 0 };
    CHECK(aae_engine_default_step(spot, &s, true, 0, &value) == Engine_Default_Set && value == 2);
    CHECK(aae_engine_default_step(spot, &s, true, 2, &value) == Engine_Default_Nothing);
    CHECK(aae_engine_default_step(spot, &s, false, 2, &value) == Engine_Default_Restore && value == 0);
    CHECK(aae_engine_default_step(spot, &s, false, 0, &value) == Engine_Default_Nothing);
    // 2.0X (1) is above the lamps' 1.0X too; the sun's own value is 2.0X
    s = (engine_default_state_t){ 0 };
    CHECK(aae_engine_default_step(spot, &s, true, 1, &value) == Engine_Default_Set && value == 2);
    CHECK(aae_engine_default_step(spot, &s, false, 2, &value) == Engine_Default_Restore && value == 1);
    s = (engine_default_state_t){ 0 };
    CHECK(aae_engine_default_step(sun, &s, true, 0, &value) == Engine_Default_Set && value == 1);
    CHECK(aae_engine_default_step(sun, &s, true, 1, &value) == Engine_Default_Nothing);
    // never raised: the game's own value and ProMod's 0.5X (3) stay, and nothing is put back
    s = (engine_default_state_t){ 0 };
    CHECK(aae_engine_default_step(spot, &s, true, 2, &value) == Engine_Default_Nothing);
    CHECK(aae_engine_default_step(spot, &s, true, 3, &value) == Engine_Default_Nothing);
    CHECK(aae_engine_default_step(spot, &s, false, 3, &value) == Engine_Default_Nothing);
    CHECK(aae_engine_default_step(sun, &s, true, 3, &value) == Engine_Default_Nothing);
    // ProMod switched on in a match while held: let go, nothing put back over it
    s = (engine_default_state_t){ 0 };
    CHECK(aae_engine_default_step(spot, &s, true, 0, &value) == Engine_Default_Set);
    CHECK(aae_engine_default_step(spot, &s, true, 3, &value) == Engine_Default_Nothing);
    CHECK(aae_engine_default_step(spot, &s, false, 3, &value) == Engine_Default_Nothing);
    // the mod's HUD at the next match start: set again, and what goes back is what it set then
    s = (engine_default_state_t){ 0 };
    CHECK(aae_engine_default_step(spot, &s, true, 1, &value) == Engine_Default_Set);
    CHECK(aae_engine_default_step(spot, &s, true, 0, &value) == Engine_Default_Set && value == 2);
    CHECK(aae_engine_default_step(spot, &s, false, 2, &value) == Engine_Default_Restore && value == 0);
}

// The made-up game: dvars laid out like the engine's; finds and sets counted (the engine's calls)
typedef struct {
    uint8_t head[0x1c];
    uint32_t type;
    uint8_t gap[0x28 - 0x20];
    int32_t value;
    const char *name;
} fake_dvar_t;
_Static_assert(offsetof(fake_dvar_t, type) == dvar_Type && offsetof(fake_dvar_t, value) == dvar_Value, "the layout");
static fake_dvar_t fakes[] = {
    { .type = 7, .name = "r_viewModelSpotOmniShadowResolution" },
    { .type = 7, .name = "r_viewModelSunShadowResolution" },
};
static const char *missing = NULL;
static int finds = 0, sets = 0, wrong_source = 0, clock_reads = 0;
static uint64_t fake_now = 1000000000ull;

static void *fake_find(uint32_t hash) {
    finds++;
    for (int i = 0; i < 2; i++)
        if (gsc_hash(fakes[i].name) == hash)
            return missing != NULL && strcmp(missing, fakes[i].name) == 0 ? NULL : &fakes[i];
    return NULL;
}
static void fake_set(void *dvar, const char *value, int source) {
    ((fake_dvar_t *)dvar)->value = atoi(value);
    sets++;
    wrong_source += source != 0;
}
static uint64_t fake_clock(void) {
    clock_reads++;
    return fake_now;
}
// half a second of client frames (16 frames, then the clock moves on)
static void half_second(void) {
    fake_now += 500000000ull;
    for (int i = 0; i < Frames_Between_Clocks; i++)
        aae_engine_defaults_frame();
}

static void start(int setting, const char *test) {
    if (test != NULL)
        setenv("BO3MACFIX_AAEENGINEDEFAULTSTEST", test, 1);
    else
        unsetenv("BO3MACFIX_AAEENGINEDEFAULTSTEST");
    const uint64_t base = 0x7000000000ull;
    battery_asked = 0;
    aae_engine_defaults_init(base, setting);
    asked_at_init = battery_asked;
    CHECK(Dvar_FindByHash == (void *)(base + ADDR_Dvar_FindByHash) &&
          Dvar_SetFromString == (void *)(base + ADDR_Dvar_SetFromString));
    CHECK(ADDR_Dvar_FindByHash == 0xf9fffa && ADDR_Dvar_SetFromString == 0xfa2ef7 && dvar_Type == 0x1c &&
          dvar_Value == 0x28);
    Dvar_FindByHash = fake_find;
    Dvar_SetFromString = fake_set;
    clock_ns = fake_clock;
    memset(found, 0, sizeof(found));
    memset(refused, 0, sizeof(refused));
    memset(states, 0, sizeof(states));
    countdown = 0;
    next_look = test_start = 0;
    last_active = -1;
    // what the mod's HUD leaves at a match start (ProMod off)
    fakes[0].type = fakes[1].type = 7;
    fakes[0].value = fakes[1].value = 0;
    finds = sets = wrong_source = clock_reads = battery_asked = 0;
    fake_on_battery = false;
    missing = NULL;
}

static void test_frames(void) {
    // off (the default): every frame a countdown, every 16th the clock, no dvar found, read or set, no battery asked
    start(0, NULL);
    CHECK(asked_at_init == 0);
    for (int i = 0; i < 160; i++)
        aae_engine_defaults_frame();
    CHECK(clock_reads == 10 && finds == 0 && sets == 0 && battery_asked == 0);
    // on: the two set once, with the internal source; then nothing but reads of their values
    start(1, NULL);
    CHECK(asked_at_init == 0);
    aae_engine_defaults_frame();
    CHECK(fakes[0].value == 2 && fakes[1].value == 1 && sets == 2 && wrong_source == 0 && finds == 2);
    for (int i = 0; i < 10; i++)
        half_second();
    CHECK(sets == 2 && finds == 2 && battery_asked == 0);
    // between two looks (16 frames, the clock not past the half second) nothing at all
    fakes[0].value = 0;
    for (int i = 0; i < Frames_Between_Clocks * 3; i++)
        aae_engine_defaults_frame();
    CHECK(fakes[0].value == 0 && sets == 2);
    // the next look: set again (the mod's HUD at a new match start)
    half_second();
    CHECK(fakes[0].value == 2 && sets == 3);
    // off from the settings page: put back as the mod had them, then nothing touched
    fakes[1].value = 0;
    half_second(); // the sun's reset by the mod again, still on
    CHECK(fakes[1].value == 1 && sets == 4);
    aae_engine_defaults_set(0);
    half_second();
    CHECK(fakes[0].value == 0 && fakes[1].value == 0 && sets == 6);
    int finds_then = finds;
    for (int i = 0; i < 10; i++)
        half_second();
    CHECK(sets == 6 && finds == finds_then);
    // the settings page picking 2 starts the battery watch there and then (not in a match frame), 1 and 0 don't
    battery_asked = 0;
    aae_engine_defaults_set(2);
    CHECK(battery_asked == 1);
    aae_engine_defaults_set(0);
    aae_engine_defaults_set(1);
    CHECK(battery_asked == 1);
    // ProMod's 0.5X: left alone while on
    aae_engine_defaults_set(1);
    fakes[0].value = fakes[1].value = 3;
    half_second();
    CHECK(fakes[0].value == 3 && fakes[1].value == 3 && sets == 6);

    // on battery only: the watch started at the start already; nothing plugged in, the values when on battery, put
    // back when plugged in again
    start(2, NULL);
    CHECK(asked_at_init == 1);
    half_second();
    CHECK(sets == 0 && battery_asked == 1);
    fake_on_battery = true;
    half_second();
    CHECK(fakes[0].value == 2 && fakes[1].value == 1 && sets == 2);
    fake_on_battery = false;
    half_second();
    CHECK(fakes[0].value == 0 && fakes[1].value == 0 && sets == 4);

    // a dvar not registered yet: the other one done, found when it comes
    start(1, NULL);
    missing = "r_viewModelSunShadowResolution";
    half_second();
    CHECK(fakes[0].value == 2 && fakes[1].value == 0);
    missing = NULL;
    half_second();
    CHECK(fakes[1].value == 1);

    // a dvar that isn't an enum: never read, never set, the other one still done
    start(1, NULL);
    fakes[1].type = 2;
    fakes[1].value = 0; // a float 0.0's bits: read as an index it would be "4.0X" and set
    for (int i = 0; i < 4; i++)
        half_second();
    CHECK(fakes[1].value == 0 && fakes[0].value == 2 && sets == 1 && refused[1]);

    // the test switch: off for its first 2 s from the first look, on for 2 s, off again (put back)
    start(1, "2");
    half_second(); // the first look: 0 s
    CHECK(sets == 0);
    half_second();
    half_second();
    half_second(); // 1.5 s
    CHECK(sets == 0);
    half_second(); // 2.0 s: on
    CHECK(fakes[0].value == 2 && sets == 2);
    half_second();
    half_second();
    half_second();
    half_second(); // 4.0 s: off, put back
    CHECK(fakes[0].value == 0 && fakes[1].value == 0 && sets == 4);
    unsetenv("BO3MACFIX_AAEENGINEDEFAULTSTEST");
}

int main(void) {
    test_active();
    test_table();
    test_text();
    test_step();
    test_frames();
    if (failures > 0) {
        printf("test_aae_engine_defaults: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_engine_defaults: all passed\n");
    return 0;
}
