// aae_arc.c: T7Overcharged's arc test, the plugin's checks and sums for the grenade's line, and the hooks against a
// made-up game.
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/aae_plugin.c"
#include "../source/aae_arc.c"
#include "arc_bend_cases.h"

static uint32_t bits_of(float value) {
    uint32_t b;
    memcpy(&b, &value, sizeof(b));
    return b;
}
static float from_bits(uint32_t b) {
    float value;
    memcpy(&value, &b, sizeof(value));
    return value;
}
static void put_int(uint8_t *place, int32_t value) {
    memcpy(place, &value, sizeof(value));
}
static void put_float(uint8_t *place, float value) {
    memcpy(place, &value, sizeof(value));
}
static void put_pointer(uint8_t *place, const void *value) {
    memcpy(place, &value, sizeof(value));
}

// ---- what the code asks of the rest of BO3MacFix
static char mod_folder[4096];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
static bool have_stubs = true;
static int stub_missing_for = -1;
bool prepatch_has_hook(uint32_t offset) {
    return have_stubs && (int)offset != stub_missing_for;
}

// ---- the made-up game's functions behind the hooks
static struct {
    int calls;
    bool answer;
    int local_client;
    uint64_t weapon;
} game_test;
static bool game_should_predict(int local_client, uint64_t weapon) {
    game_test.calls++;
    game_test.local_client = local_client, game_test.weapon = weapon;
    return game_test.answer;
}
static struct {
    int calls;
    uint64_t arguments[9];
} game_mesh;
static void game_add_mesh(const uint8_t *material, uint64_t second, uint64_t third, uint64_t fourth, uint64_t fifth,
                          uint64_t sixth, uint64_t seventh, uint64_t eighth, uint64_t ninth) {
    const uint64_t got[9] = { (uint64_t)material, second, third, fourth, fifth, sixth, seventh, eighth, ninth };
    game_mesh.calls++;
    memcpy(game_mesh.arguments, got, sizeof(got));
}
static struct {
    int calls;
    uint64_t arguments[6];
} game_skeleton;
static uint64_t game_calc_skeleton(const uint8_t *object, uint64_t second, uint64_t third, uint64_t fourth,
                                   uint64_t fifth, uint64_t sixth) {
    const uint64_t got[6] = { (uint64_t)object, second, third, fourth, fifth, sixth };
    game_skeleton.calls++;
    memcpy(game_skeleton.arguments, got, sizeof(got));
    return 0x5ca1ab1e;
}
static struct {
    int calls, local_client;
    uint64_t arguments[5];
} game_segment;
static void game_add_segment(int local_client, uint64_t second, uint64_t third, uint64_t fourth, uint64_t fifth,
                             uint64_t sixth) {
    const uint64_t got[5] = { second, third, fourth, fifth, sixth };
    game_segment.calls++;
    game_segment.local_client = local_client;
    memcpy(game_segment.arguments, got, sizeof(got));
}

static struct {
    void *address, *replacement, **original;
} hooked[16];
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 16)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    if (out_origin_func == (void **)&orig_CG_ShouldPredictArc)
        *out_origin_func = (void *)game_should_predict;
    else if (out_origin_func == (void **)&orig_R_AddCodeMeshDrawSurf)
        *out_origin_func = (void *)game_add_mesh;
    else if (out_origin_func == (void **)&orig_DObjCalcSkel)
        *out_origin_func = (void *)game_calc_skeleton;
    else if (out_origin_func == (void **)&orig_CG_ArcPrediction_AddSegment)
        *out_origin_func = (void *)game_add_segment;
    return 0;
}
static bool was_hooked(uint64_t base, uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 16; i++)
        if (hooked[i].address == (void *)(base + place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
}

// ---- the made-up game: its memory, dvars, weapons, strings and the view model

// the game's image, as far as the code reads it by address (a reservation: only what is written is there)
#define Image_Size 0x16000000
static uint8_t *image;
static uint8_t *cgs; // two local clients' cg_t
static uint8_t *view_states; // four local clients' view weapon states

static struct {
    uint8_t game_side[0x40];
    const char *name;
    bool there;
    int kind;
    char text[200];
} dvars[] = { { .name = "force_arc" }, { .name = "arc_requires_tmode" }, { .name = "aae_arc_weapondef" },
              { .name = "aae_arc_tag" } };
static void *game_find_dvar(uint32_t hash) {
    for (size_t i = 0; i < sizeof(dvars) / sizeof(dvars[0]); i++)
        if (dvars[i].there && gsc_hash(dvars[i].name) == hash)
            return &dvars[i];
    return NULL;
}
static int wrong_asks; // the text of a dvar without text was asked for
static const char *game_dvar_string(void *dvar) {
    typeof(dvars[0]) *d = dvar;
    if (d->kind != Dvar_Type_String)
        wrong_asks++;
    return d->text;
}
static int game_dvar_int(void *dvar) {
    typeof(dvars[0]) *d = dvar;
    return d->kind == 1 ? d->text[0] == '1' : atoi(d->text);
}
// a dvar of this kind (8 with text) with this text, or none
static void set_dvar_kind(const char *name, bool there, int kind, const char *text) {
    for (size_t i = 0; i < sizeof(dvars) / sizeof(dvars[0]); i++) {
        if (strcmp(dvars[i].name, name) == 0) {
            dvars[i].there = there, dvars[i].kind = kind;
            snprintf(dvars[i].text, sizeof(dvars[i].text), "%s", text);
            memcpy(dvars[i].game_side + Dvar_Type, &kind, sizeof(kind));
        }
    }
}
static void set_text(const char *name, const char *text) {
    set_dvar_kind(name, true, Dvar_Type_String, text);
}
static void no_dvar(const char *name) {
    set_dvar_kind(name, false, Dvar_Type_String, "");
}

// weapon definitions, as big as the code reads them
#define Def_Size 0x1100
static uint8_t frag[Def_Size], bow[Def_Size], bow4[Def_Size], other[Def_Size];
static struct {
    uint64_t weapon;
    const uint8_t *definition;
} weapons[] = { { 0x12b, frag }, { 0x37, bow }, { 0x1ff, other } };
static int definition_asks;
static const uint8_t *game_weapon_def(uint64_t weapon) {
    definition_asks++;
    for (size_t i = 0; i < sizeof(weapons) / sizeof(weapons[0]); i++)
        if ((weapons[i].weapon & 0x1ff) == (weapon & 0x1ff))
            return weapons[i].definition;
    return NULL;
}
static struct {
    int calls, answer;
    const uint8_t *player_state;
    uint64_t weapon;
} ammo;
static int game_weapon_ammo(const uint8_t *player_state, uint64_t weapon) {
    ammo.calls++;
    ammo.player_state = player_state, ammo.weapon = weapon;
    return ammo.answer;
}
// the weapons the game has by name: their index in bg_weaponVariantDefs
static struct {
    const char *name;
    uint32_t index;
} names[] = { { "elemental_bow4_zm", 0x41 }, { "frag_grenade", 0x12b }, { "nothing_zm", 0x42 }, { "big_zm", 0x200 },
              { "zero_zm", 0 }, { "bow_zm", 0x37 } };
static char last_name_asked[200];
static uint32_t game_weapon_index(const char *name, void *hashes, uint8_t *sorted) {
    snprintf(last_name_asked, sizeof(last_name_asked), "%s", name);
    CHECK(hashes == (void *)(image + ADDR_bg_weaponNameHashes) &&
          sorted == (uint8_t *)(image + ADDR_bg_weaponNameHashesSorted));
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(names[i].name, name) == 0)
            return names[i].index;
    return 0;
}
static uint8_t variants[3][0x20];
static void set_variant(uint32_t index, uint8_t *variant, const uint8_t *definition) {
    if (variant != NULL)
        put_pointer(variant + WeaponVariantDef_WeaponDef, definition);
    put_pointer(image + ADDR_bg_weaponVariantDefs + (size_t)index * 8, variant);
}

// the script strings: an id for each text
static const char *strings[8] = { "", "tag_weapon_right", "tag_flash", "tag_nothing" };
static int string_count = 4, string_asks;
static bool strings_down;
static uint32_t game_string(const char *text, uint32_t user, uint32_t length, int type) {
    string_asks++;
    CHECK(user == 0 && type == 3 && length == strlen(text) + 1);
    if (strings_down)
        return 0;
    for (int i = 1; i < string_count; i++)
        if (strcmp(strings[i], text) == 0)
            return (uint32_t)i * 100;
    return string_count < 8 ? (uint32_t)(strings[string_count] = strdup(text), string_count++) * 100 : 0;
}

