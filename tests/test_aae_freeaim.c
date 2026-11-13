// aae_freeaim.c: the plugin's rule for when free aim is on, its sums for where shots go, and the hooks against a
// made-up game.
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
} hooked[16];
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
    if (hooks < 16)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    if (out_origin_func == (void **)&orig_CG_DrawCrosshairTest)
        *out_origin_func = (void *)game_draw_test;
    hooks++;
    return 0;
}
static bool was_hooked(uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 16; i++)
        if (hooked[i].address == (void *)(0x100000000 + (uint64_t)place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
}

// ---- the made-up game: dvars and two local clients' cg_t
// a dvar as the game has it: its kind at +0x1c; one with text gives that to the game's own getter, and asking that
// getter for the text of any other kind is something the game can't answer (it would hand out the value as a pointer)
static struct {
    uint8_t game_side[0x40];
    const char *name;
    bool there;
    char text[200];
} dvars[] = { { .name = "mapname", .there = true, .text = "zm_theater" },
              { .name = "aae_freeaim_enabled" },
              { .name = "aae_freeaim_enabled_1" },
              { .name = "aae_freeaim_target" },
              { .name = "aae_freeaim_target_1" },
              { .name = "aae_freeaim_nonbullet" } };
static int wrong_asks; // times the text of a dvar without text was asked for
static void *game_find(uint32_t hash) {
    for (size_t i = 0; i < sizeof(dvars) / sizeof(dvars[0]); i++)
        if (dvars[i].there && gsc_hash(dvars[i].name) == hash)
            return &dvars[i];
    return NULL;
}
static const char *game_string(void *dvar) {
    typeof(dvars[0]) *d = dvar;
    int kind;
    memcpy(&kind, d->game_side + Dvar_Type, sizeof(kind));
    if (kind != Dvar_Type_String) {
        wrong_asks++;
        return "garbage";
    }
    return d->text;
}
// a dvar with this text, of this kind (8: one with text), or none
static void set_dvar_kind(const char *name, bool there, const char *text, int kind) {
    for (size_t i = 0; i < sizeof(dvars) / sizeof(dvars[0]); i++) {
        if (strcmp(dvars[i].name, name) == 0) {
            dvars[i].there = there;
            snprintf(dvars[i].text, sizeof(dvars[i].text), "%s", text);
            memcpy(dvars[i].game_side + Dvar_Type, &kind, sizeof(kind));
        }
    }
}
static void set_text(const char *name, const char *text) {
    set_dvar_kind(name, true, text, Dvar_Type_String);
}
static void set_dvar(const char *name, bool there, float number) {
    char text[32];
    snprintf(text, sizeof(text), "%g", (double)number);
    set_dvar_kind(name, there, text, Dvar_Type_String);
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
    set_text("mapname", "mp_nuketown");
    CHECK(!crosshair_asks(0, false));
    set_dvar_kind("mapname", false, "zm_theater", Dvar_Type_String);
    CHECK(!crosshair_asks(0, false));
    // nor when the name isn't a dvar with text
    set_dvar_kind("mapname", true, "zm_theater", 6);
    CHECK(!crosshair_asks(0, false) && wrong_asks == 0);
    set_text("mapname", "zm_theater");
    CHECK(crosshair_asks(0, false));
    // the switches as the plugin reads them: a number and nothing else; anything it can't read is "off" for the one
    // for all and "on" for a client's own (with one client both are the same dvar, and off wins)
    set_text("aae_freeaim_enabled", " 1 ");
    CHECK(crosshair_asks(0, false) && crosshair_asks(1, false));
    set_text("aae_freeaim_enabled", "1x");
    CHECK(!crosshair_asks(0, false) && !crosshair_asks(1, false));
    set_text("aae_freeaim_enabled", "");
    CHECK(!crosshair_asks(0, false));
    set_dvar_kind("aae_freeaim_enabled", true, "1", 6);
    CHECK(!crosshair_asks(0, false) && wrong_asks == 0);
    set_text("aae_freeaim_enabled", "0.5");
    set_text("aae_freeaim_enabled_1", "on");
    CHECK(crosshair_asks(0, false) && crosshair_asks(1, false));
    set_dvar_kind("aae_freeaim_enabled_1", true, "0", 2);
    CHECK(crosshair_asks(1, false) && wrong_asks == 0);
    set_text("aae_freeaim_enabled_1", "0");
    CHECK(!crosshair_asks(1, false));
    set_dvar("aae_freeaim_enabled", false, 1);
    set_dvar("aae_freeaim_enabled_1", false, 1);
    CHECK(crosshair_asks(0, false) && crosshair_asks(1, false));
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

// ---- where the shots go

// the same cases the plugin's own functions were given (see freeaim_cases.h), and what that gave
#define Plugin_Lines 200159
#define Plugin_Checksum 0x942f898a3c7aaac5ULL
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
#include "freeaim_cases.h"
static int side_number(const char **text, float *value) {
    return aae_freeaim_number(text, value);
}
static int side_turn(const float muzzle[3], const float directions[9], const float target[3], float turned[9]) {
    return aae_freeaim_turn(muzzle, directions, target, turned);
}
static int side_projectile(const float origin[3], const float velocity[3], const float target[3],
                           float turned_velocity[3], float angles[3]) {
    return aae_freeaim_projectile(origin, velocity, target, turned_velocity, angles);
}
static void test_like_the_plugin(void) {
    cases_out.checksum = 0xcbf29ce484222325ULL, cases_out.lines = 0;
    cases_run();
    CHECK(cases_out.lines == Plugin_Lines);
    CHECK(cases_out.checksum == Plugin_Checksum);
    if (cases_out.checksum != Plugin_Checksum)
        printf("  the cases came out as %#018llx: wip/windows/freeaim/compare.sh shows where they differ from the "
               "plugin's\n", (unsigned long long)cases_out.checksum);
}

// the same in words
static bool reads_as(const char *text, float expected, int length) {
    const char *at = text;
    float value = -99.0f;
    return aae_freeaim_number(&at, &value) && value == expected && at - text == length;
}
static bool no_number(const char *text) {
    const char *at = text;
    float value = -99.0f;
    return !aae_freeaim_number(&at, &value) && at == text && value == -99.0f;
}
static void test_numbers(void) {
    CHECK(reads_as("12", 12.0f, 2) && reads_as("-12.5", -12.5f, 5) && reads_as("+.5", 0.5f, 3) && reads_as("7.", 7.0f, 2));
    CHECK(reads_as("  \t42 x", 42.0f, 5) && reads_as("4,5", 4.0f, 1) && reads_as("1.2.3", 1.2f, 3));
    CHECK(reads_as("1e3", 1000.0f, 3) && reads_as("25E-1", 2.5f, 5) && reads_as("1e+2", 100.0f, 4));
    // an "e" without digits behind it isn't part of the number
    CHECK(reads_as("1e", 1.0f, 1) && reads_as("1e+", 1.0f, 1) && reads_as("1ex", 1.0f, 1));
    CHECK(no_number("") && no_number("x") && no_number(".") && no_number("-") && no_number("+.") && no_number(".e3") &&
          no_number("--4"));
    // not beyond 16777216, and no exponent beyond 38
    CHECK(reads_as("16777216", 16777216.0f, 8) && no_number("16777217") && reads_as("-16777216", -16777216.0f, 9));
    CHECK(reads_as("1e-38", 1e-38f, 5) && no_number("1e-39") && no_number("1e39") && no_number("1e100"));
    CHECK(reads_as("0.000000000000001e15", 1.0f, 20) || reads_as("0.000000000000001e15", 0.99999994f, 20) ||
          reads_as("0.000000000000001e15", 1.0000001f, 20));

    float target[3] = { 9, 9, 9 };
    CHECK(aae_freeaim_target("124.433 -641.925 149.125", target) && target[0] == 124.433f && target[1] == -641.925f &&
          target[2] == 149.125f);
    CHECK(aae_freeaim_target(" 1\t2\n3  ", target) && target[0] == 1.0f && target[1] == 2.0f && target[2] == 3.0f);
    // three numbers, white space between them, nothing else
    const char *const not_targets[] = { "", "1 2", "1 2 3 4", "1 2 3x", "1,2,3", "1 2 x", "x 2 3", "1 2", "1  ", "1 2 3 .",
                                        "1-2 3", "1 2 16777217" };
    for (size_t i = 0; i < sizeof(not_targets) / sizeof(not_targets[0]); i++) {
        float kept[3] = { 9, 9, 9 };
        CHECK(!aae_freeaim_target(not_targets[i], kept) && kept[0] == 9 && kept[1] == 9 && kept[2] == 9);
    }
    // in fewer than 192 characters
    char wide[400];
    memset(wide, ' ', sizeof(wide));
    memcpy(wide, "1 2 3", 5);
    wide[191] = '\0';
    CHECK(aae_freeaim_target(wide, target));
    wide[191] = ' ', wide[192] = '\0';
    CHECK(!aae_freeaim_target(wide, target));

    CHECK(aae_freeaim_switch("1", 7.0f) == 1.0f && aae_freeaim_switch("0", 7.0f) == 0.0f && aae_freeaim_switch(" 2.5 ", 7.0f) == 2.5f);
    CHECK(aae_freeaim_switch("", 7.0f) == 7.0f && aae_freeaim_switch("on", 7.0f) == 7.0f && aae_freeaim_switch("1 1", 7.0f) == 7.0f &&
          aae_freeaim_switch("1x", 7.0f) == 7.0f);
    memset(wide, ' ', sizeof(wide));
    wide[0] = '1', wide[63] = '\0';
    CHECK(aae_freeaim_switch(wide, 7.0f) == 1.0f);
    wide[63] = ' ', wide[64] = '\0';
    CHECK(aae_freeaim_switch(wide, 7.0f) == 7.0f);
}

static float dot(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static bool near(float a, float b) {
    return fabsf(a - b) < 0.0001f;
}
static const float straight[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; // forward along x, right along y, up along z
static void test_turn(void) {
    const float muzzle[3] = { 10, 20, 30 };
    float turned[9];
    // a target straight ahead: nothing turns
    CHECK(aae_freeaim_turn(muzzle, straight, (float[3]){ 110, 20, 30 }, turned) && memcmp(turned, straight, sizeof(turned)) == 0);
    // to the side: forward points at it, and the three are still of length 1 and at right angles
    const float beside[3] = { 110, 120, 30 };
    CHECK(aae_freeaim_turn(muzzle, straight, beside, turned));
    CHECK(near(turned[0], 0.70710678f) && near(turned[1], 0.70710678f) && near(turned[2], 0.0f));
    CHECK(near(dot(turned, turned), 1) && near(dot(turned + 3, turned + 3), 1) && near(dot(turned + 6, turned + 6), 1) &&
          near(dot(turned, turned + 3), 0) && near(dot(turned, turned + 6), 0) && near(dot(turned + 3, turned + 6), 0));
    CHECK(near(turned[8], 1.0f)); // the shortest turn there: up stays up
    // up and behind
    CHECK(aae_freeaim_turn(muzzle, straight, (float[3]){ 10, 20, 500 }, turned) && near(turned[2], 1.0f) && near(turned[6], -1.0f));
    CHECK(aae_freeaim_turn(muzzle, straight, (float[3]){ -90, 20, 30 }, turned) && near(turned[0], -1.0f) &&
          near(dot(turned + 3, turned + 3), 1) && near(dot(turned, turned + 3), 0));
    // a target at the muzzle is none
    CHECK(!aae_freeaim_turn(muzzle, straight, muzzle, turned));
    // directions that aren't of length 1 or at right angles (give or take 0.05), or a muzzle too far out
    float bent[9];
    memcpy(bent, straight, sizeof(bent));
    bent[0] = 1.02f;
    CHECK(aae_freeaim_turn(muzzle, bent, beside, turned));
    bent[0] = 1.03f;
    CHECK(!aae_freeaim_turn(muzzle, bent, beside, turned));
    bent[0] = 1.0f, bent[4] = 0.97f;
    CHECK(!aae_freeaim_turn(muzzle, bent, beside, turned));
    bent[4] = 1.0f, bent[3] = 0.06f;
    CHECK(!aae_freeaim_turn(muzzle, bent, beside, turned));
    bent[3] = 0.04f;
    CHECK(aae_freeaim_turn(muzzle, bent, beside, turned));
    bent[3] = 0.0f, bent[6] = 0.06f;
    CHECK(!aae_freeaim_turn(muzzle, bent, beside, turned));
    bent[6] = 0.0f, bent[7] = 0.06f;
    CHECK(!aae_freeaim_turn(muzzle, bent, beside, turned));
    bent[7] = NAN;
    CHECK(!aae_freeaim_turn(muzzle, bent, beside, turned));
    CHECK(aae_freeaim_turn((float[3]){ 16777216.0f, 0, 0 }, straight, beside, turned) &&
          !aae_freeaim_turn((float[3]){ 0, -16777218.0f, 0 }, straight, beside, turned) &&
          !aae_freeaim_turn((float[3]){ 0, 0, NAN }, straight, beside, turned));
    CHECK(!aae_freeaim_turn(muzzle, straight, (float[3]){ NAN, 0, 0 }, turned) &&
          !aae_freeaim_turn(muzzle, straight, (float[3]){ INFINITY, 0, 0 }, turned));
}

// a player on the server who fires: the entity, the player's state, and the shot's parameters
static uint8_t shooter[0x300], shooter_state[0x200], shot[0x80];
static const float shot_muzzle[3] = { -239.0f, -1208.0f, 140.0f };
static struct {
    int calls;
    float directions[9], spread;
    uint8_t *attacker, *shot;
    uint64_t rest[4];
} fired;
static void game_bullet(uint8_t *attacker, uint8_t *parameters, uint64_t third, uint64_t fourth, uint64_t fifth,
                        uint64_t sixth, float spread) {
    fired.calls++;
    fired.attacker = attacker, fired.shot = parameters, fired.spread = spread;
    fired.rest[0] = third, fired.rest[1] = fourth, fired.rest[2] = fifth, fired.rest[3] = sixth;
    memset(fired.directions, 0, sizeof(fired.directions));
    if (parameters != NULL)
        memcpy(fired.directions, parameters, sizeof(fired.directions));
}
static void set_shooter(uint32_t number, bool third_person, bool with_state) {
    const uint8_t *state = with_state ? shooter_state : NULL;
    memcpy(shooter, &number, sizeof(number));
    memcpy(shooter + Entity_Client, &state, sizeof(state));
    shooter_state[Ps_ThirdPerson] = third_person;
    memcpy(shot, straight, sizeof(straight));
    memcpy(shot + Shot_Muzzle, shot_muzzle, sizeof(shot_muzzle));
}
// the server fires a bullet for the player: whether it went somewhere else than straight ahead. Either way the
// game's function is called once with everything it was given, and the shot's own directions are back afterwards.
static bool fires(uint8_t *attacker, uint8_t *parameters) {
    fired.calls = 0;
    fake_Bullet_Fire(attacker, parameters, 0x33, 0x44, 0x55, 0x66, 0.25f);
    CHECK(fired.calls == 1 && fired.attacker == attacker && fired.shot == parameters && fired.spread == 0.25f &&
          fired.rest[0] == 0x33 && fired.rest[1] == 0x44 && fired.rest[2] == 0x55 && fired.rest[3] == 0x66);
    if (parameters != NULL) {
        CHECK(memcmp(parameters, straight, sizeof(straight)) == 0);
        CHECK(memcmp(parameters + Shot_Muzzle, shot_muzzle, sizeof(shot_muzzle)) == 0);
    }
    return parameters != NULL && memcmp(fired.directions, straight, sizeof(straight)) != 0;
}

static void test_shots(void) {
    set_mod(true);
    memset(clients, 0, sizeof(clients));
    set_view(0, true, 0, 0);
    set_view(1, true, 0, 1);
    orig_Bullet_Fire = game_bullet;
    set_text("aae_freeaim_target", "-139 -1108 140");
    set_dvar_kind("aae_freeaim_target_1", false, "", Dvar_Type_String);
    set_shooter(0, true, true);
    // free aim not on yet for the player's client: straight ahead
    CHECK(!fires(shooter, shot));
    CHECK(after_a_while(0, false));
    // on: the shot goes from the muzzle at the point the mod's script left
    CHECK(fires(shooter, shot) && near(fired.directions[0], 0.70710678f) && near(fired.directions[1], 0.70710678f) &&
          near(fired.directions[2], 0.0f) && near(dot(fired.directions + 3, fired.directions + 3), 1));
    // each local client has a point of its own
    set_shooter(1, true, true);
    CHECK(after_a_while(1, false) && !fires(shooter, shot));
    set_text("aae_freeaim_target_1", "-239 -1208 500");
    CHECK(fires(shooter, shot) && near(fired.directions[2], 1.0f));
    set_shooter(0, true, true);
    CHECK(fires(shooter, shot) && near(fired.directions[0], 0.70710678f));
    // a point that can't be read, or isn't text, or isn't there: straight ahead
    set_text("aae_freeaim_target", "-139 -1108");
    CHECK(!fires(shooter, shot));
    set_text("aae_freeaim_target", "");
    CHECK(!fires(shooter, shot));
    set_dvar_kind("aae_freeaim_target", true, "-139 -1108 140", 4);
    CHECK(!fires(shooter, shot) && wrong_asks == 0);
    set_dvar_kind("aae_freeaim_target", false, "-139 -1108 140", Dvar_Type_String);
    CHECK(!fires(shooter, shot));
    // the point is at the muzzle
    set_text("aae_freeaim_target", "-239 -1208 140");
    CHECK(!fires(shooter, shot));
    set_text("aae_freeaim_target", "-139 -1108 140");
    CHECK(fires(shooter, shot));
    // the server says the player's view isn't third person, or the entity has no player's state
    set_shooter(0, false, true);
    CHECK(!fires(shooter, shot));
    set_shooter(0, true, false);
    CHECK(!fires(shooter, shot));
    // somebody who isn't a local client's player, or no player at all
    set_shooter(2, true, true);
    CHECK(!fires(shooter, shot));
    set_shooter(Max_Players, true, true);
    set_view(0, true, 0, Max_Players);
    CHECK(!fires(shooter, shot));
    set_view(0, true, 0, 0);
    set_shooter(0, true, true);
    CHECK(fires(shooter, shot));
    // free aim off for the client: the switch, first person on the client's side, not seen lately
    set_dvar("aae_freeaim_enabled", true, 0);
    CHECK(!fires(shooter, shot));
    set_dvar("aae_freeaim_enabled", false, 1);
    CHECK(fires(shooter, shot));
    set_view(0, false, 0, 0);
    CHECK(!fires(shooter, shot));
    set_view(0, true, 0, 0);
    CHECK(!fires(shooter, shot)); // not for a quarter of a second
    CHECK(after_a_while(0, false) && fires(shooter, shot));
    test_now += Seen_For_Ms + 1;
    CHECK(!fires(shooter, shot));
    CHECK(after_a_while(0, false) && fires(shooter, shot));
    // without the mod
    set_mod(false);
    CHECK(!fires(shooter, shot));
    set_mod(true);
    CHECK(fires(shooter, shot));
    // a shot's directions that aren't what they should be are left alone
    float crooked = 1.2f;
    memcpy(shot, &crooked, sizeof(crooked));
    fired.calls = 0;
    fake_Bullet_Fire(shooter, shot, 0, 0, 0, 0, 0.0f);
    CHECK(fired.calls == 1 && fired.directions[0] == 1.2f && memcmp(shot, &crooked, sizeof(crooked)) == 0);
    // no entity, no parameters
    CHECK(!fires(NULL, NULL));
    set_shooter(0, true, true);
    fired.calls = 0;
    fake_Bullet_Fire(shooter, NULL, 0, 0, 0, 0, 0.0f);
    CHECK(fired.calls == 1);
    memset(clients, 0, sizeof(clients));
}

// ---- rockets, grenades and other projectiles

static bool about(float a, float b) {
    return fabsf(a - b) < 0.01f;
}
// something flying from this place: whether it is turned at the target, and with what
static float flies_with[3], flies_at[3];
static bool flies(const float velocity[3], const float target[3]) {
    const float from[3] = { 10, 20, 30 };
    for (int i = 0; i < 3; i++)
        flies_with[i] = flies_at[i] = -77.0f;
    bool turned = aae_freeaim_projectile(from, velocity, target, flies_with, flies_at);
    if (!turned) // nothing is given out then
        for (int i = 0; i < 3; i++)
            CHECK(flies_with[i] == -77.0f && flies_at[i] == -77.0f);
    return turned;
}
static void test_projectile(void) {
    const float along[3] = { 800, 0, 0 };
    // at a target straight ahead: as it flew, level, heading along x
    CHECK(flies(along, (float[3]){ 510, 20, 30 }) && flies_with[0] == 800.0f && flies_with[1] == 0.0f &&
          flies_with[2] == 0.0f && flies_at[0] == 0.0f && flies_at[1] == 0.0f && flies_at[2] == 0.0f);
    // to the side: as fast as before, heading there (yaw 90 is along y)
    CHECK(flies(along, (float[3]){ 10, 500, 30 }) && about(flies_with[0], 0) && about(flies_with[1], 800) &&
          about(flies_with[2], 0) && about(flies_at[0], 0) && about(flies_at[1], 90) && flies_at[2] == 0.0f);
    CHECK(flies(along, (float[3]){ 10, -500, 30 }) && about(flies_with[1], -800) && about(flies_at[1], -90));
    CHECK(flies(along, (float[3]){ -500, 20, 30 }) && about(flies_with[0], -800) && about(fabsf(flies_at[1]), 180));
    // up is a pitch below 0, down one above
    CHECK(flies(along, (float[3]){ 10, 20, 530 }) && about(flies_with[2], 800) && about(flies_at[0], -90));
    CHECK(flies(along, (float[3]){ 10, 20, -530 }) && about(flies_with[2], -800) && about(flies_at[0], 90));
    CHECK(flies(along, (float[3]){ 310, 20, 330 }) && about(flies_with[0], 565.685f) &&
          about(flies_with[2], 565.685f) && about(flies_at[0], -45) && about(flies_at[1], 0));
    // whichever way it flew: as fast as before
    const float askew[3] = { 300, -400, 1200 };
    CHECK(flies(askew, (float[3]){ -700, 900, 10 }) &&
          about(sqrtf(dot(flies_with, flies_with)), 1300) && about(flies_at[1], 128.9f));
    // straight up or down, where its "right" is made from another side: turned all the same
    CHECK(flies((float[3]){ 0, 0, 800 }, (float[3]){ 510, 20, 30 }) && about(flies_with[0], 800) &&
          about(flies_at[0], 0));
    CHECK(flies((float[3]){ 0, 0, -800 }, (float[3]){ 10, 520, 30 }) && about(flies_with[1], 800) &&
          about(flies_at[1], 90));
    // as steep as 0.99 of its way and less: from the usual side; steeper: from the other one. Either gives the same
    // velocity, so both are only seen to work
    CHECK(flies((float[3]){ 142.0f, 0, 989.0f }, (float[3]){ 510, 20, 30 }) && about(flies_with[0], 999.14f));
    CHECK(flies((float[3]){ 140.0f, 0, 990.2f }, (float[3]){ 510, 20, 30 }) && about(flies_with[0], 1000.05f));
    // slower than 0.0001 isn't flying
    CHECK(!flies((float[3]){ 0, 0, 0 }, (float[3]){ 510, 20, 30 }) &&
          !flies((float[3]){ 0.00009f, 0, 0 }, (float[3]){ 510, 20, 30 }));
    CHECK(flies((float[3]){ 0.00011f, 0, 0 }, (float[3]){ 10, 520, 30 }) && about(flies_with[1] * 10000.0f, 1.1f));
    // a velocity that isn't numbers, or whose length isn't one
    CHECK(!flies((float[3]){ NAN, 0, 0 }, (float[3]){ 510, 20, 30 }) &&
          !flies((float[3]){ 0, INFINITY, 0 }, (float[3]){ 510, 20, 30 }) &&
          !flies((float[3]){ 0, 0, 3.0e19f }, (float[3]){ 510, 20, 30 }));
    // a target where it is, or one that isn't numbers
    CHECK(!flies(along, (float[3]){ 10, 20, 30 }) && !flies(along, (float[3]){ NAN, 20, 30 }));
}

// what the server made, and the game's three functions that make such things
static uint8_t made[0x4f8];
static const float made_from[3] = { -239.0f, -1208.0f, 140.0f }, made_with[3] = { 900.0f, 0.0f, 0.0f },
                   made_at[3] = { 3.0f, 4.0f, 5.0f };
static struct {
    int calls, kind;
    uint8_t *owner, *gives;
    uint64_t rest[7];
} making;
static uint8_t *game_made(int kind, uint8_t *owner, const uint64_t rest[7]) {
    making.calls++, making.kind = kind, making.owner = owner;
    memcpy(making.rest, rest, sizeof(making.rest));
    return making.gives;
}
static uint8_t *game_grenade(uint8_t *owner, uint64_t second, uint64_t third, uint64_t fourth, uint64_t fifth,
                             uint64_t sixth, uint64_t seventh) {
    return game_made(0, owner, (uint64_t[7]){ second, third, fourth, fifth, sixth, seventh, 0 });
}
static uint8_t *game_rocket(uint8_t *owner, uint64_t second, uint64_t third, uint64_t fourth, uint64_t fifth,
                            uint64_t sixth, uint64_t seventh) {
    return game_made(1, owner, (uint64_t[7]){ second, third, fourth, fifth, sixth, seventh, 0 });
}
static uint8_t *game_projectile(uint8_t *owner, uint64_t second, uint64_t third, uint64_t fourth, uint64_t fifth,
                                uint64_t sixth, uint64_t seventh, uint64_t eighth) {
    return game_made(2, owner, (uint64_t[7]){ second, third, fourth, fifth, sixth, seventh, eighth });
}
// The server makes one of the three for this entity: whether it was turned. Either way the game's function is
// called once with everything it was given, what it made is handed back, and nothing but the velocity and the
// angles of the new thing is touched.
static float made_velocity[3], made_angles[3];
static bool makes(int kind, uint8_t *owner) {
    uint8_t before[sizeof(made)];
    memset(made, 0x6b, sizeof(made));
    memcpy(made + Entity_Origin, made_from, sizeof(made_from));
    memcpy(made + Entity_Velocity, made_with, sizeof(made_with));
    memcpy(made + Entity_Angles, made_at, sizeof(made_at));
    memcpy(before, made, sizeof(made));
    making.calls = 0, making.gives = made;
    uint8_t *given = kind == 0   ? fake_Grenade_Spawn(owner, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77)
                     : kind == 1 ? fake_Rocket_Spawn(owner, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77)
                                 : fake_Projectile_Spawn(owner, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    CHECK(given == made && making.calls == 1 && making.kind == kind && making.owner == owner);
    CHECK(making.rest[0] == 0x22 && making.rest[1] == 0x33 && making.rest[2] == 0x44 && making.rest[3] == 0x55 &&
          making.rest[4] == 0x66 && making.rest[5] == 0x77 && making.rest[6] == (kind == 2 ? 0x88 : 0));
    memcpy(made_velocity, made + Entity_Velocity, sizeof(made_velocity));
    memcpy(made_angles, made + Entity_Angles, sizeof(made_angles));
    bool turned = memcmp(made_velocity, made_with, sizeof(made_with)) != 0;
    CHECK(turned == (memcmp(made_angles, made_at, sizeof(made_at)) != 0));
    memcpy(made + Entity_Velocity, made_with, sizeof(made_with));
    memcpy(made + Entity_Angles, made_at, sizeof(made_at));
    CHECK(memcmp(before, made, sizeof(made)) == 0);
    return turned;
}

static void test_projectiles(void) {
    set_mod(true);
    memset(clients, 0, sizeof(clients));
    set_view(0, true, 0, 0);
    set_view(1, true, 0, 1);
    orig_Grenade_Spawn = game_grenade, orig_Rocket_Spawn = game_rocket, orig_Projectile_Spawn = game_projectile;
    set_text("aae_freeaim_target", "-239 -1108 140");
    set_dvar_kind("aae_freeaim_target_1", false, "", Dvar_Type_String);
    set_dvar_kind("aae_freeaim_nonbullet", false, "", Dvar_Type_String);
    set_shooter(0, true, true);
    // free aim not on yet for the player's client: as the game made it
    CHECK(!makes(0, shooter) && !makes(1, shooter) && !makes(2, shooter));
    CHECK(after_a_while(0, false));
    // on: each of the three flies from where it is at the point the mod's script left, as fast as before, and has
    // the angles of its way
    for (int kind = 0; kind < 3; kind++)
        CHECK(makes(kind, shooter) && about(made_velocity[0], 0) && about(made_velocity[1], 900) &&
              about(made_velocity[2], 0) && about(made_angles[0], 0) && about(made_angles[1], 90) &&
              made_angles[2] == 0.0f);
    // each local client has a point of its own
    set_shooter(1, true, true);
    CHECK(after_a_while(1, false) && !makes(1, shooter));
    set_text("aae_freeaim_target_1", "-239 -1208 500");
    CHECK(makes(1, shooter) && about(made_velocity[2], 900) && about(made_angles[0], -90));
    // two local clients with the same player's number: the first one's point and switch
    set_view(1, true, 0, 0);
    set_shooter(0, true, true);
    CHECK(makes(1, shooter) && about(made_velocity[1], 900));
    set_text("aae_freeaim_target", "nowhere");
    CHECK(!makes(1, shooter));
    set_text("aae_freeaim_target", "-239 -1108 140");
    // a client the crosshair code was never at work for is passed over
    clients[0].cg = NULL;
    CHECK(makes(1, shooter) && about(made_velocity[2], 900));
    clients[0].cg = cg_of(0);
    set_view(1, true, 0, 1);
    CHECK(makes(1, shooter) && about(made_velocity[1], 900));
    // the switch for things that aren't bullets: 0 is off, anything else, nothing and what can't be read are on
    set_text("aae_freeaim_nonbullet", "0");
    CHECK(!makes(0, shooter) && !makes(1, shooter) && !makes(2, shooter));
    fired.calls = 0;
    CHECK(fires(shooter, shot)); // bullets don't go by it
    set_text("aae_freeaim_nonbullet", "-0.0");
    CHECK(!makes(1, shooter));
    const char *const on[] = { "1", "0.5", "-3", "on", "", "0 0" };
    for (size_t i = 0; i < sizeof(on) / sizeof(on[0]); i++) {
        set_text("aae_freeaim_nonbullet", on[i]);
        CHECK(makes(1, shooter));
    }
    set_dvar_kind("aae_freeaim_nonbullet", true, "0", 6);
    CHECK(makes(1, shooter) && wrong_asks == 0);
    set_dvar_kind("aae_freeaim_nonbullet", false, "", Dvar_Type_String);
    CHECK(makes(1, shooter));
    // the server says the player's view isn't third person (any byte but 0 is), or the entity has no player's state
    set_shooter(0, false, true);
    CHECK(!makes(1, shooter));
    set_shooter(0, true, true);
    shooter_state[Ps_ThirdPerson] = 2;
    CHECK(makes(1, shooter));
    set_shooter(0, true, false);
    CHECK(!makes(1, shooter));
    // somebody who isn't a local client's player, or no player at all (63 is the last one that can be)
    set_shooter(2, true, true);
    CHECK(!makes(1, shooter));
    set_shooter(Max_Players, true, true);
    set_view(0, true, 0, Max_Players);
    CHECK(!makes(1, shooter));
    set_shooter(Max_Players - 1, true, true);
    set_view(0, true, 0, Max_Players - 1);
    CHECK(makes(1, shooter));
    set_view(0, true, 0, 0);
    set_shooter(0, true, true);
    CHECK(makes(1, shooter));
    // free aim off for the client, or the mod not there
    set_dvar("aae_freeaim_enabled", true, 0);
    CHECK(!makes(0, shooter) && !makes(1, shooter) && !makes(2, shooter));
    set_dvar("aae_freeaim_enabled", false, 1);
    CHECK(makes(1, shooter));
    set_mod(false);
    CHECK(!makes(0, shooter) && !makes(1, shooter) && !makes(2, shooter));
    set_mod(true);
    CHECK(makes(0, shooter) && makes(1, shooter) && makes(2, shooter));
    // the point is where the thing is: as the game made it
    set_text("aae_freeaim_target", "-239 -1208 140");
    CHECK(!makes(1, shooter));
    set_text("aae_freeaim_target", "-239 -1108 140");
    // nobody fired it; and the game made nothing
    CHECK(!makes(0, NULL) && !makes(1, NULL) && !makes(2, NULL));
    making.gives = NULL, making.calls = 0;
    CHECK(fake_Grenade_Spawn(shooter, 1, 2, 3, 4, 5, 6) == NULL &&
          fake_Rocket_Spawn(shooter, 1, 2, 3, 4, 5, 6) == NULL &&
          fake_Projectile_Spawn(shooter, 1, 2, 3, 4, 5, 6, 7) == NULL && making.calls == 3);
    memset(clients, 0, sizeof(clients));
}

// ---- players on other machines: the two hooks, and shots that go by what a machine sent

// the made-up game's server and connections
static int32_t game_server_state;       // 0: this machine runs no server
static uint8_t *game_connections;       // two clientConnection_t
static uint8_t *game_server_clients;    // svs.clients: four client_t
static struct {
    int calls, local_client;
    char text[AAE_FREEAIM_NET_TEXT];
} queued;
static void game_add_reliable(int local_client, const char *text) {
    queued.calls++, queued.local_client = local_client;
    snprintf(queued.text, sizeof(queued.text), "%s", text);
}
static struct {
    int calls, local_client, queued_before;
    void *command;
} finished;
static void game_finish_move(int local_client, void *command) {
    finished.calls++, finished.local_client = local_client, finished.command = command;
    finished.queued_before = queued.calls;
}
static struct {
    int calls, client_ok, from_old_server;
    uint8_t *client;
    const char *text;
} executed;
static void game_execute(uint8_t *client, const char *text, int client_ok, int from_old_server) {
    executed.calls++, executed.client = client, executed.text = text;
    executed.client_ok = client_ok, executed.from_old_server = from_old_server;
}
static uint8_t *server_client(int player) {
    return game_server_clients + (size_t)player * Client_Size;
}
static void set_client_state(int player, int32_t state) {
    memcpy(server_client(player), &state, sizeof(state));
}
// the net code with the made-up game where aae_freeaim_init gave it the game's addresses
static void net_for_tests(void) {
    aae_freeaim_net_game_t with = net_game;
    CL_AddReliableCommand = game_add_reliable;
    with.server_state = &game_server_state;
    with.client_connections = &game_connections;
    with.server_clients = &game_server_clients;
    aae_freeaim_net_init(&with);
}
// a client's command comes in at the server: whether the game got it (then with everything it came with)
static bool reaches_the_game(uint8_t *client, const char *text, int from_old_server) {
    executed.calls = 0;
    fake_SV_ExecuteClientCommand(client, text, 7, from_old_server);
    if (executed.calls == 1)
        CHECK(executed.client == client && executed.text == text && executed.client_ok == 7 &&
              executed.from_old_server == from_old_server);
    return executed.calls == 1;
}
// a frame of a local client goes out: whether a command was queued with it (after the game's own function ran, once,
// with what it was given)
static bool queues(int local_client) {
    static int a_command;
    queued.calls = 0, finished.calls = 0;
    fake_CL_FinishMove(local_client, &a_command);
    CHECK(finished.calls == 1 && finished.local_client == local_client && finished.command == &a_command &&
          finished.queued_before == 0);
    CHECK(queued.calls <= 1 && (queued.calls == 0 || queued.local_client == local_client));
    return queued.calls == 1;
}

static void test_other_machines(void) {
    set_mod(true);
    memset(clients, 0, sizeof(clients));
    net_for_tests();
    orig_Bullet_Fire = game_bullet, orig_SV_ExecuteClientCommand = game_execute, orig_CL_FinishMove = game_finish_move;
    orig_Grenade_Spawn = game_grenade, orig_Rocket_Spawn = game_rocket, orig_Projectile_Spawn = game_projectile;
    set_view(0, true, 0, 0);
    set_view(1, true, 0, 1);
    set_text("aae_freeaim_target", "-239 -1208 500");
    set_dvar_kind("aae_freeaim_target_1", false, "", Dvar_Type_String);
    set_dvar_kind("aae_freeaim_nonbullet", false, "", Dvar_Type_String);
    game_server_state = 2;
    for (int player = 0; player < 4; player++)
        set_client_state(player, 5);

    // The server's side. Player 2 is on another machine: no local client is theirs, so their shots go straight
    set_shooter(2, true, true);
    CHECK(!fires(shooter, shot) && !makes(1, shooter));
    // their machine says where they aim: the command doesn't reach the game, and their shots go there (the numbers
    // are half the point's)
    CHECK(reaches_the_game(server_client(2), "say hi", 0) && reaches_the_game(server_client(2), "aaeoff", 0));
    // (-140 -1108 140 is 99 and 100 away from where the shot starts)
    CHECK(!reaches_the_game(server_client(2), "aae -70 -554 70", 0));
    CHECK(fires(shooter, shot) && near(fired.directions[0], 0.70354f) && near(fired.directions[1], 0.71065f) &&
          near(fired.directions[2], 0.0f));
    CHECK(makes(0, shooter) && makes(1, shooter) && makes(2, shooter) && about(made_velocity[0], 633.19f) &&
          about(made_velocity[1], 639.585f) && about(made_velocity[2], 0));
    // a command from a server this one took over from is taken, and nothing is kept of it
    CHECK(!reaches_the_game(server_client(3), "aae -70 -554 70", 1));
    set_shooter(3, true, true);
    CHECK(!fires(shooter, shot));
    set_shooter(2, true, true);
    // the dvar aae_freeaim_enabled switches it off on the server too; one that can't be read is off here
    set_text("aae_freeaim_enabled", "0");
    CHECK(!fires(shooter, shot) && !makes(1, shooter));
    set_text("aae_freeaim_enabled", "on");
    CHECK(!fires(shooter, shot));
    set_dvar("aae_freeaim_enabled", false, 1);
    CHECK(fires(shooter, shot));
    // A player who sent a point and is a local client's as well: the point that was sent counts
    set_view(0, true, 0, 2);
    CHECK(after_a_while(0, false) && fires(shooter, shot) && near(fired.directions[0], 0.70354f));
    // and when it doesn't count any more (the client isn't in the game, but its entry is still there), nothing
    // does: the local client's own point is not used for them
    set_client_state(2, 4);
    CHECK(!fires(shooter, shot) && !makes(1, shooter));
    set_client_state(2, 5);
    CHECK(fires(shooter, shot) && near(fired.directions[0], 0.70354f));
    // "aae off": the entry is gone, and the local client's point is theirs again
    CHECK(!reaches_the_game(server_client(2), "aae off", 0));
    CHECK(fires(shooter, shot) && near(fired.directions[2], 1.0f));
    set_view(0, true, 0, 0);
    // without the mod the command is the game's
    set_mod(false);
    CHECK(reaches_the_game(server_client(2), "aae -70 -554 70", 0));
    set_mod(true);
    CHECK(!fires(shooter, shot));

    // The client's side. With a server on this machine nothing is sent
    memset(clients, 0, sizeof(clients));
    net_for_tests();
    set_text("aae_freeaim_target", "-139 -1108 140");
    CHECK(after_a_while(0, false) && !queues(0));
    // in somebody else's game: the point under the crosshair, halved, once
    game_server_state = 0;
    test_now += 1000;
    CHECK(crosshair_asks(0, false) && queues(0) && strcmp(queued.text, "aae -70 -554 70") == 0);
    test_now += 1000;
    CHECK(crosshair_asks(0, false) && !queues(0));
    // free aim off: "aae off", once
    set_view(0, false, 0, 0);
    test_now += 1000;
    CHECK(!crosshair_asks(0, false) && queues(0) && strcmp(queued.text, "aae off") == 0 && !queues(0));
    set_view(0, true, 0, 0);
    // a local client the crosshair code was never at work for sends nothing, and counts as not in third person
    set_text("aae_freeaim_target_1", "5 6 7");
    CHECK(!queues(1) && (clients[1].third_person & 3) == 1);
    // a cg_t that isn't the client's in the game's array any more is not read (this one can't be)
    CHECK(after_a_while(0, false));
    clients[0].cg = (const void *)16;
    set_shooter(0, true, true);
    test_now += 1000;
    CHECK(!queues(0) && !fires(shooter, shot) && !makes(1, shooter));
    game_server_state = 2;
    memset(clients, 0, sizeof(clients));
    net_for_tests();
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
        CHECK(hooks == 7 && was_hooked(ADDR_CG_DrawCrosshairTest, (void *)fake_CG_DrawCrosshairTest,
                                       (void **)&orig_CG_DrawCrosshairTest) &&
              was_hooked(ADDR_Bullet_Fire, (void *)fake_Bullet_Fire, (void **)&orig_Bullet_Fire) &&
              was_hooked(ADDR_Grenade_Spawn, (void *)fake_Grenade_Spawn, (void **)&orig_Grenade_Spawn));
    }
    have_stubs = true, stub_missing_for = -1;
    hooks = 0;
    // (what the net code keeps is emptied when it is set up)
    atomic_store(&aae_freeaim_net_table()->in_use, 5);
    aae_freeaim_net_sender(1)->sent_at = 9;
    aae_freeaim_init(base, 1);
    CHECK(atomic_load(&aae_freeaim_net_table()->in_use) == 0 && aae_freeaim_net_sender(1)->sent_at == 0);
    CHECK(was_hooked(ADDR_CL_FinishMove, (void *)fake_CL_FinishMove, (void **)&orig_CL_FinishMove) &&
          was_hooked(ADDR_SV_ExecuteClientCommand, (void *)fake_SV_ExecuteClientCommand,
                     (void **)&orig_SV_ExecuteClientCommand));
    CHECK(net_game.add_reliable_command == queue_command &&
          CL_AddReliableCommand == (void *)(base + ADDR_CL_AddReliableCommand) &&
          net_game.server_state == (void *)(base + ADDR_sv_state) &&
          net_game.client_connections == (void *)(base + ADDR_clientConnections) &&
          net_game.server_clients == (void *)(base + ADDR_svs_clients) && net_game.client_size == 0xe5270 &&
          net_game.active == aae_plugin_loaded_lately && net_game.on == net_on && net_game.map == net_map &&
          net_game.local_target == local_target && net_game.now_ms == net_now);
    CHECK(hooks == 9 && was_hooked(ADDR_CG_DrawCrosshairTest, (void *)fake_CG_DrawCrosshairTest,
                                   (void **)&orig_CG_DrawCrosshairTest));
    CHECK(was_hooked(ADDR_Bullet_Fire, (void *)fake_Bullet_Fire, (void **)&orig_Bullet_Fire));
    CHECK(was_hooked(ADDR_Grenade_Spawn, (void *)fake_Grenade_Spawn, (void **)&orig_Grenade_Spawn) &&
          was_hooked(ADDR_Rocket_Spawn, (void *)fake_Rocket_Spawn, (void **)&orig_Rocket_Spawn) &&
          was_hooked(ADDR_Projectile_Spawn, (void *)fake_Projectile_Spawn, (void **)&orig_Projectile_Spawn));
    CHECK(was_hooked(ADDR_CG_DrawCrosshair_Tests, (void *)handler_Crosshair_Tests, &orig_Crosshair_Tests) &&
          other_Crosshair_Tests == (void *)(base + ADDR_CG_DrawCrosshair_Draws));
    CHECK(was_hooked(ADDR_CG_DrawCrosshair_Gap, (void *)handler_Crosshair_Gap, &orig_Crosshair_Gap));
    // the places in the game's own structures, as they were read from its code
    CHECK(Cg_Size == 0x342a80 && Cg_ThirdPerson == 0x11a8c9 && Cg_PlayerState == 0x11a8d0 && Ps_Kind == 4 &&
          Ps_Sights == 0x2fc && Frame_Sights == -0x50 && Weapon_LeastGap == 0x6f0 && Dvar_Type == 0x1c &&
          Dvar_Type_String == 8 && Entity_Client == 0x250 && Ps_ThirdPerson == 0xbc && Shot_Muzzle == 0x24 &&
          Max_Players == 64 && Entity_Origin == 0x1c && Entity_Velocity == 0x28 && Entity_Angles == 0x23c &&
          Client_Size == 0xe5270);
    CHECK(Dvar_FindByHash == (void *)(base + ADDR_Dvar_FindByHash) &&
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
    game_connections = calloc(2, 0x25780);
    game_server_clients = calloc(4, Client_Size);

    test_init();
    loaded_mod = loaded_name;
    Dvar_FindByHash = game_find;
    Dvar_GetString = game_string;
    set_text("mapname", "zm_theater");
    cg_clients = &game_cgs;
    now_ms = test_clock;
    net_for_tests();
    test_rule();
    test_hook();
    test_gap();
    test_aiming();
    test_like_the_plugin();
    test_numbers();
    test_turn();
    test_shots();
    test_projectile();
    test_projectiles();
    test_other_machines();

    free(game_cgs);
    free(game_connections);
    free(game_server_clients);
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
