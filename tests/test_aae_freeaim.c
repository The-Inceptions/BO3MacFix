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
// The point's text replaced while it is read, as the mod's script does it from the client's thread: from the given
// ask for aae_freeaim_target on, the game hands out another text at another place, and what the old one was is gone
// (every time, when it keeps being replaced).
static int target_asks, replaced_at_ask;
static bool replaced_every_time;
static char replaced_text[2][200];
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
    if (strcmp(d->name, "aae_freeaim_target") == 0 && replaced_at_ask != 0 && ++target_asks >= replaced_at_ask) {
        int asked = target_asks - replaced_at_ask;
        if (asked == 0 || replaced_every_time)
            memset(d->text, '9', sizeof(d->text) - 1); // the old text's place belongs to something else now
        return replaced_text[replaced_every_time ? asked % 2 : 0];
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
    // The script replaces the point's text while the shot reads it: between the two times the dvar is asked the old
    // text's place has become something else's. The shot goes at the new point, not at what the old place holds now.
    snprintf(replaced_text[0], sizeof(replaced_text[0]), "-239 -1208 500");
    target_asks = 0;
    replaced_at_ask = 2;
    CHECK(fires(shooter, shot) && near(fired.directions[2], 1.0f) && target_asks == 4);
    // a text that is replaced every time it is looked at is given up after four tries: straight ahead
    snprintf(replaced_text[1], sizeof(replaced_text[1]), "-239 -1208 500");
    set_text("aae_freeaim_target", "-139 -1108 140");
    target_asks = 0;
    replaced_every_time = true;
    CHECK(!fires(shooter, shot) && target_asks == 8);
    replaced_at_ask = 0;
    replaced_every_time = false;
    // a text of 191 characters is read, one of 192 isn't (the plugin's copy is 192 bytes with its end)
    char long_text[200];
    memset(long_text, ' ', sizeof(long_text));
    memcpy(long_text, "-139 -1108 140", 14);
    long_text[191] = '\0';
    set_text("aae_freeaim_target", long_text);
    CHECK(fires(shooter, shot) && near(fired.directions[0], 0.70710678f));
    long_text[191] = ' ';
    long_text[192] = '\0';
    set_text("aae_freeaim_target", long_text);
    CHECK(!fires(shooter, shot));
    set_text("aae_freeaim_target", "-139 -1108 140");
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

// ---- the client's side of a shot

// the client's fire function: its frame, which holds the shooting entity's centity_s, and that entity
static uint8_t fire_frame_room[0x100], shooting_cent[0x400];
static uint8_t *const fire_frame = fire_frame_room + 0x80;
static void set_shooting(uint32_t number, bool there) {
    const uint8_t *cent = there ? shooting_cent : NULL;
    memcpy(fire_frame + Fire_Frame_Cent, &cent, sizeof(cent));
    memcpy(shooting_cent + Cent_Number, &number, sizeof(number));
}
// the game's function that works out where the client's copy of a shot ends: what it was given
static struct {
    int calls;
    uint64_t seed, end, direction, weapon, pellet, pellets;
    float spread, spread2, fraction, fraction2, range, directions[9];
    float *start, *forward, *right, *up;
} endpos;
static void game_endpos(uint64_t seed, float spread, float spread2, float *start, uint64_t end, uint64_t direction,
                        float fraction, float fraction2, float *forward, float *right, float range, float *up,
                        uint64_t weapon, uint64_t pellet, uint64_t pellets) {
    endpos.calls++;
    endpos.seed = seed, endpos.end = end, endpos.direction = direction, endpos.weapon = weapon;
    endpos.pellet = pellet, endpos.pellets = pellets;
    endpos.spread = spread, endpos.spread2 = spread2, endpos.fraction = fraction, endpos.fraction2 = fraction2;
    endpos.range = range;
    endpos.start = start, endpos.forward = forward, endpos.right = right, endpos.up = up;
    memset(endpos.directions, 0, sizeof(endpos.directions));
    if (forward != NULL && right != NULL && up != NULL) {
        memcpy(endpos.directions, forward, 3 * sizeof(float));
        memcpy(endpos.directions + 3, right, 3 * sizeof(float));
        memcpy(endpos.directions + 6, up, 3 * sizeof(float));
    }
}
// where the shot starts (the eye) and its directions as the game has them
static float eye[3] = { -239.0f, -1208.0f, 140.0f }, eye_directions[9];
// The client's fire function asks for its copy of a shot from its first call (0), its second (1) or somewhere else
// (2): whether it was worked out with other directions than its own. Either way the game's function is called once
// with everything else as it came, and the shot's own start and directions are as they were.
static bool client_fires(int call, const uint8_t *frame) {
    static const uint8_t elsewhere;
    const void *caller = call == 0 ? endpos_asked_from[0] : call == 1 ? endpos_asked_from[1] : &elsewhere;
    float start[3], directions[9];
    memcpy(start, eye, sizeof(start));
    memcpy(directions, eye_directions, sizeof(directions));
    endpos.calls = 0;
    client_shot(caller, frame, 0x11, 0.5f, 1.5f, start, 0x22, 0x33, 0.25f, 0.75f, directions, directions + 3, 8192.0f,
                directions + 6, 0x44, 0x55, 0x66);
    CHECK(endpos.calls == 1 && endpos.seed == 0x11 && endpos.spread == 0.5f && endpos.spread2 == 1.5f &&
          endpos.start == start && endpos.end == 0x22 && endpos.direction == 0x33 && endpos.fraction == 0.25f &&
          endpos.fraction2 == 0.75f && endpos.range == 8192.0f && endpos.weapon == 0x44 && endpos.pellet == 0x55 &&
          endpos.pellets == 0x66);
    CHECK(memcmp(start, eye, sizeof(start)) == 0 && memcmp(directions, eye_directions, sizeof(directions)) == 0);
    bool turned = memcmp(endpos.directions, eye_directions, sizeof(eye_directions)) != 0;
    if (!turned)
        CHECK(endpos.forward == directions && endpos.right == directions + 3 && endpos.up == directions + 6);
    return turned;
}
// it was turned at this point from the eye: forward points there, and the three are still of length 1 and at right
// angles
static bool turned_at(const float target[3]) {
    float to[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
    float length = sqrtf(dot(to, to));
    const float *d = endpos.directions;
    return near(d[0], to[0] / length) && near(d[1], to[1] / length) && near(d[2], to[2] / length) &&
           near(dot(d, d), 1) && near(dot(d + 3, d + 3), 1) && near(dot(d + 6, d + 6), 1) && near(dot(d, d + 3), 0) &&
           near(dot(d, d + 6), 0) && near(dot(d + 3, d + 6), 0);
}

// a clock that goes on by a millisecond each time it is read
static uint64_t ticking_clock(void) {
    return test_now++;
}
// the crosshair code is at work for both local clients now
static void both_seen(void) {
    crosshair_asks(0, false);
    crosshair_asks(1, false);
}

static void test_client_shots(void) {
    const float there[3] = { -139.0f, -1108.0f, 140.0f }, above[3] = { -239.0f, -1208.0f, 500.0f };
    set_mod(true);
    memset(clients, 0, sizeof(clients));
    set_view(0, true, 0, 0);
    set_view(1, true, 0, 1);
    orig_Bullet_Endpos = game_endpos;
    memcpy(eye_directions, straight, sizeof(straight));
    set_text("aae_freeaim_target", "-139 -1108 140");
    set_dvar_kind("aae_freeaim_target_1", false, "", Dvar_Type_String);
    set_shooting(0, true);
    // free aim not on yet for the player's client: straight ahead
    CHECK(!client_fires(1, fire_frame));
    both_seen();
    test_now += Third_Person_For_Ms;
    both_seen();
    // on: from both of the fire function's calls, turned at the point the mod's script left; from anywhere else not
    CHECK(client_fires(1, fire_frame) && turned_at(there));
    CHECK(client_fires(0, fire_frame) && turned_at(there));
    CHECK(!client_fires(2, fire_frame));
    // and the hook itself goes by where it returns to, which isn't the fire function here
    endpos.calls = 0;
    fake_Bullet_Endpos(1, 2.0f, 3.0f, eye, 4, 5, 6.0f, 7.0f, eye_directions, eye_directions + 3, 8.0f,
                       eye_directions + 6, 9, 10, 11);
    CHECK(endpos.calls == 1 && endpos.seed == 1 && endpos.spread == 2.0f && endpos.spread2 == 3.0f &&
          endpos.start == eye && endpos.end == 4 && endpos.direction == 5 && endpos.fraction == 6.0f &&
          endpos.fraction2 == 7.0f && endpos.forward == eye_directions && endpos.right == eye_directions + 3 &&
          endpos.range == 8.0f && endpos.up == eye_directions + 6 && endpos.weapon == 9 && endpos.pellet == 10 &&
          endpos.pellets == 11);
    // each local client has a point of its own
    set_shooting(1, true);
    CHECK(!client_fires(1, fire_frame));
    set_text("aae_freeaim_target_1", "-239 -1208 500");
    CHECK(client_fires(1, fire_frame) && turned_at(above));
    set_shooting(0, true);
    CHECK(client_fires(1, fire_frame) && turned_at(there));
    // two local clients with the same player's number: the first one decides, with its point and its switch
    set_view(1, true, 0, 0);
    CHECK(client_fires(1, fire_frame) && turned_at(there));
    set_text("aae_freeaim_target", "nowhere");
    CHECK(!client_fires(1, fire_frame));
    set_text("aae_freeaim_target", "-139 -1108 140");
    set_view(0, false, 0, 0);
    CHECK(!client_fires(1, fire_frame));
    // a client the crosshair code was never at work for is passed over
    clients[0].cg = NULL;
    CHECK(client_fires(1, fire_frame) && turned_at(above));
    clients[0].cg = cg_of(0);
    set_view(0, true, 0, 0);
    set_view(1, true, 0, 1);
    both_seen();
    test_now += Third_Person_For_Ms;
    both_seen();
    CHECK(client_fires(1, fire_frame) && turned_at(there));
    // somebody who isn't a local client's player, or no player at all (63 is the last one that can be)
    set_shooting(2, true);
    CHECK(!client_fires(1, fire_frame));
    set_shooting(Max_Players, true);
    set_view(0, true, 0, Max_Players);
    CHECK(!client_fires(1, fire_frame));
    // (a client number no player can have counted as not third person, so its quarter of a second begins anew)
    set_shooting(Max_Players - 1, true);
    set_view(0, true, 0, Max_Players - 1);
    CHECK(!client_fires(1, fire_frame));
    both_seen();
    test_now += Third_Person_For_Ms;
    both_seen();
    CHECK(client_fires(1, fire_frame) && turned_at(there));
    set_view(0, true, 0, 0);
    set_shooting(0, true);
    CHECK(client_fires(1, fire_frame));
    // a player whose machine sent the server a point (this machine runs the server) isn't a local client's: nothing
    // of theirs is turned here, until the entry is gone
    set_client_state(0, 5);
    CHECK(!reaches_the_game(server_client(0), "aae -70 -554 70", 0));
    CHECK(!client_fires(1, fire_frame));
    CHECK(!reaches_the_game(server_client(0), "aae off", 0));
    CHECK(client_fires(1, fire_frame) && turned_at(there));
    // no frame, or no entity in it
    CHECK(!client_fires(1, NULL));
    set_shooting(0, false);
    CHECK(!client_fires(1, fire_frame));
    set_shooting(0, true);
    // free aim off for the client: the switch, first person, not seen lately; or the mod not there
    set_dvar("aae_freeaim_enabled", true, 0);
    CHECK(!client_fires(1, fire_frame));
    set_dvar("aae_freeaim_enabled", false, 1);
    CHECK(client_fires(1, fire_frame));
    set_view(0, false, 0, 0);
    CHECK(!client_fires(1, fire_frame));
    set_view(0, true, 0, 0);
    CHECK(!client_fires(1, fire_frame)); // not for a quarter of a second
    both_seen();
    test_now += Third_Person_For_Ms;
    both_seen();
    CHECK(client_fires(1, fire_frame));
    test_now += Seen_For_Ms + 1;
    CHECK(!client_fires(1, fire_frame));
    both_seen();
    test_now += Third_Person_For_Ms;
    both_seen();
    CHECK(client_fires(1, fire_frame));
    set_mod(false);
    CHECK(!client_fires(1, fire_frame));
    set_mod(true);
    CHECK(client_fires(1, fire_frame));
    // free aim on for the other client only: nothing for this one's shots, and the other one's are its own
    set_view(0, false, 0, 0);
    CHECK(!client_fires(1, fire_frame));
    set_shooting(1, true);
    CHECK(client_fires(1, fire_frame) && turned_at(above));
    // The plugin first asks whether free aim is on for any local client, whoever shoots. That asks the first client
    // even when the shot is the second one's, and so the first one's quarter of a second in third person can begin
    // there.
    both_seen();
    set_view(0, true, 0, 0);
    CHECK(client_fires(1, fire_frame) && turned_at(above));
    test_now += Third_Person_For_Ms;
    CHECK(crosshair_asks(0, false));
    crosshair_asks(1, false);
    set_shooting(0, true);
    // a shot that doesn't start at a point the plugin takes (each coordinate below 1e7 either way)
    eye[0] = 9999999.0f;
    CHECK(client_fires(1, fire_frame));
    eye[0] = 10000000.0f;
    CHECK(!client_fires(1, fire_frame));
    eye[0] = -10000000.0f;
    CHECK(!client_fires(1, fire_frame));
    eye[0] = NAN;
    CHECK(!client_fires(1, fire_frame));
    eye[0] = -239.0f, eye[1] = -INFINITY;
    CHECK(!client_fires(1, fire_frame));
    eye[1] = -1208.0f, eye[2] = 10000000.0f;
    CHECK(!client_fires(1, fire_frame));
    eye[2] = 140.0f;
    CHECK(client_fires(1, fire_frame) && turned_at(there));
    // the point can't be read, or is where the shot starts: straight ahead
    set_text("aae_freeaim_target", "-139 -1108");
    CHECK(!client_fires(1, fire_frame));
    set_text("aae_freeaim_target", "-239 -1208 140");
    CHECK(!client_fires(1, fire_frame));
    set_text("aae_freeaim_target", "-139 -1108 140");
    CHECK(client_fires(1, fire_frame) && turned_at(there));
    // directions that aren't what they should be
    eye_directions[0] = 1.2f;
    CHECK(!client_fires(1, fire_frame));
    memcpy(eye_directions, straight, sizeof(straight));
    // a direction or the start missing: everything handed on as it came
    float start[3], directions[9];
    memcpy(start, eye, sizeof(start));
    memcpy(directions, straight, sizeof(directions));
    for (int missing = 0; missing < 4; missing++) {
        endpos.calls = 0;
        client_shot(endpos_asked_from[1], fire_frame, 0, 0, 0, missing == 0 ? NULL : start, 0, 0, 0, 0,
                    missing == 1 ? NULL : directions, missing == 2 ? NULL : directions + 3, 0,
                    missing == 3 ? NULL : directions + 6, 0, 0, 0);
        CHECK(endpos.calls == 1 && endpos.start == (missing == 0 ? NULL : start) &&
              endpos.forward == (missing == 1 ? NULL : directions) &&
              endpos.right == (missing == 2 ? NULL : directions + 3) &&
              endpos.up == (missing == 3 ? NULL : directions + 6));
    }
    CHECK(client_fires(1, fire_frame));
    // Each of the plugin's questions whether free aim is on reads the clock anew: the first (any local client), the
    // second (the first local client with the shooter's number), the third (that client again) and the fourth (before
    // the gun's flash is looked for, which isn't when that one says no). When the client's quarter of a second since
    // the crosshair code was last at work for it runs out between the second and the third, nothing is turned.
    uint64_t seen = test_now;
    both_seen();
    now_ms = ticking_clock;
    test_now = seen + Seen_For_Ms - 2;
    CHECK(client_fires(1, fire_frame) && test_now == seen + Seen_For_Ms + 2);
    test_now = seen + Seen_For_Ms - 1;
    CHECK(!client_fires(1, fire_frame) && test_now == seen + Seen_For_Ms + 2);
    now_ms = test_clock;
    // a local client the crosshair code was never at work for is asked as well when the first isn't on, and is noted
    // as not in third person, as the plugin's rule does with it
    memset(clients, 0, sizeof(clients));
    set_view(0, false, 0, 0);
    crosshair_asks(0, false);
    CHECK(!client_fires(1, fire_frame) && clients[1].third_person == (test_now * 4 | 1));
    set_view(0, true, 0, 0);
    memset(clients, 0, sizeof(clients));
}

// ---- the client's side of a shot: the tracer from the gun

// a kept shot as it should be
static bool kept_shot(const aae_freeaim_shot_t *shot, const float start[3], const float flash[3],
                      const float target[3], uint64_t made_at, bool used) {
    return memcmp(shot->start, start, sizeof(shot->start)) == 0 &&
           memcmp(shot->flash, flash, sizeof(shot->flash)) == 0 &&
           memcmp(shot->target, target, sizeof(shot->target)) == 0 && shot->made_at == made_at && shot->used == used;
}
static bool same_point(const float a[3], const float b[3]) {
    return memcmp(a, b, 3 * sizeof(float)) == 0;
}

static void test_flash(void) {
    aae_freeaim_tracers_t kept;
    memset(&kept, 0, sizeof(kept));
    const float start[3] = { 100.0f, 200.0f, 300.0f };
    float flash[3] = { 9, 9, 9 };
    // none found and none found before: none (also right after the clock began)
    CHECK(!aae_freeaim_flash(&kept, start, NULL, 1000, flash) && flash[0] == 9);
    memcpy(kept.flash, start, sizeof(kept.flash));
    CHECK(!aae_freeaim_flash(&kept, start, NULL, 100, flash) && flash[0] == 9);
    // found at a time after now, however far after it: not
    kept.flash_at = UINT64_MAX;
    CHECK(!aae_freeaim_flash(&kept, start, NULL, 100, flash) && flash[0] == 9);
    kept.flash_at = 0;
    // one just found, 128 from the start at the most: it is the flash, and the one last found
    const float near_by[3] = { 100.0f, 200.0f + 128.0f, 300.0f }, farther[3] = { 100.0f, 200.0f + 128.01f, 300.0f };
    CHECK(aae_freeaim_flash(&kept, start, near_by, 1000, flash) && same_point(flash, near_by) &&
          same_point(kept.flash, near_by) && kept.flash_at == 1000);
    const float askew[3] = { 100.0f + 73.9f, 200.0f + 73.9f, 300.0f + 73.9f }; // just within 128 by a sum in floats
    CHECK(aae_freeaim_flash(&kept, start, askew, 1001, flash) && same_point(flash, askew) && kept.flash_at == 1001);
    const float askew_farther[3] = { 100.0f + 73.91f, 200.0f + 73.9f, 300.0f + 73.9f };
    CHECK(!aae_freeaim_flash(&kept, start, askew_farther, 1002, flash) && same_point(kept.flash, askew) &&
          kept.flash_at == 1001);
    CHECK(aae_freeaim_flash(&kept, start, near_by, 1003, flash) && kept.flash_at == 1003);
    // farther: not, and the one last found stays
    flash[0] = 9;
    CHECK(!aae_freeaim_flash(&kept, start, farther, 1100, flash) && flash[0] == 9 && kept.flash_at == 1003 &&
          same_point(kept.flash, near_by));
    // none found: the one last found, for 250 ms (and it doesn't become any newer by that)
    CHECK(aae_freeaim_flash(&kept, start, NULL, 1003 + Flash_Kept_Ms, flash) && same_point(flash, near_by) &&
          kept.flash_at == 1003);
    flash[0] = 9;
    CHECK(!aae_freeaim_flash(&kept, start, NULL, 1003 + Flash_Kept_Ms + 1, flash) && flash[0] == 9);
    CHECK(aae_freeaim_flash(&kept, start, NULL, 1003, flash));
    CHECK(!aae_freeaim_flash(&kept, start, NULL, 1002, flash)); // found after now: a clock gone back
    // the one last found has to be near enough too
    const float elsewhere[3] = { 400.0f, 200.0f, 300.0f };
    CHECK(!aae_freeaim_flash(&kept, elsewhere, NULL, 1010, flash));
    // what isn't a point within 1e7: not, whether found now or before
    const float not_points[][3] = { { NAN, 200, 300 }, { 100, INFINITY, 300 }, { 100, 200, -INFINITY },
                                    { 10000000.0f, 200, 300 } };
    for (size_t i = 0; i < sizeof(not_points) / sizeof(not_points[0]); i++) {
        CHECK(!aae_freeaim_flash(&kept, start, not_points[i], 1020, flash) && kept.flash_at == 1003);
        aae_freeaim_tracers_t before = kept;
        memcpy(kept.flash, not_points[i], sizeof(kept.flash));
        kept.flash_at = 1020;
        CHECK(!aae_freeaim_flash(&kept, start, NULL, 1020, flash));
        kept = before;
    }
    const float far_out[3] = { 9999999.0f, 0.0f, 0.0f }, past[3] = { 10000000.0f, 0.0f, 0.0f };
    CHECK(aae_freeaim_flash(&kept, far_out, far_out, 1030, flash) && kept.flash_at == 1030);
    CHECK(!aae_freeaim_flash(&kept, far_out, past, 1031, flash) && kept.flash_at == 1030); // 1 from the start
    // a start that isn't a number: no distance, so none
    CHECK(!aae_freeaim_flash(&kept, (float[3]){ NAN, 0, 0 }, far_out, 1040, flash) && kept.flash_at == 1030);
}

static void test_keep(void) {
    aae_freeaim_tracers_t kept;
    memset(&kept, 0, sizeof(kept));
    const float start[3] = { 1, 2, 3 }, flash[3] = { 4, 5, 6 }, target[3] = { 70, 80, 90 };
    CHECK(aae_freeaim_keep(&kept, start, flash, target, 500) && kept.count == 1 && kept.pending &&
          kept_shot(&kept.shots[0], start, flash, target, 500, false));
    // round and round: the 33rd goes where the first was
    kept.shots[0].used = true;
    for (int i = 1; i <= AAE_FREEAIM_SHOTS; i++)
        CHECK(aae_freeaim_keep(&kept, start, flash, (float[3]){ (float)i, 0, 0 }, 500 + (uint64_t)i));
    CHECK(kept.count == AAE_FREEAIM_SHOTS + 1 && kept.shots[0].target[0] == AAE_FREEAIM_SHOTS &&
          !kept.shots[0].used && kept.shots[0].made_at == 500 + AAE_FREEAIM_SHOTS && kept.shots[1].target[0] == 1);
    kept.count = UINT32_MAX;
    CHECK(aae_freeaim_keep(&kept, start, flash, target, 600) && kept.count == 0 &&
          kept_shot(&kept.shots[AAE_FREEAIM_SHOTS - 1], start, flash, target, 600, false));
    // each of the three a point within 1e7, or nothing is kept
    const float bad[3] = { 0, 10000000.0f, 0 }, worse[3] = { NAN, 0, 0 };
    kept.pending = false;
    const float *points[3] = { start, flash, target };
    for (int i = 0; i < 3; i++)
        for (int b = 0; b < 2; b++) {
            const float *with[3] = { points[0], points[1], points[2] };
            with[i] = b == 0 ? bad : worse;
            CHECK(!aae_freeaim_keep(&kept, with[0], with[1], with[2], 700) && kept.count == 0 && !kept.pending);
        }
}

// The made-up scene for finding a kept shot: a queued shot fired from the origin, its tracer from the muzzle; a kept
// shot from where the origin is, the gun 20 to the side, the point 500 ahead.
static const float scene_origin[3] = { 1000.0f, 2000.0f, 100.0f }, scene_muzzle[3] = { 1030.0f, 2000.0f, 100.0f },
                   scene_flash[3] = { 1010.0f, 2020.0f, 90.0f }, scene_target[3] = { 1500.0f, 2000.0f, 100.0f };
static aae_freeaim_tracers_t scene;
static void scene_keep(int slot, const float start[3], const float flash[3], const float target[3], uint64_t at) {
    aae_freeaim_shot_t *shot = &scene.shots[slot];
    memcpy(shot->start, start, sizeof(shot->start));
    memcpy(shot->flash, flash, sizeof(shot->flash));
    memcpy(shot->target, target, sizeof(shot->target));
    shot->made_at = at, shot->used = false;
}
static void scene_reset(void) {
    memset(&scene, 0, sizeof(scene));
    scene.count = 1;
    scene_keep(0, scene_origin, scene_flash, scene_target, 5000);
    scene.pending = true;
}
// whether the queued shot finds a kept one at this time; which one it found goes into found_slot
static float found_flash[3], found_target[3];
static int found_slot;
static bool finds(const float origin[3], const float start[3], const float muzzle[3], uint64_t now) {
    aae_freeaim_tracers_t before = scene;
    for (int i = 0; i < 3; i++)
        found_flash[i] = found_target[i] = -7.0f;
    bool found = aae_freeaim_match(&scene, origin, start, muzzle, now, found_flash, found_target);
    found_slot = -1;
    for (int i = 0; i < AAE_FREEAIM_SHOTS; i++)
        if (scene.shots[i].used != before.shots[i].used) {
            CHECK(found_slot == -1 && found && scene.shots[i].used);
            found_slot = i;
        }
    if (found)
        CHECK(found_slot >= 0 && same_point(found_flash, scene.shots[found_slot].flash) &&
              same_point(found_target, scene.shots[found_slot].target));
    else
        CHECK(found_flash[0] == -7.0f && found_target[0] == -7.0f);
    // nothing else of the kept shots changes
    for (int i = 0; i < AAE_FREEAIM_SHOTS; i++)
        before.shots[i].used = scene.shots[i].used;
    before.pending = scene.pending;
    CHECK(memcmp(&before, &scene, sizeof(scene)) == 0);
    return found;
}
static bool scene_finds(uint64_t now) {
    return finds(scene_origin, scene_origin, scene_muzzle, now);
}
// with the kept shot's start, flash or point moved by this much along x (0: start, 1: flash, 2: point)
static bool finds_with(int which, float by) {
    scene_reset();
    float *point = which == 0 ? scene.shots[0].start : which == 1 ? scene.shots[0].flash : scene.shots[0].target;
    point[0] = point[0] + by;
    return scene_finds(5000);
}

static void test_match(void) {
    scene_reset();
    // it fits: found once, and nothing is left to find
    CHECK(scene_finds(5000) && found_slot == 0 && !scene.pending);
    CHECK(!scene_finds(5000) && !scene.pending);
    // within 500 ms of being kept, not before it nor after
    scene_reset();
    CHECK(scene_finds(5000 + Shot_Kept_Ms));
    scene_reset();
    CHECK(!scene_finds(5000 + Shot_Kept_Ms + 1) && !scene.pending);
    scene_reset();
    CHECK(!scene_finds(4999) && !scene.pending);
    scene_reset();
    scene.shots[0].made_at = 0;
    CHECK(!scene_finds(0));
    scene_reset();
    scene.shots[0].made_at = UINT64_MAX;
    CHECK(!scene_finds(100) && !scene.pending);
    // the newest first, counting back from the next place in the ring
    scene_reset();
    scene.count = 7;
    scene_keep(5, scene_origin, scene_flash, (float[3]){ 1600, 2000, 100 }, 5001);
    scene_keep(6, scene_origin, scene_flash, (float[3]){ 1700, 2000, 100 }, 5002);
    scene_keep(7, scene_origin, scene_flash, (float[3]){ 1800, 2000, 100 }, 5003); // the oldest one: count is 7
    CHECK(scene_finds(5003) && found_slot == 6 && scene.pending);
    CHECK(scene_finds(5003) && found_slot == 5 && scene.pending);
    CHECK(scene_finds(5003) && found_slot == 0 && scene.pending);
    CHECK(scene_finds(5003) && found_slot == 7 && !scene.pending);
    scene_reset();
    scene.count = 0; // the ring wrapped: the newest is in the last place
    scene_keep(31, scene_origin, scene_flash, (float[3]){ 1600, 2000, 100 }, 5001);
    CHECK(scene_finds(5001) && found_slot == 31 && scene.pending);
    // still something to find: one not used yet, of the last 500 ms (whether it fits or not)
    scene_reset();
    scene_keep(9, (float[3]){ 0, 0, 0 }, scene_flash, scene_target, 5000);
    CHECK(scene_finds(5000) && found_slot == 0 && scene.pending);
    CHECK(!scene_finds(5000) && scene.pending);
    CHECK(!scene_finds(5501) && !scene.pending);
    scene_reset();
    scene_keep(AAE_FREEAIM_SHOTS - 1, (float[3]){ 0, 0, 0 }, scene_flash, scene_target, 5000);
    CHECK(scene_finds(5000) && found_slot == 0 && scene.pending);
    // the kept shot's start: 8 from the queued shot's origin at the most
    CHECK(finds_with(0, 8.0f) && !finds_with(0, 8.0005f) && finds_with(0, -8.0f));
    CHECK(!finds_with(0, INFINITY) && !finds_with(0, 20000000.0f));
    CHECK(finds_with(0, NAN)); // (one that isn't a number passes, as with the plugin; none such is ever kept)
    // the flash: a point within 1e7, 128 from the muzzle and from the origin at the most
    scene_reset();
    float *flash = scene.shots[0].flash;
    // 98 from the muzzle, 128 from the origin
    memcpy(flash, (float[3]){ 1030.0f + 98.0f, 2000.0f, 100.0f }, 3 * sizeof(float));
    CHECK(scene_finds(5000));
    scene_reset();
    memcpy(flash, (float[3]){ 1030.0f + 98.01f, 2000.0f, 100.0f }, 3 * sizeof(float));
    CHECK(!scene_finds(5000));
    scene_reset();
    // 128 from the muzzle, 98 from the origin
    memcpy(flash, (float[3]){ 1030.0f - 128.0f, 2000.0f, 100.0f }, 3 * sizeof(float));
    CHECK(scene_finds(5000));
    scene_reset();
    memcpy(flash, (float[3]){ 1030.0f - 128.01f, 2000.0f, 100.0f }, 3 * sizeof(float));
    CHECK(!scene_finds(5000));
    CHECK(!finds_with(1, NAN) && !finds_with(1, INFINITY));
    scene_reset();
    memcpy(flash, (float[3]){ 10000000.0f, 2000.0f, 100.0f }, 3 * sizeof(float));
    memcpy(scene.shots[0].start, flash, 3 * sizeof(float));
    memcpy(scene.shots[0].target, (float[3]){ 9999499.0f, 2000.0f, 100.0f }, 3 * sizeof(float));
    CHECK(!finds((float[3]){ 9999999.0f, 2000, 100 }, (float[3]){ 9999999.0f, 2000, 100 },
                 (float[3]){ 9999999.0f, 2000, 100 }, 5000));
    scene_reset();
    memcpy(flash, (float[3]){ 9999999.0f, 2000.0f, 100.0f }, 3 * sizeof(float));
    memcpy(scene.shots[0].start, flash, 3 * sizeof(float));
    memcpy(scene.shots[0].target, (float[3]){ 9999499.0f, 2000.0f, 100.0f }, 3 * sizeof(float));
    CHECK(finds((float[3]){ 9999999.0f, 2000, 100 }, (float[3]){ 9999999.0f, 2000, 100 },
                (float[3]){ 9999999.0f, 2000, 100 }, 5000));
    // the point: within 1e7, at least 1 and at most 131072 from the flash, in doubles
    scene_reset();
    float *target = scene.shots[0].target;
    memcpy(target, (float[3]){ 1011.0f, 2020.0f, 90.0f }, 3 * sizeof(float));
    CHECK(scene_finds(5000));
    scene_reset();
    memcpy(target, (float[3]){ 1010.9999f, 2020.0f, 90.0f }, 3 * sizeof(float));
    CHECK(!scene_finds(5000));
    scene_reset();
    memcpy(target, (float[3]){ 1010.0f + 131072.0f, 2020.0f, 90.0f }, 3 * sizeof(float));
    CHECK(scene_finds(5000));
    scene_reset();
    memcpy(target, (float[3]){ 1010.0f + 131072.0f + 0.015625f, 2020.0f, 90.0f }, 3 * sizeof(float));
    CHECK(!scene_finds(5000));
    CHECK(!finds_with(2, NAN) && !finds_with(2, -INFINITY) && !finds_with(2, 10000000.0f));
    // (in doubles: 131072 along and 1 across is 2^34 + 1, which floats would round to 2^34)
    scene_reset();
    memcpy(target, (float[3]){ 1010.0f + 131072.0f, 2021.0f, 90.0f }, 3 * sizeof(float));
    CHECK(!scene_finds(5000));
    scene_reset();
    memcpy(target, (float[3]){ -9999999.0f, 2020.0f, 90.0f }, 3 * sizeof(float));
    CHECK(!scene_finds(5000)); // within 1e7, but farther than 131072
    scene_reset();
    memcpy(flash, (float[3]){ 9999999.0f, 2000.0f, 100.0f }, 3 * sizeof(float));
    memcpy(scene.shots[0].start, flash, 3 * sizeof(float));
    memcpy(target, (float[3]){ 10000000.0f, 2000.0f, 100.0f }, 3 * sizeof(float)); // 1 from the flash, past 1e7
    CHECK(!finds((float[3]){ 9999999.0f, 2000, 100 }, (float[3]){ 9999999.0f, 2000, 100 },
                 (float[3]){ 9999999.0f, 2000, 100 }, 5000));
    // the muzzle: 128 from the origin at the most (the flash 128 from both then)
    scene_reset();
    memcpy(flash, (float[3]){ 1064.0f, 2000.0f, 100.0f }, 3 * sizeof(float));
    CHECK(finds(scene_origin, scene_origin, (float[3]){ 1128.0f, 2000.0f, 100.0f }, 5000));
    scene_reset();
    memcpy(flash, (float[3]){ 1064.0f, 2000.0f, 100.0f }, 3 * sizeof(float));
    CHECK(!finds(scene_origin, scene_origin, (float[3]){ 1128.01f, 2000.0f, 100.0f }, 5000));
    // a queued shot whose origin, start or muzzle isn't a point within 1e7: nothing, and nothing noted anew
    const float not_points[][3] = { { NAN, 0, 0 }, { 0, INFINITY, 0 }, { 0, 0, 10000000.0f }, { -10000000.0f, 0, 0 } };
    for (size_t i = 0; i < sizeof(not_points) / sizeof(not_points[0]); i++) {
        scene_reset();
        scene.pending = false;
        CHECK(!finds(not_points[i], scene_origin, scene_muzzle, 5000) && !scene.pending);
        CHECK(!finds(scene_origin, not_points[i], scene_muzzle, 5000) && !scene.pending);
        CHECK(!finds(scene_origin, scene_origin, not_points[i], 5000) && !scene.pending);
        CHECK(finds(scene_origin, scene_origin, scene_muzzle, 5000));
    }
    // where the queued shot starts doesn't matter otherwise
    scene_reset();
    CHECK(finds(scene_origin, (float[3]){ -5000, 7000, 3 }, scene_muzzle, 5000));
}

// the same cases the plugin's own functions were given (see freeaim_endpos_cases.h): its hook for the client's copy of
// a shot (FUN_18001eff0) and its function that finds the kept shot a queued one is (FUN_180023bd0); and what that gave
#define Plugin_Endpos_Lines 49538
#define Plugin_Endpos_Checksum 0x3f12bdf9f0ec0693ULL
#include "freeaim_endpos_cases.h"
static void endpos_cases_line(const char *line) {
    cases_line(line);
}
#include "freeaim_endpos_side.h"
static void test_match_like_the_plugin(void) {
    set_mod(true);
    cases_out.checksum = 0xcbf29ce484222325ULL, cases_out.lines = 0;
    endpos_cases_run();
    CHECK(cases_out.lines == Plugin_Endpos_Lines);
    CHECK(cases_out.checksum == Plugin_Endpos_Checksum);
    if (cases_out.checksum != Plugin_Endpos_Checksum)
        printf("  the cases came out as %#018llx: wip/agents/endpos2/compare/compare.sh shows where they differ from "
               "the plugin's\n", (unsigned long long)cases_out.checksum);
}

// the made-up game's models: the shooting player's, and where its tag_flash is
static uint8_t player_model[0x10];
static uint32_t game_tag_flash; // the game's own "tag_flash", 0 until the client's game starts
static struct {
    int asked, number, local_client;
    bool there, has_tag; // the entity has a model; the model has the tag
    float position[3];   // where it is
    const uint8_t *cent;
    void *object;
    uint32_t tag;
} model;
static void *game_dobj(int number, int local_client) {
    model.asked++, model.number = number, model.local_client = local_client;
    return model.there ? player_model : NULL;
}
static int game_tag_position(const uint8_t *cent, void *object, uint32_t tag, float position[3]) {
    model.cent = cent, model.object = object, model.tag = tag;
    if (model.has_tag)
        memcpy(position, model.position, sizeof(model.position));
    return model.has_tag;
}
static void set_model(bool there, bool has_tag, const float position[3]) {
    model.there = there, model.has_tag = has_tag;
    memcpy(model.position, position, sizeof(model.position));
}
// the made-up game's queued shots
static uint8_t game_queue[4 * Queue_Entry_Size];
static uint8_t *queue_entry(int index) {
    return game_queue + (size_t)index * Queue_Entry_Size;
}
// a queued shot of this local client and player, fired from origin, its tracer from muzzle
static void queue_shot(int index, int32_t local_client, uint32_t number, const float origin[3], const float muzzle[3]) {
    uint8_t *entry = queue_entry(index);
    memset(entry, 0x5c, Queue_Entry_Size);
    memcpy(entry, &local_client, sizeof(local_client));
    memcpy(entry + Queued_Shot + Shot_Origin, origin, 3 * sizeof(float));
    memcpy(entry + Queued_Shot + Shot_Start, origin, 3 * sizeof(float));
    memcpy(entry + Queued_Number, &number, sizeof(number));
    memcpy(entry + Queued_Muzzle, muzzle, 3 * sizeof(float));
}
// The game comes to the head of its loop over the queued shots, at this one: whether its tracer was moved. Nothing
// but where the shot was fired from, where it starts and where its tracer starts is written, and the registers stay as
// they were. What those three are afterwards goes into drawn_*.
static float drawn_origin[3], drawn_start[3], drawn_muzzle[3];
static bool draws(int index) {
    uint8_t before[Queue_Entry_Size], *entry = queue_entry(index);
    memcpy(before, entry, sizeof(before));
    detour_registers_t registers;
    memset(&registers, 0x5a, sizeof(registers));
    registers.r13 = (uint64_t)game_queue;
    registers.rsi = (uint64_t)index;
    detour_registers_t as_they_were = registers;
    handler_Queued_Effects(&registers);
    CHECK(memcmp(&as_they_were, &registers, sizeof(registers)) == 0);
    for (int i = 0; i < Queue_Entry_Size; i++) {
        bool may = (i >= Queued_Shot + Shot_Origin && i < Queued_Shot + Shot_Origin + 12) ||
                   (i >= Queued_Shot + Shot_Start && i < Queued_Shot + Shot_Start + 12) ||
                   (i >= Queued_Muzzle && i < Queued_Muzzle + 12);
        if (!may && entry[i] != before[i]) {
            CHECK(!"something else of the queued shot was written");
            break;
        }
    }
    memcpy(drawn_origin, entry + Queued_Shot + Shot_Origin, sizeof(drawn_origin));
    memcpy(drawn_start, entry + Queued_Shot + Shot_Start, sizeof(drawn_start));
    memcpy(drawn_muzzle, entry + Queued_Muzzle, sizeof(drawn_muzzle));
    bool moved = memcmp(before + Queued_Muzzle, drawn_muzzle, sizeof(drawn_muzzle)) != 0;
    if (!moved)
        CHECK(memcmp(entry, before, sizeof(before)) == 0);
    return moved;
}

static void test_tracers(void) {
    const float there[3] = { -139.0f, -1108.0f, 140.0f };
    const float gun[3] = { -229.0f, -1188.0f, 120.0f };      // 30 from the eye
    const float in_front[3] = { -209.0f, -1208.0f, 140.0f }; // where the game starts the tracer: 30 in front of the eye
    set_mod(true);
    memset(clients, 0, sizeof(clients));
    memset(tracers, 0, sizeof(tracers));
    set_view(0, true, 0, 0);
    set_view(1, true, 0, 1);
    orig_Bullet_Endpos = game_endpos;
    memcpy(eye_directions, straight, sizeof(straight));
    set_text("aae_freeaim_target", "-139 -1108 140");
    set_text("aae_freeaim_target_1", "-239 -1208 500");
    set_shooting(0, true);
    game_tag_flash = 0x2a5;
    set_model(true, true, gun);
    both_seen();
    test_now += Third_Person_For_Ms;
    both_seen();
    // a shot, turned and kept with where the gun's flash is on the player's model
    model.asked = 0;
    CHECK(client_fires(1, fire_frame) && turned_at(there));
    CHECK(model.asked == 1 && model.number == 0 && model.local_client == 0 && model.cent == shooting_cent &&
          model.object == player_model && model.tag == 0x2a5);
    CHECK(tracers[0].count == 1 && tracers[0].pending && tracers[1].count == 0 &&
          kept_shot(&tracers[0].shots[0], eye, gun, there, test_now, false));
    // when its effects are drawn, its tracer starts at the gun (and the shot is fired from the gun towards the point,
    // as the plugin writes it)
    queue_shot(1, 0, 0, eye, in_front);
    CHECK(draws(1) && same_point(drawn_muzzle, gun) && same_point(drawn_origin, gun) && same_point(drawn_start, there));
    CHECK(tracers[0].shots[0].used && !tracers[0].pending);
    // where the queued shot starts is only looked at for being a point within 1e7
    CHECK(client_fires(1, fire_frame));
    queue_shot(1, 0, 0, eye, in_front);
    memcpy(queue_entry(1) + Queued_Shot + Shot_Start, (float[3]){ 9999999.0f, 0.0f, 0.0f }, 3 * sizeof(float));
    CHECK(draws(1) && same_point(drawn_start, there));
    CHECK(client_fires(1, fire_frame));
    queue_shot(1, 0, 0, eye, in_front);
    memcpy(queue_entry(1) + Queued_Shot + Shot_Start, (float[3]){ 10000000.0f, 0.0f, 0.0f }, 3 * sizeof(float));
    CHECK(!draws(1) && tracers[0].pending);
    queue_shot(1, 0, 0, eye, in_front);
    CHECK(draws(1));
    // that kept shot is used: another queued one finds none, nor does one while nothing is pending
    queue_shot(2, 0, 0, eye, in_front);
    CHECK(!draws(2));
    tracers[0].shots[0].used = false;
    CHECK(!draws(2));
    tracers[0].pending = true;
    CHECK(draws(2) && same_point(drawn_muzzle, gun));
    // from the first call as well (whose shots aren't queued: what is kept for them is found by nothing)
    uint32_t kept_before = tracers[0].count;
    CHECK(client_fires(0, fire_frame) && tracers[0].count == kept_before + 1 && tracers[0].pending);
    // only a queued shot of the client that kept it, and only of its player
    queue_shot(0, 1, 0, eye, in_front);
    CHECK(!draws(0));
    queue_shot(0, 2, 0, eye, in_front);
    CHECK(!draws(0));
    queue_shot(0, -1, 0, eye, in_front);
    CHECK(!draws(0));
    queue_shot(0, 0, 1, eye, in_front);
    CHECK(!draws(0));
    queue_shot(0, 0, Max_Players, eye, in_front);
    set_view(0, true, 0, Max_Players);
    CHECK(!draws(0));
    set_view(0, true, 0, 0);
    both_seen();
    test_now += Third_Person_For_Ms;
    both_seen();
    queue_shot(0, 0, 0, eye, in_front);
    CHECK(draws(0));
    // the queued shot's player has an entry in the table of points other machines sent: not a local client's
    CHECK(client_fires(1, fire_frame) && tracers[0].pending);
    set_client_state(0, 5);
    CHECK(!reaches_the_game(server_client(0), "aae -70 -554 70", 0));
    queue_shot(0, 0, 0, eye, in_front);
    CHECK(!draws(0));
    CHECK(!reaches_the_game(server_client(0), "aae off", 0));
    CHECK(draws(0));
    // free aim off for the client, or the mod not there: nothing
    CHECK(client_fires(1, fire_frame));
    set_dvar("aae_freeaim_enabled", true, 0);
    queue_shot(0, 0, 0, eye, in_front);
    CHECK(!draws(0) && tracers[0].pending);
    set_dvar("aae_freeaim_enabled", false, 1);
    set_mod(false);
    CHECK(!draws(0));
    set_mod(true);
    CHECK(draws(0));
    // the view of the client in first person: the plugin forgets that a queued shot may find a kept one there
    memset(tracers, 0, sizeof(tracers));
    CHECK(client_fires(1, fire_frame) && tracers[0].pending);
    set_view(0, false, 0, 0);
    CHECK(!crosshair_asks(0, false) && !tracers[0].pending);
    set_view(0, true, 0, 0);
    both_seen();
    test_now += Third_Person_For_Ms;
    both_seen();
    queue_shot(0, 0, 0, eye, in_front);
    CHECK(!draws(0)); // (the shot kept is still there, and fits: nothing looks for it)
    // with a newer one kept, both are found, the newer one first
    CHECK(client_fires(1, fire_frame) && tracers[0].pending && draws(0) && tracers[0].shots[1].used &&
          !tracers[0].shots[0].used && tracers[0].pending);
    queue_shot(0, 0, 0, eye, in_front);
    CHECK(draws(0) && tracers[0].shots[0].used && !tracers[0].pending);
    queue_shot(0, 0, 0, eye, in_front);
    CHECK(!draws(0));
    // and for a local client the crosshair code was never at work for, when the plugin asks about it: for a queued
    // shot of it,
    tracers[1].pending = true;
    clients[1].cg = NULL;
    queue_shot(0, 1, 1, eye, in_front);
    CHECK(!draws(0) && !tracers[1].pending);
    // and for a shot of the other client
    tracers[1].pending = true;
    set_view(0, false, 0, 0);
    CHECK(!client_fires(1, fire_frame) && !tracers[1].pending);
    set_view(0, true, 0, 0);
    both_seen();
    test_now += Third_Person_For_Ms;
    both_seen();
    // the second local client keeps its own
    set_shooting(1, true);
    memset(tracers, 0, sizeof(tracers));
    CHECK(client_fires(1, fire_frame) && tracers[1].count == 1 && tracers[0].count == 0 && model.local_client == 1 &&
          model.number == 1 && tracers[1].flash_at == test_now && tracers[0].flash_at == 0);
    queue_shot(3, 1, 1, eye, in_front);
    CHECK(draws(3) && same_point(drawn_start, (float[3]){ -239.0f, -1208.0f, 500.0f }));
    set_shooting(0, true);
    // Where the gun's flash was found last stands in for 250 ms when it can't be found: the game's name for the tag not
    // made yet, no model, no such tag on it, or the tag somewhere that isn't a point within 1e7.
    memset(tracers, 0, sizeof(tracers));
    const float *ways[] = { NULL, NULL, NULL, (float[3]){ 10000000.0f, 0, 0 }, (float[3]){ NAN, 0, 0 } };
    for (int way = 0; way < 5; way++) {
        game_tag_flash = 0x2a5;
        set_model(true, true, gun);
        both_seen();
        CHECK(client_fires(1, fire_frame) && tracers[0].flash_at == test_now);
        uint64_t found_at = test_now;
        game_tag_flash = way == 0 ? 0 : 0x2a5;
        set_model(way != 1, way != 2, ways[way] != NULL ? ways[way] : gun);
        test_now = found_at + Flash_Kept_Ms;
        both_seen();
        uint32_t count = tracers[0].count;
        CHECK(client_fires(1, fire_frame) && tracers[0].count == count + 1 && tracers[0].flash_at == found_at &&
              kept_shot(&tracers[0].shots[count % AAE_FREEAIM_SHOTS], eye, gun, there, test_now, false));
        test_now = found_at + Flash_Kept_Ms + 1;
        both_seen();
        CHECK(client_fires(1, fire_frame) && tracers[0].count == count + 1);
    }
    set_model(true, true, gun);
    game_tag_flash = 0x2a5;
    both_seen();
    // a flash farther than 128 from the eye, or a point under the crosshair beyond 1e7: turned, nothing kept
    memset(tracers, 0, sizeof(tracers));
    set_model(true, true, (float[3]){ -239.0f, -1208.0f + 128.0f, 140.0f });
    CHECK(client_fires(1, fire_frame) && tracers[0].count == 1);
    set_model(true, true, (float[3]){ -239.0f, -1208.0f + 128.01f, 140.0f });
    CHECK(client_fires(1, fire_frame) && tracers[0].count == 1 && tracers[0].flash[1] == -1208.0f + 128.0f);
    set_model(true, true, gun);
    set_text("aae_freeaim_target", "9999999 -1108 140");
    CHECK(client_fires(1, fire_frame) && tracers[0].count == 2);
    set_text("aae_freeaim_target", "10000000 -1108 140");
    CHECK(client_fires(1, fire_frame) && tracers[0].count == 2);
    set_text("aae_freeaim_target", "-139 -1108 140");
    // a shot that isn't turned keeps nothing, but where the flash was found is noted all the same
    memset(tracers, 0, sizeof(tracers));
    eye_directions[0] = 1.2f;
    CHECK(!client_fires(1, fire_frame) && tracers[0].count == 0 && tracers[0].flash_at == test_now);
    memcpy(eye_directions, straight, sizeof(straight));
    // Each question whether free aim is on reads the clock anew: the fourth one, before the gun's flash is looked for,
    // and then where it was found and when the shot is kept. When free aim runs out right before the fourth, the shot
    // is turned and nothing is kept.
    memset(tracers, 0, sizeof(tracers));
    uint64_t seen = test_now;
    both_seen();
    now_ms = ticking_clock;
    test_now = seen + Seen_For_Ms - 3;
    CHECK(client_fires(1, fire_frame) && tracers[0].count == 1 &&
          tracers[0].shots[0].made_at == seen + Seen_For_Ms + 2 && tracers[0].flash_at == seen + Seen_For_Ms + 1 &&
          test_now == seen + Seen_For_Ms + 3);
    test_now = seen + Seen_For_Ms - 2;
    model.asked = 0;
    CHECK(client_fires(1, fire_frame) && tracers[0].count == 1 && model.asked == 0 &&
          test_now == seen + Seen_For_Ms + 2);
    now_ms = test_clock;
    memset(tracers, 0, sizeof(tracers));
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
        CHECK(hooks == 8 && was_hooked(ADDR_CG_DrawCrosshairTest, (void *)fake_CG_DrawCrosshairTest,
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
    CHECK(hooks == 11 && was_hooked(ADDR_CG_DrawCrosshairTest, (void *)fake_CG_DrawCrosshairTest,
                                   (void **)&orig_CG_DrawCrosshairTest));
    CHECK(was_hooked(ADDR_Bullet_Fire, (void *)fake_Bullet_Fire, (void **)&orig_Bullet_Fire));
    CHECK(was_hooked(ADDR_Grenade_Spawn, (void *)fake_Grenade_Spawn, (void **)&orig_Grenade_Spawn) &&
          was_hooked(ADDR_Rocket_Spawn, (void *)fake_Rocket_Spawn, (void **)&orig_Rocket_Spawn) &&
          was_hooked(ADDR_Projectile_Spawn, (void *)fake_Projectile_Spawn, (void **)&orig_Projectile_Spawn));
    CHECK(was_hooked(ADDR_CG_DrawCrosshair_Tests, (void *)handler_Crosshair_Tests, &orig_Crosshair_Tests) &&
          other_Crosshair_Tests == (void *)(base + ADDR_CG_DrawCrosshair_Draws));
    CHECK(was_hooked(ADDR_CG_DrawCrosshair_Gap, (void *)handler_Crosshair_Gap, &orig_Crosshair_Gap));
    // the client's own copy of a shot, and where the two calls of it that count return to
    CHECK(was_hooked(ADDR_Bullet_Endpos, (void *)fake_Bullet_Endpos, (void **)&orig_Bullet_Endpos) &&
          endpos_asked_from[0] == (const void *)(base + ADDR_Bullet_Endpos_Return1) &&
          endpos_asked_from[1] == (const void *)(base + ADDR_Bullet_Endpos_Return2));
    CHECK(Cent_Number == 0x3f8 && Fire_Frame_Cent == -0x58 && Point_Bound == 1e7f);
    // the tracer from the gun: the head of the loop over the queued shots, and the model's tag
    CHECK(was_hooked(ADDR_Bullet_QueuedShots_Loop, (void *)handler_Queued_Effects, &orig_Queued_Effects) &&
          Com_GetClientDObj == (void *)(base + ADDR_Com_GetClientDObj) &&
          CG_DObjGetWorldTagPos == (void *)(base + ADDR_CG_DObjGetWorldTagPos) &&
          tag_flash_name == (const uint32_t *)(base + ADDR_scr_tag_flash));
    CHECK(Queue_Entry_Size == 0x8b0 && Queued_Shot == 4 && Shot_Origin == 0x10 && Shot_Start == 0x1c &&
          Queued_Number == 0x58 && Queued_Muzzle == 0x64);
    CHECK(Flash_Kept_Ms == 250 && Shot_Kept_Ms == 500 && Flash_Farthest == 16384.0f && Start_Farthest == 64.0f &&
          Target_Nearest == 1.0 && Target_Farthest == 17179869184.0);
    // the places in the game's own structures, as they were read from its code
    CHECK(Cg_Size == 0x342a80 && Cg_ThirdPerson == 0x11a8c9 && Cg_PlayerState == 0x11a8d0 && Ps_Kind == 4 &&
          Ps_Sights == 0x2fc && Frame_Sights == -0x50 && Weapon_LeastGap == 0x6f0 && Dvar_Type == 0x1c &&
          Dvar_Type_String == 8 && Entity_Client == 0x250 && Ps_ThirdPerson == 0xbc && Shot_Muzzle == 0x24 &&
          Max_Players == 64 && Entity_Origin == 0x1c && Entity_Velocity == 0x28 && Entity_Angles == 0x23c &&
          Client_Size == 0xe5270);
    CHECK(Dvar_FindByHash == (void *)(base + ADDR_Dvar_FindByHash) &&
          Dvar_GetString == (void *)(base + ADDR_Dvar_GetString) && cg_clients == (void *)(base + ADDR_cgArray) &&
          crosshair_asks_from == (const void *)(base + ADDR_CG_DrawCrosshair_TestReturn) && now_ms == uptime_ms);
    // a pre-patched executable without the stub at the queued shots: everything but that
    stub_missing_for = ADDR_Bullet_QueuedShots_Loop;
    hooks = 0;
    aae_freeaim_init(base, 1);
    CHECK(hooks == 10 && was_hooked(ADDR_Bullet_Endpos, (void *)fake_Bullet_Endpos, (void **)&orig_Bullet_Endpos) &&
          !was_hooked(ADDR_Bullet_QueuedShots_Loop, (void *)handler_Queued_Effects, &orig_Queued_Effects));
    stub_missing_for = -1;
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
    Com_GetClientDObj = game_dobj;
    CG_DObjGetWorldTagPos = game_tag_position;
    tag_flash_name = &game_tag_flash;
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
    test_client_shots();
    test_flash();
    test_keep();
    test_match();
    test_match_like_the_plugin();
    test_tracers();

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