// the view model: a DObj with its tree, bone count, computed bits and skeleton; its bones by name
#define Bones 40
static uint8_t view_model[0x140], other_model[0x140];
static uint8_t skeleton[Bones * Skeleton_Bone_Size];
static int tree_a, tree_b;
static struct {
    uint32_t name;
    int16_t index;
} bones[] = { { 100, 7 }, { 200, 31 }, { 300, 33 } };
static int bone_asks;
static int game_bone_index(const uint8_t *object, uint32_t name, uint16_t *index, int model) {
    bone_asks++;
    CHECK(*index == 0xfffe && model == 0xffff && (object == view_model || object == other_model));
    for (size_t i = 0; i < sizeof(bones) / sizeof(bones[0]); i++) {
        if (bones[i].name == name) {
            *index = (uint16_t)bones[i].index;
            return 1;
        }
    }
    // a name it says no for, and leaves an index behind all the same
    if (name == 400)
        *index = 7;
    return 0;
}
static void set_bone(int index, float x, float y, float z, bool computed) {
    float *translation = (float *)(skeleton + (size_t)index * Skeleton_Bone_Size + Skeleton_Translation);
    translation[0] = x, translation[1] = y, translation[2] = z;
    uint32_t word;
    memcpy(&word, view_model + Object_Computed + (size_t)(index >> 5) * 4, sizeof(word));
    word = computed ? word | (0x80000000u >> (index & 31)) : word & ~(0x80000000u >> (index & 31));
    memcpy(view_model + Object_Computed + (size_t)(index >> 5) * 4, &word, sizeof(word));
}
static void set_model(uint8_t *model, const void *tree, uint16_t count, const uint8_t *bones_skeleton) {
    put_pointer(model + Object_Tree, tree);
    memcpy(model + Object_Bone_Count, &count, sizeof(count));
    put_pointer(model + Object_Skeleton, bones_skeleton);
}
static void set_view_model(int which, const uint8_t *model) {
    put_pointer(view_states + (size_t)which * View_Size, model);
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

// ---- T7Overcharged's test

static uint8_t player_state[0x400];
static void grenade_in_hand(void) {
    memset(player_state, 0, sizeof(player_state));
    put_int(player_state + Ps_Weapon_State, 0x26);
    uint64_t held = 0x12b;
    memcpy(player_state + Ps_Ammo_Weapon, &held, sizeof(held));
    memset(frag, 0, sizeof(frag));
    put_int(frag + Def_Type, Weapon_Type_Grenade);
    ammo.calls = 0, ammo.answer = 1;
    definition_asks = 0;
}
static bool predicts(int force, int requires) {
    return aae_arc_predicts(force, requires, player_state, 0x12b, game_weapon_def, game_weapon_ammo);
}

static void test_t7o_rule(void) {
    // a frag being held: an arc unless arc_requires_tmode says no
    grenade_in_hand();
    CHECK(predicts(Aae_Arc_No_Dvar, Aae_Arc_No_Dvar) && predicts(0, 1) && !predicts(0, 0) && predicts(0, 2));
    CHECK(definition_asks == 4 && ammo.calls == 0);
    // force_arc: yes for anything, nothing else asked
    definition_asks = 0;
    CHECK(aae_arc_predicts(1, 0, NULL, 0, game_weapon_def, game_weapon_ammo) && definition_asks == 0);
    CHECK(aae_arc_predicts(5, 0, NULL, 0, game_weapon_def, game_weapon_ammo));
    CHECK(!aae_arc_predicts(0, 1, NULL, 0x12b, game_weapon_def, game_weapon_ammo));
    // the player state's flag
    player_state[Ps_Flags] = No_Arc_Flag;
    CHECK(!predicts(0, 1) && predicts(1, 1));
    player_state[Ps_Flags] = (uint8_t)~No_Arc_Flag;
    CHECK(predicts(0, 1));
    player_state[Ps_Flags] = 0;
    // no weapon, or one without an index: the definition isn't asked for
    definition_asks = 0;
    CHECK(!aae_arc_predicts(0, 1, player_state, 0, game_weapon_def, game_weapon_ammo));
    CHECK(!aae_arc_predicts(0, 1, player_state, 0x200, game_weapon_def, game_weapon_ammo) && definition_asks == 0);
    CHECK(aae_arc_predicts(0, 1, player_state, 0x52b, game_weapon_def, game_weapon_ammo));
    CHECK(!aae_arc_predicts(0, 1, player_state, 0x150, game_weapon_def, game_weapon_ammo)); // no definition
    // a definition that never has an arc
    put_int(frag + Def_Never_Arc, 1);
    CHECK(!predicts(0, 1) && predicts(1, 1));
    put_int(frag + Def_Never_Arc, 0);
    // one with an arc without aiming: even with arc_requires_tmode 0 and another type and state
    put_int(frag + Def_Arc_Without_Aim, 1), put_int(frag + Def_Type, 3), put_int(player_state + Ps_Weapon_State, 1);
    CHECK(predicts(0, 0));
    // aiming: the flag and the sights above 0.85000002 as a double (so 0.85f is above)
    player_state[Ps_Aim_Flags] = Aim_Flag;
    put_float(player_state + Ps_Sights, 0.85f);
    CHECK(!predicts(0, 0));
    put_float(player_state + Ps_Sights, from_bits(bits_of(0.85f) - 1));
    CHECK(predicts(0, 0));
    put_float(player_state + Ps_Sights, from_bits(0x7fc00000));
    CHECK(predicts(0, 0));
    put_float(player_state + Ps_Sights, 1.0f), player_state[Ps_Aim_Flags] = (uint8_t)~Aim_Flag;
    CHECK(predicts(0, 0));
    // one with an arc while aiming
    put_int(frag + Def_Arc_Without_Aim, 0), put_int(frag + Def_Arc_With_Aim, 1);
    CHECK(!predicts(0, 0));
    player_state[Ps_Aim_Flags] = Aim_Flag;
    CHECK(predicts(0, 0));
    put_float(player_state + Ps_Sights, 0.85f);
    CHECK(predicts(0, 0));
    put_float(player_state + Ps_Sights, from_bits(bits_of(0.85f) - 1));
    CHECK(!predicts(0, 0));
    put_int(frag + Def_Arc_With_Aim, 0);
    // a grenade only, in the states of one being held
    grenade_in_hand();
    put_int(frag + Def_Type, 2);
    CHECK(!predicts(0, 1));
    put_int(frag + Def_Type, 1);
    static const struct {
        int32_t state;
        bool arc;
    } states[] = { { 0x24, false }, { 0x25, false }, { 0x26, true }, { 0x2a, true }, { 0x2e, true },
                   { 0x2f, false }, { 0, false }, { -1, false }, { 0x7fffffff, false } };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        put_int(player_state + Ps_Weapon_State, states[i].state);
        CHECK(predicts(0, 1) == states[i].arc);
    }
    // a weapon that also needs ammo: asked for the player state's weapon, after the type
    grenade_in_hand();
    frag[Def_Asks_Ammo] = 1;
    uint64_t held = 0x777;
    memcpy(player_state + Ps_Ammo_Weapon, &held, sizeof(held));
    ammo.answer = 0;
    CHECK(!predicts(0, 1) && ammo.calls == 1 && ammo.player_state == player_state && ammo.weapon == 0x777);
    ammo.answer = 3;
    CHECK(predicts(0, 1) && ammo.calls == 2);
    put_int(frag + Def_Type, 2);
    CHECK(!predicts(0, 1) && ammo.calls == 2);
    put_int(frag + Def_Type, 1);
    CHECK(!predicts(0, 0) && ammo.calls == 2);
    // the dvars' text, as the game sets a bool dvar from it
    CHECK(aae_arc_dvar_switch("1") == 1 && aae_arc_dvar_switch("0") == 0 && aae_arc_dvar_switch("") == 0 &&
          aae_arc_dvar_switch("2") == 1 && aae_arc_dvar_switch("-1") == 1 && aae_arc_dvar_switch(" 1") == 1 &&
          aae_arc_dvar_switch("true") == 0 && aae_arc_dvar_switch("0x1") == 0 && aae_arc_dvar_switch("1x") == 1);
    // the constant is the one T7Overcharged has
    double up = Sights_Up;
    uint64_t up_bits;
    memcpy(&up_bits, &up, sizeof(up_bits));
    CHECK(up_bits == 0x3feb33333deffaa4ULL);
}

// ---- the plugin (a)

