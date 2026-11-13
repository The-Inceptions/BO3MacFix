// aae_freeaim.c: the plugin's rule for when free aim is on, and the crosshair test's hook against a made-up game.
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/aae_plugin.c"
#include "../source/aae_freeaim.c"

// ---- what the code asks of the rest of BO3MacFix
static char mod_folder[4096];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
// which places the pre-patched executable has stubs for
static bool have_stubs = true;
static int stub_missing_for = -1;
bool prepatch_has_hook(uint32_t offset) {
    return have_stubs && (int)offset != stub_missing_for;
}
static struct {
    void *address, *replacement, **original;
} hooked[8];
static int hooks;
// the made-up game's own test: what it answers, and how often it was asked
static struct {
    int calls;
    bool answer;
    const uint8_t *cg, *ps;
} game_test;
static bool game_draw_test(uint8_t *cg, uint8_t *ps) {
    game_test.calls++;
    game_test.cg = cg, game_test.ps = ps;
    return game_test.answer;
}
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 8)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    if (out_origin_func == (void **)&orig_CG_DrawCrosshairTest)
        *out_origin_func = (void *)game_draw_test;
    hooks++;
    return 0;
}
static bool was_hooked(uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 8; i++)
        if (hooked[i].address == (void *)(0x100000000 + (uint64_t)place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
}

// ---- the made-up game: dvars and two local clients' cg_t
static struct {
    const char *name;
    bool there;
    float number;
    const char *text;
} dvars[] = { { "mapname", true, 0, "zm_theater" },
              { "aae_freeaim_enabled", false, 1, NULL },
              { "aae_freeaim_enabled_1", false, 1, NULL } };
static void *game_find(uint32_t hash) {
    for (size_t i = 0; i < sizeof(dvars) / sizeof(dvars[0]); i++)
        if (dvars[i].there && gsc_hash(dvars[i].name) == hash)
            return &dvars[i];
    return NULL;
}
static float game_float(void *dvar) {
    return ((typeof(dvars[0]) *)dvar)->number;
}
static const char *game_string(void *dvar) {
    return ((typeof(dvars[0]) *)dvar)->text;
}
static void set_dvar(const char *name, bool there, float number) {
    for (size_t i = 0; i < sizeof(dvars) / sizeof(dvars[0]); i++)
        if (strcmp(dvars[i].name, name) == 0)
            dvars[i].there = there, dvars[i].number = number;
}
static uint8_t *game_cgs; // three in a row: two local clients and what comes after them
static uint8_t *cg_of(int which) {
    return game_cgs + (size_t)which * Cg_Size;
}
static void set_view(int which, bool third_person, int kind, uint32_t number) {
    cg_of(which)[Cg_ThirdPerson] = third_person;
    memcpy(cg_of(which) + Cg_PlayerState + Ps_Kind, &kind, sizeof(kind));
    memcpy(cg_of(which), &number, sizeof(number));
}

static uint64_t test_now = 100000;
static uint64_t test_clock(void) {
    return test_now;
}

// ---- the mod with the plugin
static char loaded_name[Mod_Name_Size + 8], plugin_file[4096 + 64];
static uint64_t plugin_time = 5000;
static uint64_t test_plugin_now(void) {
    return plugin_time;
}
static void set_mod(bool loaded) {
    memset(loaded_name, 0, sizeof(loaded_name));
    if (loaded)
        snprintf(loaded_name, sizeof(loaded_name), "%s", strrchr(mod_folder, '/') + 1);
    plugin_time += Plugin_Look_Ms;
}

static void test_rule(void) {
    aae_freeaim_client_t client = { 0 };
    static const int a_cg = 1, another_cg = 2;
    // never seen: off, whatever else, also in the first quarter of a second there is
    CHECK(!aae_freeaim_on(&client, 1000, 0, true, 0, "zm_x", 1, 1));
    CHECK((client.third_person & 3) == 1);
    aae_freeaim_client_t early = { .cg = &a_cg, .third_person = 2 };
    CHECK(!aae_freeaim_on(&early, 100, 0, true, 0, "zm_x", 1, 1) && early.third_person == (100 * 4 | 1));
    // seen with a third person view: on after a quarter of a second of it, not before
    aae_freeaim_seen(&client, &a_cg, 1000);
    CHECK(!aae_freeaim_on(&client, 1000, 0, true, 0, "zm_x", 1, 1) && client.third_person == (1000 * 4 | 2));
    aae_freeaim_seen(&client, &a_cg, 1249);
    CHECK(!aae_freeaim_on(&client, 1249, 0, true, 0, "zm_x", 1, 1));
    aae_freeaim_seen(&client, &a_cg, 1250);
    CHECK(aae_freeaim_on(&client, 1250, 0, true, 0, "zm_x", 1, 1) && client.third_person == (1000 * 4 | 2));
    // it stays on while the client is seen: 250 ms after the last time still counts, 251 don't
    CHECK(aae_freeaim_on(&client, 1500, 0, true, 0, "zm_x", 1, 1));
    CHECK(!aae_freeaim_on(&client, 1501, 0, true, 0, "zm_x", 1, 1) && client.third_person == (1501 * 4 | 1));
    // and seen again it takes its quarter of a second anew
    aae_freeaim_seen(&client, &a_cg, 1600);
    CHECK(!aae_freeaim_on(&client, 1600, 0, true, 0, "zm_x", 1, 1));
    aae_freeaim_seen(&client, &a_cg, 1850);
    CHECK(aae_freeaim_on(&client, 1850, 0, true, 0, "zm_x", 1, 1));
    // first person for a frame: off, and anew
    CHECK(!aae_freeaim_on(&client, 1851, 0, false, 0, "zm_x", 1, 1));
    CHECK(!aae_freeaim_on(&client, 1852, 0, true, 0, "zm_x", 1, 1));
    aae_freeaim_seen(&client, &a_cg, 2101);
    CHECK(!aae_freeaim_on(&client, 2101, 0, true, 0, "zm_x", 1, 1));
    aae_freeaim_seen(&client, &a_cg, 2102);
    CHECK(aae_freeaim_on(&client, 2102, 0, true, 0, "zm_x", 1, 1));
    // what switches it off while the view is third person, without starting anew
    CHECK(!aae_freeaim_on(&client, 2102, 0, true, Player_Kind_Spectator, "zm_x", 1, 1));
    CHECK(aae_freeaim_on(&client, 2102, 0, true, Player_Kind_Spectator - 1, "zm_x", 1, 1));
    CHECK(aae_freeaim_on(&client, 2102, 0, true, Player_Kind_Spectator + 1, "zm_x", 1, 1));
    CHECK(!aae_freeaim_on(&client, 2102, 0, true, 0, "mp_x", 1, 1) && !aae_freeaim_on(&client, 2102, 0, true, 0, "zm", 1, 1) &&
          !aae_freeaim_on(&client, 2102, 0, true, 0, "", 1, 1) && !aae_freeaim_on(&client, 2102, 0, true, 0, NULL, 1, 1) &&
          !aae_freeaim_on(&client, 2102, 0, true, 0, "Zm_x", 1, 1));
    CHECK(aae_freeaim_on(&client, 2102, 0, true, 0, "zm_", 1, 1));
    CHECK(!aae_freeaim_on(&client, 2102, 0, true, 0, "zm_x", 0, 1) && !aae_freeaim_on(&client, 2102, 0, true, 0, "zm_x", 1, 0));
    CHECK(aae_freeaim_on(&client, 2102, 0, true, 0, "zm_x", 0.5f, -2));
    CHECK(aae_freeaim_on(&client, 2102, 0, true, 0, "zm_x", 1, 1) && client.third_person == (1852 * 4 | 2));
    // a client number no player can have: as if not seen
    CHECK(aae_freeaim_on(&client, 2102, Max_Client_Number, true, 0, "zm_x", 1, 1));
    CHECK(!aae_freeaim_on(&client, 2102, Max_Client_Number + 1, true, 0, "zm_x", 1, 1));
    // another cg for the client: its time in third person starts anew
    aae_freeaim_seen(&client, &a_cg, 3000);
    aae_freeaim_on(&client, 3000, 0, true, 0, "zm_x", 1, 1);
    aae_freeaim_seen(&client, &a_cg, 3250);
    CHECK(aae_freeaim_on(&client, 3250, 0, true, 0, "zm_x", 1, 1));
    aae_freeaim_seen(&client, &another_cg, 3260);
    CHECK(client.third_person == 0 && !aae_freeaim_on(&client, 3260, 0, true, 0, "zm_x", 1, 1));
    // a clock that went back
    aae_freeaim_seen(&client, &another_cg, 3200);
    CHECK(!aae_freeaim_on(&client, 3200, 0, true, 0, "zm_x", 1, 1));
}

// the crosshair function asks the test for this client: what it is told. The view has been third person or not for
// long enough either way.
static bool asked_by_the_crosshair = true;
static bool crosshair_asks(int which, bool game_says) {
    game_test.answer = game_says;
    game_test.calls = 0;
    bool told = crosshair_test(cg_of(which), cg_of(which) + Cg_PlayerState, asked_by_the_crosshair);
    CHECK(game_test.calls == 1 && game_test.cg == cg_of(which) && game_test.ps == cg_of(which) + Cg_PlayerState);
    return told;
}
static bool after_a_while(int which, bool game_says) {
    crosshair_asks(which, game_says);
    test_now += Third_Person_For_Ms;
    return crosshair_asks(which, game_says);
}

static void test_hook(void) {
    set_mod(true);
    memset(clients, 0, sizeof(clients));
    set_view(0, true, 0, 0);
    set_view(1, true, 0, 1);
    // third person, where the game draws no crosshair: yes after a quarter of a second
    CHECK(!crosshair_asks(0, false));
    test_now += Third_Person_For_Ms - 1;
    CHECK(!crosshair_asks(0, false));
    test_now += 1;
    CHECK(crosshair_asks(0, false) && crosshair_asks(0, true));
    // each local client by itself
    CHECK(!crosshair_asks(1, false) && after_a_while(1, false) && crosshair_asks(0, false));
    set_view(1, false, 0, 1);
    CHECK(!crosshair_asks(1, false) && crosshair_asks(1, true) && crosshair_asks(0, false));
    set_view(1, true, 0, 1);
    CHECK(after_a_while(1, false));
    // the switches: one for all, and the second client's own
    set_dvar("aae_freeaim_enabled", true, 0);
    CHECK(!crosshair_asks(0, false) && !crosshair_asks(1, false) && crosshair_asks(0, true));
    set_dvar("aae_freeaim_enabled", true, 1);
    set_dvar("aae_freeaim_enabled_1", true, 0);
    CHECK(crosshair_asks(0, false) && !crosshair_asks(1, false));
    set_dvar("aae_freeaim_enabled_1", true, 2);
    CHECK(crosshair_asks(1, false));
    set_dvar("aae_freeaim_enabled", false, 1);
    set_dvar("aae_freeaim_enabled_1", false, 1);
    CHECK(crosshair_asks(0, false) && crosshair_asks(1, false));
    // not in a map that isn't a zombies one, nor without a map's name
    dvars[0].text = "mp_nuketown";
    CHECK(!crosshair_asks(0, false));
    dvars[0].there = false;
    CHECK(!crosshair_asks(0, false));
    dvars[0].there = true, dvars[0].text = "zm_theater";
    CHECK(crosshair_asks(0, false));
    // a spectator
    set_view(0, true, Player_Kind_Spectator, 0);
    CHECK(!crosshair_asks(0, false));
    set_view(0, true, 0, 0);
    CHECK(crosshair_asks(0, false));

    // somebody else asks the test: the game's own answer, and the client doesn't count as seen
    asked_by_the_crosshair = false;
    CHECK(!crosshair_asks(0, false) && crosshair_asks(0, true));
    test_now += Seen_For_Ms + 1;
    asked_by_the_crosshair = true;
    // (the hook itself goes by where it returns to, which isn't the crosshair function here)
    game_test.answer = false;
    CHECK(!fake_CG_DrawCrosshairTest(cg_of(0), cg_of(0) + Cg_PlayerState));
    game_test.answer = true;
    CHECK(fake_CG_DrawCrosshairTest(cg_of(0), cg_of(0) + Cg_PlayerState));
    // (nothing was kept meanwhile: the view counts as third person since when it was, so it is on at once)
    CHECK(crosshair_asks(0, false));
    // without the mod: the game's own answer, and nothing kept either
    set_mod(false);
    CHECK(!crosshair_asks(0, false) && crosshair_asks(0, true));
    set_mod(true);
    test_now += Seen_For_Ms + 1;
    CHECK(crosshair_asks(0, false));
    // something that isn't a local client's cg_t: before them, after them, inside one, none at all
    game_test.answer = false;
    set_view(2, true, 0, 2);
    CHECK(!crosshair_test(cg_of(2), cg_of(2) + Cg_PlayerState, true));
    test_now += Third_Person_For_Ms;
    CHECK(!crosshair_test(cg_of(2), cg_of(2) + Cg_PlayerState, true));
    CHECK(!crosshair_test(cg_of(0) + 8, cg_of(0) + Cg_PlayerState, true));
    CHECK(!crosshair_test(NULL, NULL, true));
    uint8_t *all = game_cgs;
    game_cgs = cg_of(1);
    CHECK(!crosshair_test(all, all + Cg_PlayerState, true));
    game_cgs = NULL;
    CHECK(!crosshair_test(all, all + Cg_PlayerState, true));
    game_cgs = all;
    CHECK(crosshair_asks(0, false));
}

// ---- the crosshair while aiming

static void test_gap(void) {
    // from the spread's gap to the weapon's least one as the sights come up
    CHECK(aae_freeaim_gap(40.0f, 8.0f, 0.0f) == 40.0f && aae_freeaim_gap(40.0f, 8.0f, 1.0f) == 8.0f &&
          aae_freeaim_gap(40.0f, 8.0f, 0.5f) == 24.0f && aae_freeaim_gap(40.0f, 8.0f, 0.25f) == 32.0f);
    // the sights' value counts within 0 and 1
    CHECK(aae_freeaim_gap(40.0f, 8.0f, 1.5f) == 8.0f && aae_freeaim_gap(40.0f, 8.0f, -0.5f) == 40.0f);
    // a least gap of 0 is one, a negative one isn't
    CHECK(aae_freeaim_gap(40.0f, 0.0f, 1.0f) == 0.0f && aae_freeaim_gap(40.0f, -1.0f, 1.0f) == 40.0f);
    // the gap is already the least one, or below it (the game takes the larger one before): it stays or goes up to it
    CHECK(aae_freeaim_gap(8.0f, 8.0f, 0.7f) == 8.0f && aae_freeaim_gap(4.0f, 8.0f, 1.0f) == 8.0f);
    // anything that isn't a number: the gap as it is
    CHECK(aae_freeaim_gap(40.0f, 8.0f, NAN) == 40.0f && aae_freeaim_gap(40.0f, 8.0f, INFINITY) == 40.0f &&
          aae_freeaim_gap(40.0f, NAN, 1.0f) == 40.0f && aae_freeaim_gap(40.0f, INFINITY, 1.0f) == 40.0f);
    CHECK(isnan(aae_freeaim_gap(NAN, 8.0f, 1.0f)) && isinf(aae_freeaim_gap(INFINITY, 8.0f, 1.0f)));
}

// the crosshair function's frame, with its copy of the sights' value, and a weapon's definition
static struct {
    uint8_t below[0x100];
    uint8_t top[0x40];
} game_frame;
static uint8_t game_weapon[0x1000];
static float frame_sights(void) {
    float value;
    memcpy(&value, game_frame.top + Frame_Sights, sizeof(value));
    return value;
}
// the game comes to where its tests for the sights and the weapon begin: whether it is sent past them
static bool at_the_tests(const uint8_t *cg) {
    const float up = 0.8f;
    memset(&game_frame, 0xee, sizeof(game_frame));
    memcpy(game_frame.top + Frame_Sights, &up, sizeof(up));
    bool past = handler_Crosshair_Tests((uint64_t)cg, (uint64_t)game_frame.top);
    // only the copy of the sights' value is ever written, and only when the game is sent on
    for (int i = 0; i < (int)sizeof(game_frame.below); i++) {
        int offset = i - (int)sizeof(game_frame.below);
        if (!(offset >= Frame_Sights && offset < Frame_Sights + 4) && game_frame.below[i] != 0xee) {
            CHECK(!"something else on the game's frame was written");
            break;
        }
    }
    CHECK(frame_sights() == (past ? 0.0f : up));
    return past;
}
// the game comes to where the gap is worked out, with this one: what it goes on with
static float at_the_gap(const uint8_t *cg, float gap) {
    detour_registers_t registers;
    memset(&registers, 0x5a, sizeof(registers));
    registers.r13 = (uint64_t)cg;
    registers.r12 = (uint64_t)game_weapon;
    memcpy(registers.xmm[1], &gap, sizeof(gap));
    detour_registers_t before = registers;
    handler_Crosshair_Gap(&registers);
    float after;
    memcpy(&after, registers.xmm[1], sizeof(after));
    memcpy(registers.xmm[1], &gap, sizeof(gap));
    CHECK(memcmp(&before, &registers, sizeof(registers)) == 0); // nothing but the gap is changed
    return after;
}
static void set_sights(int which, float value) {
    memcpy(cg_of(which) + Cg_PlayerState + Ps_Sights, &value, sizeof(value));
}

static void test_aiming(void) {
    const int32_t least = 8;
    memcpy(game_weapon + Weapon_LeastGap, &least, sizeof(least));
    set_mod(true);
    memset(clients, 0, sizeof(clients));
    set_view(0, true, 0, 0);
    set_view(1, false, 0, 1);
    set_sights(0, 0.5f);
    set_sights(1, 0.5f);
    // a client the crosshair function hasn't asked about yet: the game's own way
    CHECK(!at_the_tests(cg_of(0)) && at_the_gap(cg_of(0), 40.0f) == 40.0f);
    // free aim on: past the tests with the sights counted as down, and the gap by the sights
    CHECK(after_a_while(0, false));
    CHECK(at_the_tests(cg_of(0)) && at_the_gap(cg_of(0), 40.0f) == 24.0f);
    set_sights(0, 1.0f);
    CHECK(at_the_gap(cg_of(0), 40.0f) == 8.0f);
    set_sights(0, 0.0f);
    CHECK(at_the_gap(cg_of(0), 40.0f) == 40.0f);
    set_sights(0, 0.5f);
    // the other local client is in first person: nothing for it
    CHECK(crosshair_asks(1, true) && !at_the_tests(cg_of(1)) && at_the_gap(cg_of(1), 40.0f) == 40.0f);
    CHECK(at_the_tests(cg_of(0)));
    // off for whatever reason: the game's own way again
    set_dvar("aae_freeaim_enabled", true, 0);
    CHECK(!at_the_tests(cg_of(0)) && at_the_gap(cg_of(0), 40.0f) == 40.0f);
    set_dvar("aae_freeaim_enabled", false, 1);
    CHECK(at_the_tests(cg_of(0)) && at_the_gap(cg_of(0), 40.0f) == 24.0f);
    set_view(0, false, 0, 0);
    CHECK(!at_the_tests(cg_of(0)) && at_the_gap(cg_of(0), 40.0f) == 40.0f);
    set_view(0, true, 0, 0);
    CHECK(after_a_while(0, false) && at_the_tests(cg_of(0)));
    set_mod(false);
    CHECK(!at_the_tests(cg_of(0)) && at_the_gap(cg_of(0), 40.0f) == 40.0f);
    set_mod(true);
    CHECK(at_the_tests(cg_of(0)));
    // not seen by the crosshair's test for more than a quarter of a second
    test_now += Seen_For_Ms + 1;
    CHECK(!at_the_tests(cg_of(0)) && at_the_gap(cg_of(0), 40.0f) == 40.0f);
    CHECK(after_a_while(0, false) && at_the_tests(cg_of(0)));
    // something that isn't a local client's cg_t, or not the one the client was seen with
    CHECK(!at_the_tests(cg_of(2)) && !at_the_tests(cg_of(0) + 8) && !at_the_tests(NULL));
    CHECK(at_the_gap(cg_of(2), 40.0f) == 40.0f && at_the_gap(NULL, 40.0f) == 40.0f);
    clients[0].cg = cg_of(1);
    CHECK(!at_the_tests(cg_of(0)) && at_the_gap(cg_of(0), 40.0f) == 40.0f);
    clients[0].cg = cg_of(0);
    CHECK(at_the_tests(cg_of(0)));
    memset(clients, 0, sizeof(clients));
}

static void test_init(void) {
    const uint64_t base = 0x100000000;
    aae_freeaim_init(base, 0);
    CHECK(hooks == 0);
    // not from the pre-patched executable, or from one without both places: the crosshair's test only
    const int places[] = { ADDR_CG_DrawCrosshair_Tests, ADDR_CG_DrawCrosshair_Gap };
    for (int i = 0; i < 3; i++) {
        have_stubs = i < 2, stub_missing_for = i < 2 ? places[i] : -1;
        hooks = 0;
        aae_freeaim_init(base, 1);
        CHECK(hooks == 1 && was_hooked(ADDR_CG_DrawCrosshairTest, (void *)fake_CG_DrawCrosshairTest,
                                       (void **)&orig_CG_DrawCrosshairTest));
    }
    have_stubs = true, stub_missing_for = -1;
    hooks = 0;
    aae_freeaim_init(base, 1);
    CHECK(hooks == 3 && was_hooked(ADDR_CG_DrawCrosshairTest, (void *)fake_CG_DrawCrosshairTest,
                                   (void **)&orig_CG_DrawCrosshairTest));
    CHECK(was_hooked(ADDR_CG_DrawCrosshair_Tests, (void *)handler_Crosshair_Tests, &orig_Crosshair_Tests) &&
          other_Crosshair_Tests == (void *)(base + ADDR_CG_DrawCrosshair_Draws));
    CHECK(was_hooked(ADDR_CG_DrawCrosshair_Gap, (void *)handler_Crosshair_Gap, &orig_Crosshair_Gap));
    // the places in the game's own structures, as they were read from its code
    CHECK(Cg_Size == 0x342a80 && Cg_ThirdPerson == 0x11a8c9 && Cg_PlayerState == 0x11a8d0 && Ps_Kind == 4 &&
          Ps_Sights == 0x2fc && Frame_Sights == -0x50 && Weapon_LeastGap == 0x6f0);
    CHECK(Dvar_FindByHash == (void *)(base + ADDR_Dvar_FindByHash) && Dvar_GetFloat == (void *)(base + ADDR_Dvar_GetFloat) &&
          Dvar_GetString == (void *)(base + ADDR_Dvar_GetString) && cg_clients == (void *)(base + ADDR_cgArray) &&
          crosshair_asks_from == (const void *)(base + ADDR_CG_DrawCrosshair_TestReturn) && now_ms == uptime_ms);
}

int main(void) {
    char template[] = "/tmp/bo3macfix-freeaim-test-XXXXXX";
    if (mkdtemp(template) == NULL)
        return 2;
    snprintf(mod_folder, sizeof(mod_folder), "%s/2631943123", template);
    char folder[4096 + 64], command[4096 + 64];
    snprintf(command, sizeof(command), "mkdir -p '%s/T7Overcharged/plugins'", mod_folder);
    if (system(command) != 0)
        return 2;
    snprintf(plugin_file, sizeof(plugin_file), "%s/%s", mod_folder, Plugin_Path);
    close(open(plugin_file, O_CREAT | O_WRONLY, 0644));
    (void)folder;
    plugin_now_ms = test_plugin_now;
    game_cgs = calloc(3, Cg_Size);

    test_init();
    loaded_mod = loaded_name;
    Dvar_FindByHash = game_find;
    Dvar_GetFloat = game_float;
    Dvar_GetString = game_string;
    cg_clients = &game_cgs;
    now_ms = test_clock;
    test_rule();
    test_hook();
    test_gap();
    test_aiming();

    free(game_cgs);
    snprintf(command, sizeof(command), "rm -rf '%s'", template);
    if (system(command) != 0)
        failures++;
    if (failures != 0) {
        printf("test_aae_freeaim: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_freeaim: all passed\n");
    return 0;
}