static const uint8_t *named_answer;
static int named_asks;
static char named_last[200];
static const uint8_t *named(const char *name) {
    named_asks++;
    snprintf(named_last, sizeof(named_last), "%s", name);
    return named_answer;
}
static void good_bow(uint8_t *definition) {
    memset(definition, 0, Def_Size);
    put_int(definition + Def_Type, Weapon_Type_Grenade);
    put_int(definition + Def_Speed, 1500);
    put_int(definition + Def_Fuse, 3000);
    put_float(definition + Def_Lifetime, 5.0f);
}

static void test_weapon_name(void) {
    char name[Name_Size];
    CHECK(aae_arc_weapon_name("elemental_bow4_zm", name) && strcmp(name, "elemental_bow4_zm") == 0);
    CHECK(aae_arc_weapon_name("a-b.c/D_9", name) && strcmp(name, "a-b.c/D_9") == 0);
    CHECK(!aae_arc_weapon_name("", name) && !aae_arc_weapon_name(NULL, name));
    char text[200];
    memset(text, 'z', 127), text[127] = '\0';
    CHECK(aae_arc_weapon_name(text, name) && strlen(name) == 127);
    memset(text, 'Z', 128), text[128] = '\0';
    CHECK(!aae_arc_weapon_name(text, name));
    // every character: letters, digits and _ - . / only
    for (int c = 1; c < 256; c++) {
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                       c == '-' || c == '.' || c == '/';
        char one[4] = { 'x', (char)c, 'y', '\0' };
        CHECK(aae_arc_weapon_name(one, name) == allowed);
    }
}

static void test_definition_problem(void) {
    static uint8_t original[Def_Size], replacement[Def_Size];
    memset(original, 0, sizeof(original));
    put_int(original + Def_Type, Weapon_Type_Grenade);
    good_bow(replacement);
    CHECK(aae_arc_definition_problem(original, replacement) == NULL);
    put_int(replacement + Def_Type, 2);
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "WEAPON_TYPE_MISMATCH") == 0);
    put_int(original + Def_Type, 2);
    CHECK(aae_arc_definition_problem(original, replacement) == NULL);
    put_int(original + Def_Type, Weapon_Type_Grenade);
    // the speeds: the first from 0 to 100000, the others from -100000 to 100000
    static const struct {
        int which;
        int32_t value;
        const char *problem;
    } speeds[] = { { 0, 100000, NULL }, { 0, 100001, "SPEED_INVALID" }, { 0, -1, "SPEED_INVALID" },
                   { 1, 100000, NULL }, { 1, 100001, "SPEED_UP_INVALID" }, { 1, -100000, NULL },
                   { 1, -100001, "SPEED_UP_INVALID" }, { 2, 100000, NULL }, { 2, 100001, "SPEED_RELATIVE_UP_INVALID" },
                   { 2, -100000, NULL }, { 2, -100001, "SPEED_RELATIVE_UP_INVALID" }, { 3, 100000, NULL },
                   { 3, 100001, "SPEED_FORWARD_INVALID" }, { 3, -100000, NULL },
                   { 3, -100001, "SPEED_FORWARD_INVALID" }, { 3, INT32_MIN, "SPEED_FORWARD_INVALID" } };
    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        good_bow(replacement);
        put_int(replacement + Def_Speed, speeds[i].which == 0 ? 1500 : 0);
        put_int(replacement + Def_Speed + 4 * speeds[i].which, speeds[i].value);
        const char *problem = aae_arc_definition_problem(original, replacement);
        CHECK(speeds[i].problem == NULL ? problem == NULL : problem != NULL && strcmp(problem, speeds[i].problem) == 0);
    }
    // all four 0, or only one of them not
    good_bow(replacement);
    put_int(replacement + Def_Speed, 0);
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "ALL_SPEEDS_ZERO") == 0);
    for (int which = 0; which < 4; which++) {
        put_int(replacement + Def_Speed + 4 * which, -1 + 2 * (which == 0));
        CHECK(aae_arc_definition_problem(original, replacement) == NULL);
        put_int(replacement + Def_Speed + 4 * which, 0);
    }
    // the fuse: up to 600000 ms
    good_bow(replacement);
    put_int(replacement + Def_Fuse, 600000);
    CHECK(aae_arc_definition_problem(original, replacement) == NULL);
    put_int(replacement + Def_Fuse, 600001);
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "FUSE_INVALID") == 0);
    put_int(replacement + Def_Fuse, -1);
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "FUSE_INVALID") == 0);
    // the lifetime: a number from 0 to 100000
    static const struct {
        uint32_t bits;
        bool good;
    } lifetimes[] = { { 0x00000000, true }, { 0x80000000, true }, { 0x47c35000, true }, { 0x47c35001, false },
                      { 0xbf800000, false }, { 0x80000001, false }, { 0x7f800000, false }, { 0x7fc00000, false },
                      { 0xff800000, false } };
    for (size_t i = 0; i < sizeof(lifetimes) / sizeof(lifetimes[0]); i++) {
        good_bow(replacement);
        put_float(replacement + Def_Lifetime, from_bits(lifetimes[i].bits));
        const char *problem = aae_arc_definition_problem(original, replacement);
        CHECK(lifetimes[i].good ? problem == NULL : problem != NULL && strcmp(problem, "LIFETIME_INVALID") == 0);
    }
    // the two bytes
    good_bow(replacement);
    replacement[Def_Timed_Detonation] = 1;
    CHECK(aae_arc_definition_problem(original, replacement) == NULL);
    replacement[Def_Timed_Detonation] = 2;
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "TIMED_DETONATION_INVALID") == 0);
    good_bow(replacement);
    replacement[Def_Impact_Explode] = 1;
    CHECK(aae_arc_definition_problem(original, replacement) == NULL);
    replacement[Def_Impact_Explode] = 2;
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "IMPACT_EXPLODE_INVALID") == 0);
    // timed detonation without a fuse
    good_bow(replacement);
    replacement[Def_Timed_Detonation] = 1, put_int(replacement + Def_Fuse, 0);
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "FUSE_MISSING_FOR_TIMED_DETONATION") == 0);
    replacement[Def_Timed_Detonation] = 0;
    CHECK(aae_arc_definition_problem(original, replacement) == NULL);
    // the first reason in the plugin's order counts
    good_bow(replacement);
    put_int(replacement + Def_Type, 4), put_int(replacement + Def_Speed, -5), put_int(replacement + Def_Fuse, -5);
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "WEAPON_TYPE_MISMATCH") == 0);
    put_int(replacement + Def_Type, 1);
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "SPEED_INVALID") == 0);
    put_int(replacement + Def_Speed, 5), put_float(replacement + Def_Lifetime, -1.0f);
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "FUSE_INVALID") == 0);
    put_int(replacement + Def_Fuse, 5), replacement[Def_Timed_Detonation] = 9, replacement[Def_Impact_Explode] = 9;
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "LIFETIME_INVALID") == 0);
    put_float(replacement + Def_Lifetime, 1.0f);
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "TIMED_DETONATION_INVALID") == 0);
    replacement[Def_Timed_Detonation] = 1;
    CHECK(strcmp(aae_arc_definition_problem(original, replacement), "IMPACT_EXPLODE_INVALID") == 0);
}

static void test_prediction_definition(void) {
    static uint8_t original[Def_Size], replacement[Def_Size];
    memset(original, 0, sizeof(original));
    put_int(original + Def_Type, Weapon_Type_Grenade);
    good_bow(replacement);
    named_answer = replacement, named_asks = 0;
    CHECK(aae_arc_prediction_definition(original, "elemental_bow4_zm", named) == replacement && named_asks == 1 &&
          strcmp(named_last, "elemental_bow4_zm") == 0);
    // none of it without a good name, a definition of that name, or one with a problem
    CHECK(aae_arc_prediction_definition(original, "", named) == original);
    CHECK(aae_arc_prediction_definition(original, NULL, named) == original);
    CHECK(aae_arc_prediction_definition(original, "bow zm", named) == original && named_asks == 1);
    CHECK(aae_arc_prediction_definition(NULL, "elemental_bow4_zm", named) == NULL && named_asks == 1);
    named_answer = NULL;
    CHECK(aae_arc_prediction_definition(original, "elemental_bow4_zm", named) == original);
    named_answer = original;
    CHECK(aae_arc_prediction_definition(original, "elemental_bow4_zm", named) == original);
    named_answer = replacement;
    put_int(replacement + Def_Fuse, -1);
    CHECK(aae_arc_prediction_definition(original, "elemental_bow4_zm", named) == original);
}

// ---- the plugin (b)

static void test_usable(void) {
    float point[3] = { 9999999.0f, -9999999.0f, 0.0f };
    CHECK(aae_arc_usable(point));
    for (int c = 0; c < 3; c++) {
        static const uint32_t bad[] = { 0x4b189680, 0xcb189680, 0x7f800000, 0xff800000, 0x7fc00000, 0x7f7fffff };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            float one[3] = { 1.0f, 2.0f, 3.0f };
            one[c] = from_bits(bad[i]);
            CHECK(!aae_arc_usable(one));
        }
    }
}

// the plugin's own bend, as the cases in arc_bend_cases.h gave it (under Wine, see there)
#define Plugin_Lines 2835
#define Plugin_Checksum 0x6c5a987f582663fcULL
static struct {
    uint64_t checksum;
    int lines;
} cases_out;
static void arc_cases_line(const char *line) {
    for (const char *p = line; *p != '\0'; p++)
        cases_out.checksum = (cases_out.checksum ^ (unsigned char)*p) * 0x100000001b3ULL;
    cases_out.checksum = (cases_out.checksum ^ '\n') * 0x100000001b3ULL;
    cases_out.lines++;
}
static int side_bend(float (*points)[3], const float anchor[3], int n) {
    return aae_arc_bend(points, anchor, n);
}
static int side_usable(const float point[3]) {
    return aae_arc_usable(point);
}

static void test_bend(void) {
    // like the plugin, bit for bit
    cases_out.checksum = 0xcbf29ce484222325ULL, cases_out.lines = 0;
    arc_cases_run();
    CHECK(cases_out.lines == Plugin_Lines);
    CHECK(cases_out.checksum == Plugin_Checksum);
    if (cases_out.checksum != Plugin_Checksum)
        printf("test_aae_arc: the cases came to checksum %#018llx, not the plugin's\n",
               (unsigned long long)cases_out.checksum);
    // a straight line from the start: points 1 to k - 1 become the curve, the rest stays
    float points[Points_Most + 1][3];
    for (int i = 0; i <= Points_Most; i++)
        points[i][0] = 10.0f * (float)i, points[i][1] = 0.0f, points[i][2] = 0.0f;
    const float anchor[3] = { 0.0f, 0.0f, 0.0f };
    CHECK(aae_arc_bend(points, anchor, 20) == 5);
    CHECK(points[5][0] == 50.0f && points[6][0] == 60.0f && points[0][0] == 0.0f);
    for (int i = 1; i < 5; i++)
        CHECK(points[i][0] > points[i - 1][0] && points[i][0] < 50.0f && points[i][1] == 0.0f && points[i][2] == 0.0f);
    // how many: (n + 3) / 4 from 4 to 16, and not for fewer than 8 points or with no point behind point k
    for (int n = 0; n <= 70; n++) {
        for (int i = 0; i <= Points_Most; i++)
            points[i][0] = 10.0f * (float)i + 3.0f, points[i][1] = 1.0f, points[i][2] = 0.0f;
        int k = (n + 3) / 4 < 4 ? 4 : (n + 3) / 4 > 16 ? 16 : (n + 3) / 4;
        CHECK(aae_arc_bend(points, anchor, n) == (n < 8 ? 0 : k));
    }
}

static void test_anchored(void) {
    float points[Points_Most + 1][3], out[Points_Most + 1][3];
    for (int i = 0; i <= Points_Most; i++)
        points[i][0] = 5.0f * (float)i + 100.0f, points[i][1] = 2.0f * (float)i, points[i][2] = 50.0f;
    const float anchor[3] = { 90.0f, -5.0f, 60.0f }, end[3] = { 400.0f, 7.0f, 8.0f };
    // the anchor first, the end last, the points between as the bend leaves them
    float bent[Points_Most + 1][3];
    for (int n = 1; n <= Points_Most; n++) {
        memset(out, 0, sizeof(out));
        CHECK(aae_arc_anchored_points(anchor, (const float (*)[3])points, n, end, out));
        memcpy(bent, points, sizeof(bent));
        memcpy(bent[n], end, sizeof(bent[n]));
        aae_arc_bend(bent, anchor, n);
        CHECK(memcmp(out[0], anchor, sizeof(out[0])) == 0 && memcmp(out[n], end, sizeof(out[n])) == 0);
        for (int i = 1; i < n; i++)
            CHECK(memcmp(out[i], bent[i], sizeof(out[i])) == 0);
    }
    CHECK(!aae_arc_anchored_points(anchor, (const float (*)[3])points, 0, end, out));
    CHECK(!aae_arc_anchored_points(anchor, (const float (*)[3])points, Points_Most + 1, end, out));
    CHECK(!aae_arc_anchored_points(anchor, (const float (*)[3])points, -1, end, out));
    CHECK(!aae_arc_anchored_points(anchor, NULL, 10, end, out));
    float odd[3] = { 1.0f, from_bits(0x7fc00000), 2.0f };
    CHECK(!aae_arc_anchored_points(odd, (const float (*)[3])points, 10, end, out));
    CHECK(!aae_arc_anchored_points(anchor, (const float (*)[3])points, 10, odd, out));
    // every point up to the last is to be usable, and only those
    points[9][1] = 1.0e7f;
    CHECK(!aae_arc_anchored_points(anchor, (const float (*)[3])points, 10, end, out));
    CHECK(aae_arc_anchored_points(anchor, (const float (*)[3])points, 9, end, out));
    points[0][2] = from_bits(0x7f800000);
    CHECK(!aae_arc_anchored_points(anchor, (const float (*)[3])points, 9, end, out));
}

static void test_tag(void) {
    char tag[Tag_Size];
    uint64_t key = Fnv_Basis;
    for (const char *p = "tag_flash"; *p != '\0'; p++)
        key = (key ^ (unsigned char)*p) * Fnv_Prime;
    CHECK(aae_arc_tag("tag_flash", tag) == key && strcmp(tag, "tag_flash") == 0);
    CHECK(aae_arc_tag("", tag) == Fnv_Basis && tag[0] == '\0');
    CHECK(aae_arc_tag(NULL, tag) == Fnv_Basis && tag[0] == '\0');
    char text[80];
    memset(text, 'a', 63), text[63] = '\0';
    CHECK(aae_arc_tag(text, tag) != Fnv_Basis && strlen(tag) == 63);
    memset(text, 'a', 64), text[64] = '\0';
    CHECK(aae_arc_tag(text, tag) == Fnv_Basis && tag[0] == '\0');
    for (int c = 1; c < 256; c++) {
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        char one[4] = { 'x', (char)c, 'y', '\0' };
        CHECK((aae_arc_tag(one, tag) != Fnv_Basis) == allowed && (tag[0] == 'x') == allowed);
    }
}

static void test_bone(void) {
    DObjGetBoneIndex = game_bone_index;
    memset(view_model, 0, sizeof(view_model));
    memset(skeleton, 0, sizeof(skeleton));
    set_bone(7, 1.0f, 2.0f, 3.0f, true);
    set_bone(31, 4.0f, 5.0f, 6.0f, true);
    set_bone(33, 7.0f, 8.0f, 9.0f, false);
    aae_arc_bone_cache_t cache = { .index = -2 };
    float position[3] = { -1.0f, -1.0f, -1.0f };
    bone_asks = 0;
    CHECK(aae_arc_bone_position(view_model, &tree_a, Bones, skeleton, 100, &cache, bone_index, position) &&
          position[0] == 1.0f && position[1] == 2.0f && position[2] == 3.0f && bone_asks == 1);
    CHECK(cache.object == view_model && cache.tree == &tree_a && cache.name == 100 && cache.index == 7);
    // the same bone again: from the cache
    CHECK(aae_arc_bone_position(view_model, &tree_a, Bones, skeleton, 100, &cache, bone_index, position) &&
          bone_asks == 1);
    // another tree, another bone or another model: asked again
    CHECK(aae_arc_bone_position(view_model, &tree_b, Bones, skeleton, 100, &cache, bone_index, position) &&
          bone_asks == 2);
    CHECK(aae_arc_bone_position(view_model, &tree_b, Bones, skeleton, 200, &cache, bone_index, position) &&
          bone_asks == 3 && position[0] == 4.0f && cache.index == 31);
    // the cached index not below the bone count: asked again, and then outside
    CHECK(!aae_arc_bone_position(view_model, &tree_b, 31, skeleton, 200, &cache, bone_index, position) &&
          bone_asks == 4 && cache.index == -1);
    CHECK(!aae_arc_bone_position(view_model, &tree_b, 31, skeleton, 200, &cache, bone_index, position) &&
          bone_asks == 4);
    // a bone that isn't there is kept as -1 and not asked for again
    CHECK(!aae_arc_bone_position(view_model, &tree_b, Bones, skeleton, 999, &cache, bone_index, position) &&
          bone_asks == 5 && cache.index == -1);
    CHECK(!aae_arc_bone_position(view_model, &tree_b, Bones, skeleton, 999, &cache, bone_index, position) &&
          bone_asks == 5);
    // one whose skeleton hasn't been worked out this frame
    CHECK(!aae_arc_bone_position(view_model, &tree_b, Bones, skeleton, 300, &cache, bone_index, position) &&
          cache.index == 33);
    set_bone(33, 7.0f, 8.0f, 9.0f, true);
    CHECK(aae_arc_bone_position(view_model, &tree_b, Bones, skeleton, 300, &cache, bone_index, position) &&
          position[2] == 9.0f);
    // one whose place isn't usable
    set_bone(33, 7.0f, 1.0e7f, 9.0f, true);
    position[0] = -1.0f;
    CHECK(!aae_arc_bone_position(view_model, &tree_b, Bones, skeleton, 300, &cache, bone_index, position) &&
          position[0] == -1.0f);
    // the game's no counts, whatever it left in the index
    cache = (aae_arc_bone_cache_t){ .index = -2 };
    CHECK(!aae_arc_bone_position(view_model, &tree_a, Bones, skeleton, 400, &cache, bone_index, position) &&
          cache.index == -1);
    // no name, or no way to ask
    CHECK(!aae_arc_bone_position(view_model, &tree_b, Bones, skeleton, 0, &cache, bone_index, position));
    CHECK(!aae_arc_bone_position(view_model, &tree_b, Bones, skeleton, 100, &cache, NULL, position));
    // the game's answer: no, a negative index, one beyond the count
    bones[0].index = -3;
    cache = (aae_arc_bone_cache_t){ .index = -2 };
    CHECK(!aae_arc_bone_position(view_model, &tree_a, Bones, skeleton, 100, &cache, bone_index, position) &&
          cache.index == -1);
    bones[0].index = Bones;
    cache = (aae_arc_bone_cache_t){ .index = -2 };
    CHECK(!aae_arc_bone_position(view_model, &tree_a, Bones, skeleton, 100, &cache, bone_index, position) &&
          cache.index == -1);
    bones[0].index = 7;
}

// ---- the plugin (c)

static void test_guards(void) {
    for (int which = -1; which <= 4; which++)
        for (int segments = -1; segments <= 6; segments++)
            CHECK(aae_arc_may_add_segment(which, segments) ==
                  (which >= 0 && which <= 3 && segments >= 0 && segments <= 4));
    for (int segments = -1; segments <= 6; segments++) {
        CHECK(aae_arc_may_draw_marker(0, segments));
        CHECK(aae_arc_may_draw_marker(1, segments) == (segments >= 1 && segments <= 4));
        CHECK(aae_arc_may_draw_marker(0xff, segments) == (segments >= 1 && segments <= 4));
    }
}

// ---- the hooks against the made-up game

static void test_init(void) {
    const uint64_t base = 0x100000000;
    hooks = 0;
    aae_arc_init(base, 0);
    CHECK(hooks == 0);
    // without the stubs for the places inside the game's functions: nothing at all
    const uint32_t places[] = { ADDR_CG_ArcPrediction_NoTacticalMode, ADDR_CG_ArcPrediction_WeaponDef,
                                ADDR_CG_DrawArc_FirstLine, ADDR_CG_DrawArc_SecondLine, ADDR_CG_DrawArc_MarkerTest };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        stub_missing_for = (int)places[i], hooks = 0;
        aae_arc_init(base, 1);
        CHECK(hooks == 0);
    }
    stub_missing_for = -1, hooks = 0;
    aae_arc_init(base, 1);
    CHECK(hooks == 9);
    CHECK(was_hooked(base, ADDR_CG_ShouldPredictArc, (void *)fake_CG_ShouldPredictArc,
                     (void **)&orig_CG_ShouldPredictArc));
    CHECK(was_hooked(base, ADDR_CG_ArcPrediction_NoTacticalMode, (void *)handler_Arc_No_Tactical_Mode,
                     &orig_Arc_No_Tactical_Mode) &&
          other_Arc_No_Tactical_Mode == (void *)(base + ADDR_CG_ArcPrediction_TacticalMode));
    CHECK(was_hooked(base, ADDR_R_AddCodeMeshDrawSurf, (void *)fake_R_AddCodeMeshDrawSurf,
                     (void **)&orig_R_AddCodeMeshDrawSurf));
    CHECK(was_hooked(base, ADDR_CG_ArcPrediction_WeaponDef, (void *)handler_Arc_Weapon_Definition,
                     &orig_Arc_Weapon_Definition));
    CHECK(was_hooked(base, ADDR_DObjCalcSkel, (void *)fake_DObjCalcSkel, (void **)&orig_DObjCalcSkel));
    CHECK(was_hooked(base, ADDR_CG_DrawArc_FirstLine, (void *)handler_Arc_First_Line, &orig_Arc_First_Line) &&
          was_hooked(base, ADDR_CG_DrawArc_SecondLine, (void *)handler_Arc_Second_Line, &orig_Arc_Second_Line));
    CHECK(was_hooked(base, ADDR_CG_DrawArc_MarkerTest, (void *)handler_Arc_Marker, &orig_Arc_Marker));
    CHECK(was_hooked(base, ADDR_CG_ArcPrediction_AddSegment, (void *)fake_CG_ArcPrediction_AddSegment,
                     (void **)&orig_CG_ArcPrediction_AddSegment));
    CHECK(Dvar_FindByHash == (void *)(base + ADDR_Dvar_FindByHash) &&
          Dvar_GetString == (void *)(base + ADDR_Dvar_GetString) && Dvar_GetInt == (void *)(base + ADDR_Dvar_GetInt));
    CHECK(BG_GetWeaponDef == (void *)(base + ADDR_BG_GetWeaponDef) &&
          BG_WeaponAmmo == (void *)(base + ADDR_BG_WeaponAmmo) &&
          BG_FindWeaponIndexByName == (void *)(base + ADDR_BG_FindWeaponIndexByName));
    CHECK(SL_GetStringOfSize == (void *)(base + ADDR_SL_GetStringOfSize) &&
          DObjGetBoneIndex == (void *)(base + ADDR_DObjGetBoneIndex));
    CHECK(skeleton_sites[0] == (const void *)(base + ADDR_DObjCalcSkel_ViewModelReturn) &&
          skeleton_sites[1] == (const void *)(base + ADDR_DObjCalcSkel_ViewModelReturn2));
    // the places in the game's own structures, as they were read from its code
    CHECK(Cg_Size == 0x342a80 && Cg_Player_State == 0x11a8d0 && Ps_Aim_Flags == 0x12 && Ps_Flags == 0x20 &&
          Ps_Weapon_State == 0x5c && Ps_Ammo_Weapon == 0x298 && Ps_Sights == 0x2fc);
    CHECK(Def_Type == 0x6c && Def_Never_Arc == 0xf8c && Def_Arc_Without_Aim == 0xf84 && Def_Arc_With_Aim == 0xf88 &&
          Def_Asks_Ammo == 0x10dd && Def_Fuse == 0xc28 && Def_Speed == 0xfec && Def_Lifetime == 0x1004 &&
          Def_Impact_Explode == 0x10c9 && Def_Timed_Detonation == 0x10de);
    CHECK(Arc_Block_Size == 0xeec && Arc_Segments == 0xe60 && Arc_Marker == 0xee9 && View_Size == 0x3a0 &&
          Object_Tree == 0x18 && Object_Skeleton == 0x58 && Object_Bone_Count == 0x120 && Object_Computed == 0xc8 &&
          Skeleton_Bone_Size == 0x20 && Skeleton_Translation == 0x10 && Bones_Most == 0x180);
    CHECK(Draw_Local_Client == -0xac && Draw_Command == -0xa8 && Command_Start == 4 && Command_End == 0x10 &&
          Command_Last == 0x40 && Command_Points == 0x68 && Material_Techniques == 0x278 && Dvar_Type == 0x1c &&
          Dvar_Type_String == 8 && Ring_Size == 1024 && WeaponVariantDef_WeaponDef == 0x18);
}

// from here on the code works on the made-up game's image
static void use_made_up_game(void) {
    aae_arc_init((uint64_t)image, 1);
    loaded_mod = loaded_name;
    Dvar_FindByHash = game_find_dvar;
    Dvar_GetString = game_dvar_string;
    Dvar_GetInt = game_dvar_int;
    BG_GetWeaponDef = game_weapon_def;
    BG_WeaponAmmo = game_weapon_ammo;
    BG_FindWeaponIndexByName = game_weapon_index;
    SL_GetStringOfSize = game_string;
    DObjGetBoneIndex = game_bone_index;
}

static void test_test_hook(void) {
    uint8_t *ps = cgs + Cg_Player_State;
    memset(ps, 0, 0x400);
    put_int(ps + Ps_Weapon_State, 0x2a);
    memset(frag, 0, sizeof(frag));
    put_int(frag + Def_Type, Weapon_Type_Grenade);
    no_dvar("force_arc"), no_dvar("arc_requires_tmode");
    // without the mod: the game's own test, handed on as it came
    set_mod(false);
    game_test.calls = 0, game_test.answer = true;
    CHECK(fake_CG_ShouldPredictArc(0, 0x12b) && game_test.calls == 1 && game_test.local_client == 0 &&
          game_test.weapon == 0x12b);
    game_test.answer = false;
    CHECK(!fake_CG_ShouldPredictArc(1, 0x99) && game_test.calls == 2 && game_test.local_client == 1 &&
          game_test.weapon == 0x99);
    // with it: T7Overcharged's, on the local client's player state; before the script makes the dvars a grenade has
    // an arc (T7Overcharged makes arc_requires_tmode with true)
    set_mod(true);
    CHECK(fake_CG_ShouldPredictArc(0, 0x12b) && game_test.calls == 2);
    set_text("arc_requires_tmode", "0");
    CHECK(!fake_CG_ShouldPredictArc(0, 0x12b));
    set_text("arc_requires_tmode", "1");
    CHECK(fake_CG_ShouldPredictArc(0, 0x12b));
    put_int(ps + Ps_Weapon_State, 0x2f);
    CHECK(!fake_CG_ShouldPredictArc(0, 0x12b));
    set_text("force_arc", "1");
    CHECK(fake_CG_ShouldPredictArc(0, 0x12b) && fake_CG_ShouldPredictArc(3, 0));
    set_text("force_arc", "0");
    CHECK(!fake_CG_ShouldPredictArc(0, 0x12b));
    put_int(ps + Ps_Weapon_State, 0x2a);
    // a local client that isn't there, or no cg_t at all
    memcpy(cgs + Cg_Size + Cg_Player_State, ps, 0x400);
    CHECK(!fake_CG_ShouldPredictArc(1, 0x12b) && !fake_CG_ShouldPredictArc(-1, 0x12b));
    put_int(image + ADDR_LOCAL_CLIENT_COUNT, 2);
    CHECK(fake_CG_ShouldPredictArc(1, 0x12b));
    put_pointer(image + ADDR_cgArray, NULL);
    CHECK(!fake_CG_ShouldPredictArc(1, 0x12b));
    put_int(image + ADDR_LOCAL_CLIENT_COUNT, 1);
    CHECK(!fake_CG_ShouldPredictArc(0, 0x12b));
    put_pointer(image + ADDR_cgArray, cgs);
    // dvars of another kind count by the game's reading as a number; their text isn't asked for
    wrong_asks = 0;
    set_dvar_kind("arc_requires_tmode", true, 1, "0");
    CHECK(!fake_CG_ShouldPredictArc(0, 0x12b));
    set_dvar_kind("arc_requires_tmode", true, 1, "1");
    CHECK(fake_CG_ShouldPredictArc(0, 0x12b) && wrong_asks == 0);
    no_dvar("force_arc"), no_dvar("arc_requires_tmode");
    // the job's own Tactical Mode test: always the branch for it being on, with the mod
    CHECK(handler_Arc_No_Tactical_Mode(1, 2));
    set_mod(false);
    CHECK(!handler_Arc_No_Tactical_Mode(1, 2));
}

static void test_mesh_guard(void) {
    static uint8_t material[0x300];
    static int techniques = 1;
    put_pointer(material + Material_Techniques, &techniques);
    set_mod(true);
    game_mesh.calls = 0;
    fake_R_AddCodeMeshDrawSurf(material, 2, 3, 4, 5, 6, 7, 8, 9);
    const uint64_t expected[9] = { (uint64_t)material, 2, 3, 4, 5, 6, 7, 8, 9 };
    CHECK(game_mesh.calls == 1 && memcmp(game_mesh.arguments, expected, sizeof(expected)) == 0);
    fake_R_AddCodeMeshDrawSurf(NULL, 2, 3, 4, 5, 6, 7, 8, 9);
    put_pointer(material + Material_Techniques, NULL);
    fake_R_AddCodeMeshDrawSurf(material, 2, 3, 4, 5, 6, 7, 8, 9);
    CHECK(game_mesh.calls == 1);
    // without the mod: the game's own
    set_mod(false);
    fake_R_AddCodeMeshDrawSurf(material, 2, 3, 4, 5, 6, 7, 8, 9);
    fake_R_AddCodeMeshDrawSurf(NULL, 2, 3, 4, 5, 6, 7, 8, 9);
    CHECK(game_mesh.calls == 3);
}

static void test_definition_detour(void) {
    detour_registers_t registers;
    memset(&registers, 0, sizeof(registers));
    memset(frag, 0, sizeof(frag));
    put_int(frag + Def_Type, Weapon_Type_Grenade);
    good_bow(bow4);
    set_variant(0x41, variants[0], bow4);
    set_variant(0x42, NULL, NULL);
    set_variant(0x37, variants[1], NULL);
    set_variant(0x12b, variants[2], frag);
    static uint8_t outside[2][0x20];
    set_variant(0, outside[0], bow4);
    set_variant(0x200, outside[1], bow4);
    set_mod(true);
    set_text("aae_arc_weapondef", "elemental_bow4_zm");
    registers.rax = (uint64_t)frag, registers.rbx = 0x12b;
    handler_Arc_Weapon_Definition(&registers);
    CHECK(registers.rax == (uint64_t)bow4 && strcmp(last_name_asked, "elemental_bow4_zm") == 0 &&
          registers.rbx == 0x12b);
    // the weapon's own: no text, a name the game doesn't have, an index outside, a variant without a definition,
    // the same definition, one with a problem, a dvar without text, and without the mod
    static const char *const own[] = { "", "nothing_zm", "big_zm", "zero_zm", "bow_zm", "frag_grenade", "not_there" };
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        set_text("aae_arc_weapondef", own[i]);
        registers.rax = (uint64_t)frag;
        handler_Arc_Weapon_Definition(&registers);
        CHECK(registers.rax == (uint64_t)frag);
    }
    set_text("aae_arc_weapondef", "elemental_bow4_zm");
    put_int(bow4 + Def_Type, 2);
    registers.rax = (uint64_t)frag;
    handler_Arc_Weapon_Definition(&registers);
    CHECK(registers.rax == (uint64_t)frag);
    put_int(bow4 + Def_Type, Weapon_Type_Grenade);
    set_dvar_kind("aae_arc_weapondef", true, 1, "elemental_bow4_zm");
    handler_Arc_Weapon_Definition(&registers);
    CHECK(registers.rax == (uint64_t)frag);
    set_text("aae_arc_weapondef", "elemental_bow4_zm");
    set_mod(false);
    handler_Arc_Weapon_Definition(&registers);
    CHECK(registers.rax == (uint64_t)frag);
    set_mod(true);
    handler_Arc_Weapon_Definition(&registers);
    CHECK(registers.rax == (uint64_t)bow4);
}

// the anchor as the draw code would read it now: after noting the view models, as it does
static bool anchor_of(int which, float anchor[3]) {
    note_view_models();
    char tag[Tag_Size];
    const char *text = game_find_dvar(gsc_hash("aae_arc_tag")) != NULL ? dvars[3].text : NULL;
    return anchor_now(which, aae_arc_tag(text, tag), anchor);
}
static void reset_views(void) {
    for (int which = 0; which < Local_Clients; which++) {
        set_view_model(which, NULL);
        atomic_store(&views[which].object, NULL);
        atomic_store(&views[which].tree, NULL);
        views[which].custom = views[which].right = (aae_arc_bone_cache_t){ .index = -2 };
        atomic_store(&views[which].valid, false);
    }
    atomic_store(&right_name, 0), atomic_store(&custom_name, 0), atomic_store(&tag_key, Fnv_Basis);
}
static void a_view_model(void) {
    reset_views();
    memset(view_model, 0, sizeof(view_model));
    memset(skeleton, 0, sizeof(skeleton));
    set_model(view_model, &tree_a, Bones, skeleton);
    set_bone(7, 11.0f, 12.0f, 13.0f, true);
    set_bone(31, 21.0f, 22.0f, 23.0f, true);
    set_bone(33, 31.0f, 32.0f, 33.0f, false);
    set_view_model(0, view_model);
    set_text("aae_arc_tag", "");
    strings_down = false;
    set_mod(true);
}
static bool skeleton_done(const uint8_t *object) {
    return calc_skeleton(object, 2, 3, 4, 5, 6, skeleton_sites[0]) == 0x5ca1ab1e;
}

static void test_anchor(void) {
    float anchor[3];
    a_view_model();
    // after the view model's skeleton: its right hand, from either of the two places
    game_skeleton.calls = 0;
    CHECK(calc_skeleton(view_model, 2, 3, 4, 5, 6, skeleton_sites[1]) == 0x5ca1ab1e && game_skeleton.calls == 1);
    const uint64_t expected[6] = { (uint64_t)view_model, 2, 3, 4, 5, 6 };
    CHECK(memcmp(game_skeleton.arguments, expected, sizeof(expected)) == 0);
    CHECK(anchor_of(0, anchor) && anchor[0] == 11.0f && anchor[1] == 12.0f && anchor[2] == 13.0f);
    CHECK(!anchor_of(1, anchor));
    // from anywhere else, or for another model, or without the mod: nothing
    a_view_model();
    CHECK(calc_skeleton(view_model, 2, 3, 4, 5, 6, (const void *)test_anchor) == 0x5ca1ab1e && !anchor_of(0, anchor));
    CHECK(skeleton_done(other_model) && !anchor_of(0, anchor));
    CHECK(calc_skeleton(NULL, 2, 3, 4, 5, 6, skeleton_sites[0]) == 0x5ca1ab1e && !anchor_of(0, anchor));
    set_mod(false);
    CHECK(skeleton_done(view_model) && !anchor_of(0, anchor));
    set_mod(true);
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor));
    // the bone aae_arc_tag names: its key counts, and changing the text forgets every anchor until the next one
    set_text("aae_arc_tag", "tag_flash");
    CHECK(!anchor_of(0, anchor));
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor) && anchor[0] == 21.0f);
    // a bone that isn't worked out, one the model doesn't have, and the right hand by its name: the right hand
    set_text("aae_arc_tag", "tag_nothing");
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor) && anchor[0] == 11.0f);
    set_text("aae_arc_tag", "tag_not_on_it");
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor) && anchor[0] == 11.0f);
    bone_asks = 0;
    set_text("aae_arc_tag", "tag_weapon_right");
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor) && anchor[0] == 11.0f && bone_asks == 0);
    // text the plugin doesn't take: as none
    set_text("aae_arc_tag", "tag flash");
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor) && anchor[0] == 11.0f);
    // no bone of either name: none
    set_bone(7, 11.0f, 12.0f, 13.0f, false);
    set_text("aae_arc_tag", "");
    CHECK(skeleton_done(view_model) && !anchor_of(0, anchor));
    set_bone(7, 11.0f, 12.0f, 13.0f, true);
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor));
    // the game without the strings: no right hand, so none
    reset_views();
    set_view_model(0, view_model);
    strings_down = true;
    CHECK(skeleton_done(view_model) && !anchor_of(0, anchor));
    strings_down = false;
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor));
    // a model without a tree, without bones, with too many, without a skeleton
    set_model(view_model, NULL, Bones, skeleton);
    CHECK(skeleton_done(view_model) && !anchor_of(0, anchor));
    set_model(view_model, &tree_a, 0, skeleton);
    CHECK(skeleton_done(view_model) && !anchor_of(0, anchor));
    set_model(view_model, &tree_a, Bones_Most + 1, skeleton);
    CHECK(skeleton_done(view_model) && !anchor_of(0, anchor));
    set_model(view_model, &tree_a, Bones_Most, skeleton);
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor));
    set_model(view_model, &tree_a, Bones, NULL);
    CHECK(skeleton_done(view_model) && !anchor_of(0, anchor));
    set_model(view_model, &tree_a, Bones, skeleton);
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor));
    // another tree: what was found out about the model is forgotten, then found again
    bone_asks = 0;
    set_model(view_model, &tree_b, Bones, skeleton);
    CHECK(!anchor_of(0, anchor));
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor) && bone_asks == 1);
    // another view model: the old one's skeleton is no longer the local client's
    set_view_model(0, other_model);
    CHECK(skeleton_done(view_model) && !anchor_of(0, anchor));
    set_view_model(0, view_model);
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor));
    // another model's skeleton, one with all it needs, leaves the local client's anchor alone
    static uint8_t other_skeleton[Bones * Skeleton_Bone_Size];
    set_model(other_model, &tree_a, Bones, other_skeleton);
    CHECK(skeleton_done(other_model) && anchor_of(0, anchor) && anchor[0] == 11.0f);
    // what is read: not while it's written, not of another key, not of another tag text, not without a model
    uint32_t sequence = atomic_load(&views[0].sequence);
    atomic_store(&views[0].sequence, sequence + 1);
    CHECK(!anchor_of(0, anchor));
    atomic_store(&views[0].sequence, sequence);
    CHECK(anchor_of(0, anchor));
    uint64_t key = atomic_load(&views[0].key);
    atomic_store(&views[0].key, key + 1);
    CHECK(!anchor_of(0, anchor));
    atomic_store(&views[0].key, key);
    atomic_store(&tag_key, key + 1);
    CHECK(!anchor_of(0, anchor));
    atomic_store(&tag_key, key);
    CHECK(anchor_of(0, anchor));
    CHECK(!anchor_now(0, key + 1, anchor));
    atomic_store(&views[0].object, NULL);
    CHECK(!anchor_now(0, key, anchor));
    atomic_store(&views[0].object, view_model);
    CHECK(anchor_now(0, key, anchor));
    // a second local client with another model: a new tag text forgets its anchor too, also once it's back
    put_int(image + ADDR_LOCAL_CLIENT_COUNT, 2);
    memcpy(other_skeleton, skeleton, sizeof(other_skeleton));
    memcpy(other_model + Object_Computed, view_model + Object_Computed, 12 * 4);
    set_view_model(1, other_model);
    CHECK(skeleton_done(view_model) && skeleton_done(other_model) && anchor_of(0, anchor) && anchor_of(1, anchor));
    set_text("aae_arc_tag", "tag_flash");
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor) && !anchor_of(1, anchor));
    set_text("aae_arc_tag", "");
    CHECK(!anchor_of(1, anchor) && !anchor_of(0, anchor));
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor) && !anchor_of(1, anchor));
    CHECK(skeleton_done(other_model) && anchor_of(1, anchor));
    set_view_model(1, NULL);
    put_int(image + ADDR_LOCAL_CLIENT_COUNT, 1);
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor));
    // a second local client with the same model has one of its own
    put_int(image + ADDR_LOCAL_CLIENT_COUNT, 2);
    set_view_model(1, view_model);
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor) && anchor_of(1, anchor));
    put_int(image + ADDR_LOCAL_CLIENT_COUNT, 1);
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor) && !anchor_of(1, anchor));
    // the view weapon states not there yet
    put_pointer(image + ADDR_cgViewWeaponStates, NULL);
    CHECK(skeleton_done(view_model) && !anchor_of(0, anchor));
    put_pointer(image + ADDR_cgViewWeaponStates, view_states);
}

static uint8_t draw_frame[0x200];
static void a_command(int which, int segment, int last) {
    uint8_t *frame = draw_frame + 0x100;
    put_int(frame + Draw_Local_Client, which);
    uint8_t *command = frame + Draw_Command;
    memset(command, 0x77, 0x80);
    const uint8_t *points = image + ADDR_CG_ArcBlocks + (size_t)which * Arc_Block_Size + (size_t)segment * 0x2d0;
    put_pointer(command + Command_Points, points);
    put_int(command + Command_Last, last);
    const float start[3] = { 1.0f, 2.0f, 3.0f }, end[3] = { 500.0f, 600.0f, 70.0f };
    memcpy(command + Command_Start, start, sizeof(start));
    memcpy(command + Command_End, end, sizeof(end));
}
static void an_arc(int which) {
    float (*points)[3] = (float (*)[3])(image + ADDR_CG_ArcBlocks + (size_t)which * Arc_Block_Size);
    for (int i = 0; i < 60; i++)
        points[i][0] = 20.0f + 7.0f * (float)i, points[i][1] = 30.0f + 3.0f * (float)i,
        points[i][2] = 40.0f + 9.0f * (float)i - 0.4f * (float)(i * i);
}

static void test_draw(void) {
    detour_registers_t registers;
    memset(&registers, 0, sizeof(registers));
    registers.rbp = (uint64_t)(draw_frame + 0x100);
    uint8_t *command = draw_frame + 0x100 + Draw_Command;
    float anchor[3], points[Points_Most + 1][3];
    a_view_model();
    an_arc(0);
    CHECK(skeleton_done(view_model) && anchor_of(0, anchor));
    // the first segment, in either loop: from the anchor, with the points as the plugin has them
    for (int loop = 0; loop < 2; loop++) {
        a_command(0, 0, 20);
        loop == 0 ? handler_Arc_First_Line(&registers) : handler_Arc_Second_Line(&registers);
        const float (*drawn)[3];
        memcpy(&drawn, command + Command_Points, sizeof(drawn));
        float end[3];
        memcpy(end, command + Command_End, sizeof(end));
        CHECK(memcmp(command + Command_Start, anchor, sizeof(anchor)) == 0 && end[0] == 500.0f);
        CHECK(aae_arc_anchored_points(anchor, (const float (*)[3])(image + ADDR_CG_ArcBlocks), 20, end, points));
        CHECK(drawn != (const float (*)[3])(image + ADDR_CG_ArcBlocks) && memcmp(drawn, points, 21 * 12) == 0);
        CHECK(as_int(command + Command_Last) == 20);
    }
    // each one its own buffer
    a_command(0, 0, 20);
    handler_Arc_First_Line(&registers);
    const void *first, *second;
    memcpy(&first, command + Command_Points, sizeof(first));
    a_command(0, 0, 20);
    handler_Arc_First_Line(&registers);
    memcpy(&second, command + Command_Points, sizeof(second));
    CHECK(first != second);
    // left as they are: another segment, another local client, without the mod, without an anchor, with another
    // text in aae_arc_tag since, and for points the plugin doesn't take
    uint8_t before[0x80];
    a_command(0, 1, 20);
    memcpy(before, command, sizeof(before));
    handler_Arc_First_Line(&registers);
    CHECK(memcmp(before, command, sizeof(before)) == 0);
    static const int others[] = { 1, 4, -1 };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        a_command(others[i] == 1 ? 1 : 0, 0, 20);
        put_int(draw_frame + 0x100 + Draw_Local_Client, others[i]);
        memcpy(before, command, sizeof(before));
        handler_Arc_First_Line(&registers);
        CHECK(memcmp(before, command, sizeof(before)) == 0);
    }
    set_mod(false);
    a_command(0, 0, 20);
    memcpy(before, command, sizeof(before));
    handler_Arc_Second_Line(&registers);
    CHECK(memcmp(before, command, sizeof(before)) == 0);
    set_mod(true);
    set_text("aae_arc_tag", "tag_flash");
    handler_Arc_Second_Line(&registers);
    CHECK(memcmp(before, command, sizeof(before)) == 0);
    set_text("aae_arc_tag", "");
    static const int lasts[] = { 0, 61, -5 };
    for (size_t i = 0; i < sizeof(lasts) / sizeof(lasts[0]); i++) {
        a_command(0, 0, lasts[i]);
        memcpy(before, command, sizeof(before));
        handler_Arc_First_Line(&registers);
        CHECK(memcmp(before, command, sizeof(before)) == 0);
    }
    a_command(0, 0, 20);
    handler_Arc_First_Line(&registers);
    CHECK(memcmp(command + Command_Start, anchor, sizeof(anchor)) == 0);
    set_view_model(0, NULL);
    a_command(0, 0, 20);
    memcpy(before, command, sizeof(before));
    handler_Arc_First_Line(&registers);
    CHECK(memcmp(before, command, sizeof(before)) == 0);
    set_view_model(0, view_model);
}

static void test_guard_places(void) {
    detour_registers_t registers;
    memset(&registers, 0, sizeof(registers));
    registers.rsi = (uint64_t)(image + ADDR_CG_ArcBlocks);
    set_mod(true);
    for (int which = 0; which < Local_Clients; which++) {
        uint8_t *block = image + ADDR_CG_ArcBlocks + (size_t)which * Arc_Block_Size;
        registers.r15 = (uint64_t)which * Arc_Block_Size;
        for (int segments = -1; segments <= 6; segments++) {
            put_int(block + Arc_Segments, segments);
            block[Arc_Marker] = 1;
            handler_Arc_Marker(&registers);
            CHECK(block[Arc_Marker] == (segments >= 1 && segments <= 4));
            block[Arc_Marker] = 0;
            handler_Arc_Marker(&registers);
            CHECK(block[Arc_Marker] == 0);
        }
        set_mod(false);
        put_int(block + Arc_Segments, 5), block[Arc_Marker] = 1;
        handler_Arc_Marker(&registers);
        CHECK(block[Arc_Marker] == 1);
        set_mod(true);
    }
    // a segment more: as the game has it, unless there are 5 already or the local client is none of the first 4
    for (int which = -1; which <= 4; which++) {
        for (int segments = 3; segments <= 6; segments++) {
            if (which >= 0 && which < Local_Clients)
                put_int(image + ADDR_CG_ArcBlocks + (size_t)which * Arc_Block_Size + Arc_Segments, segments);
            game_segment.calls = 0;
            fake_CG_ArcPrediction_AddSegment(which, 2, 3, 4, 5, 6);
            bool added = which >= 0 && which <= 3 && segments < 5;
            CHECK(game_segment.calls == added);
            const uint64_t expected[5] = { 2, 3, 4, 5, 6 };
            if (added)
                CHECK(game_segment.local_client == which &&
                      memcmp(game_segment.arguments, expected, sizeof(expected)) == 0);
        }
    }
    set_mod(false);
    put_int(image + ADDR_CG_ArcBlocks + Arc_Segments, 9);
    game_segment.calls = 0;
    fake_CG_ArcPrediction_AddSegment(0, 2, 3, 4, 5, 6);
    fake_CG_ArcPrediction_AddSegment(7, 2, 3, 4, 5, 6);
    CHECK(game_segment.calls == 2);
    set_mod(true);
}

int main(void) {
    char template[] = "/tmp/bo3macfix-arc-test-XXXXXX";
    if (mkdtemp(template) == NULL)
        return 2;
    snprintf(mod_folder, sizeof(mod_folder), "%s/2631943123", template);
    char command[4096 + 64];
    snprintf(command, sizeof(command), "mkdir -p '%s/T7Overcharged/plugins'", mod_folder);
    if (system(command) != 0)
        return 2;
    snprintf(plugin_file, sizeof(plugin_file), "%s/%s", mod_folder, Plugin_Path);
    close(open(plugin_file, O_CREAT | O_WRONLY, 0644));
    plugin_now_ms = test_plugin_now;
    image = mmap(NULL, Image_Size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (image == MAP_FAILED)
        return 2;
    cgs = calloc(2, Cg_Size);
    view_states = calloc(Local_Clients, View_Size);
    put_pointer(image + ADDR_cgArray, cgs);
    put_int(image + ADDR_LOCAL_CLIENT_COUNT, 1);
    put_pointer(image + ADDR_cgViewWeaponStates, view_states);

    test_t7o_rule();
    test_weapon_name();
    test_definition_problem();
    test_prediction_definition();
    test_usable();
    test_bend();
    test_anchored();
    test_tag();
    test_bone();
    test_guards();
    test_init();
    use_made_up_game();
    test_test_hook();
    test_mesh_guard();
    test_definition_detour();
    test_anchor();
    test_draw();
    test_guard_places();
    // once more with the debug lines on: they only read what the code reads anyway
    static uint8_t arc_material[0x300];
    static int techniques = 1;
    put_pointer(arc_material + Material_Techniques, &techniques);
    put_pointer(image + ADDR_CG_ArcMaterials, arc_material);
    put_pointer(image + ADDR_CG_ArcMaterials + 8, NULL);
    debug = 1;
    test_test_hook();
    test_mesh_guard();
    test_definition_detour();
    test_anchor();
    test_draw();
    test_guard_places();
    debug = 0;

    free(cgs);
    free(view_states);
    munmap(image, Image_Size);
    snprintf(command, sizeof(command), "rm -rf '%s'", template);
    if (system(command) != 0)
        failures++;
    if (failures != 0) {
        printf("test_aae_arc: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_arc: all passed\n");
    return 0;
}
