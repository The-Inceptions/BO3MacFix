// aae_viewmodel.c: T7Overcharged's reading of the dvars, of viewmodel_move.cfg and of viewmodel_hide.cfg, its rules for
// the moves of the view model's bones and for the bullets it hides, and the hook, against a made-up game.
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "../source/aae_viewmodel.c"

// ---- what the code asks of the rest of BO3MacFix
static char mod_folder[4096];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
static struct {
    void *address, *replacement, **original;
} hooked[4];
static int hooks;

// ---- the made-up game

// the hook's original: when it ran, and with what
static struct {
    int calls, tags_before; // tags set before it ran
    void *cg, *ps, *cent;
    DObj *obj;
    uint64_t weapon;
} original;
static int tags;
static void game_original(void *cg, void *ps, DObj *obj, uint64_t weapon, void *cent) {
    original.calls++;
    original.tags_before = tags;
    original.cg = cg, original.ps = ps, original.obj = obj, original.weapon = weapon, original.cent = cent;
}
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 4)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    *out_origin_func = (void *)game_original;
    return 0;
}

// dvars: a game side with the kind at +0x1c and a bool's byte at +0x28, a text for a text one
static struct {
    uint8_t game_side[0x40];
    char name[64];
    char text[400];
} dvars[24];
static int text_asks_of_others; // texts asked of dvars that aren't text ones
static void *game_find(uint32_t hash) {
    for (size_t i = 0; i < sizeof(dvars) / sizeof(dvars[0]); i++)
        if (dvars[i].name[0] != '\0' && gsc_hash(dvars[i].name) == hash)
            return &dvars[i];
    return NULL;
}
static const char *game_string(void *dvar) {
    uint32_t type;
    memcpy(&type, (uint8_t *)dvar + 0x1c, sizeof(type));
    if (type != 8)
        text_asks_of_others++;
    return ((typeof(dvars[0]) *)dvar)->text;
}
static typeof(dvars[0]) *dvar_named(const char *name) {
    for (size_t i = 0; i < sizeof(dvars) / sizeof(dvars[0]); i++)
        if (strcmp(dvars[i].name, name) == 0)
            return &dvars[i];
    for (size_t i = 0; i < sizeof(dvars) / sizeof(dvars[0]); i++)
        if (dvars[i].name[0] == '\0') {
            snprintf(dvars[i].name, sizeof(dvars[i].name), "%s", name);
            return &dvars[i];
        }
    return NULL;
}
static void set_text(const char *name, const char *text) {
    typeof(dvars[0]) *dvar = dvar_named(name);
    uint32_t type = 8;
    memcpy(dvar->game_side + 0x1c, &type, sizeof(type));
    snprintf(dvar->text, sizeof(dvar->text), "%s", text);
}
static void set_bool(const char *name, bool value) {
    typeof(dvars[0]) *dvar = dvar_named(name);
    uint32_t type = 1;
    memcpy(dvar->game_side + 0x1c, &type, sizeof(type));
    dvar->game_side[0x28] = value;
}
static void set_kind(const char *name, uint32_t type) {
    typeof(dvars[0]) *dvar = dvar_named(name);
    memcpy(dvar->game_side + 0x1c, &type, sizeof(type));
}
static void remove_dvar(const char *name) {
    for (size_t i = 0; i < sizeof(dvars) / sizeof(dvars[0]); i++)
        if (strcmp(dvars[i].name, name) == 0)
            memset(&dvars[i], 0, sizeof(dvars[i]));
}
static void remove_dvars(void) {
    memset(dvars, 0, sizeof(dvars));
}

// the thread, the front end
static bool main_thread = true, ui_level;
static int thread_asks, ui_asks;
static bool game_main_thread(void) {
    thread_asks++;
    return main_thread;
}
static bool game_ui_level(void) {
    ui_asks++;
    return ui_level;
}

// a view model's DObj: its local client in the byte at +0x123; its bones
struct DObj {
    uint8_t bytes[0x200];
};
static DObj view_model;
static int game_local_client(const DObj *obj) {
    return (int8_t)obj->bytes[0x123];
}
static const char *const bone_names[] = {
    "tag_origin", "tag_ads", "j_bolt", "tag_cambone", "tag_camera", "tag_fill_light", "tag_flashlight", "tag_gasmask",
    "j_shoulder_le", "tag_weapon_left",
    // a belt of ten bullets, the left hand's gun with three, and three more by other names
    "j_bullet01", "j_bullet02", "j_bullet03", "j_bullet04", "j_bullet05", "j_bullet06", "j_bullet07", "j_bullet08",
    "j_bullet09", "j_bullet10", "j_bullet01_le", "j_bullet02_le", "j_bullet03_le", "j_bullet1", "j_bullet2",
    "j_bullet3" };
#define Bones ((int)(sizeof(bone_names) / sizeof(bone_names[0])))
// the game's strings: a name's id is its place in this list, plus 1
static char strings[64][300];
static int string_count, string_asks;
static uint32_t game_alloc_string(const char *name) {
    string_asks++;
    for (int i = 0; i < string_count; i++)
        if (strcmp(strings[i], name) == 0)
            return (uint32_t)i + 1;
    snprintf(strings[string_count], sizeof(strings[0]), "%s", name);
    return (uint32_t)++string_count;
}
static struct {
    int calls;
    uint16_t index_in; // the guess the last one came with
    int models;
    const DObj *obj;
} bone_asks;
static int game_bone_index(const DObj *obj, uint32_t name, uint16_t *index, int models) {
    bone_asks.calls++;
    bone_asks.index_in = *index, bone_asks.models = models, bone_asks.obj = obj;
    if (*index == 0xffff || name == 0 || name > (uint32_t)string_count)
        return 0;
    for (int i = 0; i < Bones; i++)
        if (strcmp(bone_names[i], strings[name - 1]) == 0) {
            *index = (uint16_t)i;
            return 1;
        }
    *index = 0xffff;
    return 0;
}
// the local tags set: the bone, its translation and angles, in order
static struct {
    int bone;
    float translation[3], angles[3];
    bool all_parts;
    const DObj *obj;
} tag[64];
static void game_set_local_tag(DObj *obj, int32_t *part_bits, int index, const float *translation,
                               const float *angles) {
    if (tags < 64) {
        tag[tags].bone = index, tag[tags].obj = obj;
        memcpy(tag[tags].translation, translation, sizeof(tag[0].translation));
        memcpy(tag[tags].angles, angles, sizeof(tag[0].angles));
        tag[tags].all_parts = true;
        for (int i = 0; i < Part_Bits; i++)
            tag[tags].all_parts &= part_bits[i] == -1;
    }
    tags++;
}
static int bone_of(const char *name) {
    for (int i = 0; i < Bones; i++)
        if (strcmp(bone_names[i], name) == 0)
            return i;
    return -1;
}
static bool tagged(int which, const char *bone, float x, float y, float z, float pitch, float yaw, float roll) {
    return which < tags && which < 64 && tag[which].bone == bone_of(bone) && tag[which].all_parts &&
           tag[which].obj == &view_model && tag[which].translation[0] == x && tag[which].translation[1] == y &&
           tag[which].translation[2] == z && tag[which].angles[0] == pitch && tag[which].angles[1] == yaw &&
           tag[which].angles[2] == roll;
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

// a frame: the game's view model update with the made-up player
static uint8_t a_cg[16], a_ps[16], a_cent[16];
static void frame(void) {
    tags = 0;
    fake_CG_UpdateViewModelDynamicBones(a_cg, a_ps, &view_model, 0x123456789aULL, a_cent);
}
// the move dvars as the mod's HUD makes them, with nothing moved by them
static void neutral_moves(int local_client) {
    static const char *const texts[] = { "tag_cambone 0 0 0 0 0 0", "tag_camera 0 0 0 0 0 0",
                                         "tag_fill_light 0 0 0 0 0 0", "tag_flashlight 0 0 0 0 0 0",
                                         "tag_gasmask 0 0 0 0 0 0" };
    for (int i = 0; i < 5; i++) {
        char name[64];
        snprintf(name, sizeof(name), "viewmodel_p%d_move%d", local_client, i);
        set_text(name, texts[i]);
    }
}
static void forget_moves(void) {
    for (int i = 0; i < Local_Clients; i++)
        aae_viewmodel_entries_clear(&clients[i].dvar_moves);
}
static void write_file(const char *folder, const char *name, const char *text) {
    char path[4096 + 128];
    snprintf(path, sizeof(path), "%s/T7Overcharged/%s", folder, name);
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        failures++;
        return;
    }
    fputs(text, file);
    fclose(file);
}

// ---- T7Overcharged's numbers

static bool number(const char *text, float *value) {
    return aae_viewmodel_number(text, strlen(text), value);
}
static bool number_is(const char *text, float expected) {
    float value = -12345.0f;
    return number(text, &value) && memcmp(&value, &expected, sizeof(value)) == 0;
}
static bool integer_is(const char *text, int expected) {
    int value = -12345;
    return aae_viewmodel_integer(text, strlen(text), &value) && value == expected;
}

static void test_numbers(void) {
    float value = 7.0f;
    // a whole piece: the nearest float
    CHECK(number_is("0", 0.0f) && number_is("-0", -0.0f) && number_is("10", 10.0f) && number_is("-2.5", -2.5f));
    CHECK(number_is("23.450000762939", 23.45f) && number_is("0.1", 0.1f) && number_is("1e-05", 1e-05f));
    CHECK(number_is(".5", 0.5f) && number_is("5.", 5.0f) && number_is("1E3", 1000.0f) && number_is("2e+2", 200.0f));
    CHECK(number_is("16777217", 16777216.0f) && number_is("3.4028234e38", FLT_MAX));
    // what from_chars stops in, std::stof reads the start of
    CHECK(number_is("1.5abc", 1.5f) && number_is("+5", 5.0f) && number_is("1e", 1.0f) && number_is("2e+", 2.0f));
    CHECK(number_is("0x10", 16.0f) && number_is("0x1p3", 8.0f) && number_is("7,5", 7.0f) && number_is("4-4", 4.0f));
    // and its names
    CHECK(number_is("inf", INFINITY) && number_is("-INFINITY", -INFINITY) && number("nan", &value) && isnan(value));
    // nothing of it a number, or a number beyond a float: not one
    value = 7.0f;
    CHECK(!number("abc", &value) && !number("-", &value) && !number(".", &value) && !number("e5", &value));
    CHECK(!number("1e39", &value) && !number("-1e39", &value) && !number("1e-50", &value) && value == 7.0f);
    CHECK(!number("7e-46", &value) && !number("-1e-46", &value) && !number("3.4028236e38", &value) && value == 7.0f);
    // a number below the normal floats is one, as with MSVC (macOS's strtof says ERANGE for it)
    CHECK(number_is("1e-45", 1.40129846e-45f) && number_is("1e-39", 1e-39f) && number_is("-1e-38", -1e-38f) &&
          number_is("1e-45e", 1.40129846e-45f) && number_is("1.17549435e-38", 1.17549435e-38f));
    CHECK(!aae_viewmodel_number("12", 0, &value) && value == 7.0f);
    // only the piece counts
    CHECK(aae_viewmodel_number("12 34", 2, &value) && value == 12.0f);
    CHECK(aae_viewmodel_number("1234", 2, &value) && value == 12.0f);
    // a piece longer than the buffer
    char longer[400];
    memset(longer, '0', sizeof(longer));
    longer[0] = '1', longer[1] = '.';
    longer[sizeof(longer) - 2] = '5', longer[sizeof(longer) - 1] = '\0';
    CHECK(number_is(longer, 1.0f));
    memcpy(longer, "0.", 2);
    CHECK(!number(longer, &value)); // 5e-398: below a float, ERANGE
    longer[0] = 'x';
    CHECK(!number(longer, &value));

    // whole numbers
    CHECK(integer_is("0", 0) && integer_is("12", 12) && integer_is("-3", -3) && integer_is("007", 7));
    CHECK(integer_is("+5", 5) && integer_is("12abc", 12) && integer_is("0x10", 0) && integer_is("1e5", 1));
    CHECK(integer_is("2147483647", INT_MAX) && integer_is("-2147483648", INT_MIN));
    int whole = 9;
    CHECK(!aae_viewmodel_integer("2147483648", 10, &whole) && !aae_viewmodel_integer("-2147483649", 11, &whole));
    CHECK(!aae_viewmodel_integer("abc", 3, &whole) && !aae_viewmodel_integer("", 0, &whole) && whole == 9);
    CHECK(aae_viewmodel_integer("123", 1, &whole) && whole == 1);
    char digits[100];
    memset(digits, '0', sizeof(digits));
    digits[sizeof(digits) - 2] = '4', digits[sizeof(digits) - 1] = '\0';
    CHECK(integer_is(digits, 4));
}

// ---- the dvars' texts

static void test_read(void) {
    aae_viewmodel_move_t move;
    // the mod's gun position: a bone and six numbers, kept as two texts with six significant digits
    CHECK(aae_viewmodel_read_move("tag_ads 23.450000762939 -0.1 1e-05 360 0 0", &move) == Move_Read);
    CHECK(strcmp(move.bone, "tag_ads") == 0 && strcmp(move.position, "23.45 -0.1 1e-05") == 0 &&
          strcmp(move.angles, "360 0 0") == 0);
    CHECK(aae_viewmodel_read_move("j_bolt 1234567 0.000123456789 -0 1e20 -2.5 7", &move) == Move_Read);
    CHECK(strcmp(move.position, "1.23457e+06 0.000123457 -0") == 0 && strcmp(move.angles, "1e+20 -2.5 7") == 0);
    CHECK(aae_viewmodel_read_move("j_bolt 0 0 0 1234567 0.1234567 359.99999", &move) == Move_Read &&
          strcmp(move.angles, "1.23457e+06 0.123457 360") == 0);
    // every kind of white space, before, between and after; pieces after the seventh don't count
    CHECK(aae_viewmodel_read_move(" \t tag_ads\t1\v2\f3\r4\n5  6 \t extra pieces x", &move) == Move_Read);
    CHECK(strcmp(move.bone, "tag_ads") == 0 && strcmp(move.position, "1 2 3") == 0 &&
          strcmp(move.angles, "4 5 6") == 0);
    // fewer than seven pieces: a move without a bone
    CHECK(aae_viewmodel_read_move("tag_ads 1 2 3 4 5", &move) == Move_None && move.bone[0] == '\0' &&
          move.position[0] == '\0');
    CHECK(aae_viewmodel_read_move("1 2 3 4 5 6", &move) == Move_None);
    CHECK(aae_viewmodel_read_move("   ", &move) == Move_None);
    // "skip"
    CHECK(aae_viewmodel_read_move("skip", &move) == Move_None);
    CHECK(aae_viewmodel_read_move("skip 1 2 3 4 5 6", &move) == Move_Read && strcmp(move.bone, "skip") == 0);
    // a '!' or '#' in front isn't read; one elsewhere is part of the bone
    CHECK(aae_viewmodel_read_move("!tag_ads 1 2 3 4 5 6", &move) == Move_Read && strcmp(move.bone, "tag_ads") == 0);
    CHECK(aae_viewmodel_read_move("#j_bolt 1 2 3 4 5 6", &move) == Move_Read && strcmp(move.bone, "j_bolt") == 0);
    CHECK(aae_viewmodel_read_move("! tag_ads 1 2 3 4 5 6", &move) == Move_Read && strcmp(move.bone, "tag_ads") == 0);
    CHECK(aae_viewmodel_read_move("!!x 1 2 3 4 5 6", &move) == Move_Read && strcmp(move.bone, "!x") == 0);
    CHECK(aae_viewmodel_read_move("a!b 1 2 3 4 5 6", &move) == Move_Read && strcmp(move.bone, "a!b") == 0);
    CHECK(aae_viewmodel_read_move("!1 2 3 4 5 6", &move) == Move_None);
    // a number that isn't one: the whole text is unreadable; numbers that only begin like one are read
    CHECK(aae_viewmodel_read_move("tag_ads 1 2 three 4 5 6", &move) == Move_Unreadable);
    CHECK(aae_viewmodel_read_move("tag_ads 1 2 3 4 5 x", &move) == Move_Unreadable);
    CHECK(aae_viewmodel_read_move("tag_ads 1 2 3 1e39 5 6", &move) == Move_Unreadable);
    CHECK(aae_viewmodel_read_move("tag_ads 1.5cm +2 0x10 4deg 5 6", &move) == Move_Read &&
          strcmp(move.position, "1.5 2 16") == 0 && strcmp(move.angles, "4 5 6") == 0);
    // a bone name too long for any model
    char text[400];
    memset(text, 'b', 300);
    snprintf(text + 300, sizeof(text) - 300, " 1 2 3 4 5 6");
    CHECK(aae_viewmodel_read_move(text, &move) == Move_Read && move.bone[0] == '\0' &&
          strcmp(move.position, "1 2 3") == 0);
    memset(text, 'b', 256);
    snprintf(text + 256, sizeof(text) - 256, " 1 2 3 4 5 6");
    CHECK(aae_viewmodel_read_move(text, &move) == Move_Read && move.bone[0] == '\0');
    memset(text, 'b', 255);
    snprintf(text + 255, sizeof(text) - 255, " 1 2 3 4 5 6");
    CHECK(aae_viewmodel_read_move(text, &move) == Move_Read && strlen(move.bone) == 255);

    // which texts switch their move on
    CHECK(aae_viewmodel_text_active("tag_ads 1 2 3 4 5 6") && aae_viewmodel_text_active("skip ") &&
          aae_viewmodel_text_active("x!") && aae_viewmodel_text_active(" !x"));
    CHECK(!aae_viewmodel_text_active("skip") && !aae_viewmodel_text_active("!tag_ads 1 2 3 4 5 6") &&
          !aae_viewmodel_text_active("#x"));

    // the translation and angles: (c, -a, b) and (d, e, f), each text read back whole
    float translation[3], rotation[3];
    CHECK(aae_viewmodel_transform("1 2 3", "4 5 6", translation, rotation));
    CHECK(translation[0] == 3.0f && translation[1] == -1.0f && translation[2] == 2.0f && rotation[0] == 4.0f &&
          rotation[1] == 5.0f && rotation[2] == 6.0f);
    CHECK(aae_viewmodel_transform("0 0 -1000", "0 0 0", translation, rotation) && translation[0] == -1000.0f &&
          translation[1] == 0.0f && signbit(translation[1]) && translation[2] == 0.0f);
    CHECK(aae_viewmodel_transform("1.23457e+06 -0.000123457 inf", "nan 0 -0", translation, rotation));
    CHECK(translation[0] == INFINITY && translation[1] == -1234570.0f && translation[2] == -0.000123457f &&
          isnan(rotation[0]) && rotation[1] == 0.0f && signbit(rotation[2]));
    CHECK(!aae_viewmodel_transform("1 2", "4 5 6", translation, rotation));
    CHECK(!aae_viewmodel_transform("1 2 3", "4 5", translation, rotation));
    CHECK(!aae_viewmodel_transform("1 2 x", "4 5 6", translation, rotation));
    CHECK(aae_viewmodel_transform("1 2 3 9", "4 5 6x", translation, rotation) && translation[0] == 3.0f &&
          rotation[2] == 6.0f);
}

// ---- the moves kept per dvar

static void test_store(void) {
    aae_viewmodel_entries_t entries = { 0 };
    aae_viewmodel_move_t one, two;
    aae_viewmodel_read_move("tag_ads 1 2 3 4 5 6", &one);
    aae_viewmodel_read_move("j_bolt 0 0 -3 0 0 0", &two);
    // a new name: a new entry, at the end
    CHECK(aae_viewmodel_store(&entries, "viewmodel_p0_move0", true, &one) && entries.count == 1);
    CHECK(aae_viewmodel_store(&entries, "viewmodel_p0_move1", true, &two) && entries.count == 2);
    CHECK(strcmp(entries.entries[0].name, "viewmodel_p0_move0") == 0 && entries.entries[0].active &&
          entries.entries[0].has_bone && strcmp(entries.entries[0].move.bone, "tag_ads") == 0);
    CHECK(strcmp(entries.entries[1].move.bone, "j_bolt") == 0);
    // the same name: the same entry, which takes the new move
    CHECK(aae_viewmodel_store(&entries, "viewmodel_p0_move0", true, &two) && entries.count == 2 &&
          strcmp(entries.entries[0].move.bone, "j_bolt") == 0);
    // switched off: the move stays as it was
    CHECK(aae_viewmodel_store(&entries, "viewmodel_p0_move0", false, &one) && !entries.entries[0].active &&
          entries.entries[0].has_bone && strcmp(entries.entries[0].move.bone, "j_bolt") == 0);
    CHECK(aae_viewmodel_store(&entries, "viewmodel_p0_move0", false, NULL) && !entries.entries[0].active &&
          entries.entries[0].has_bone);
    // on again, without a bone
    CHECK(aae_viewmodel_store(&entries, "viewmodel_p0_move0", true, NULL) && entries.entries[0].active &&
          !entries.entries[0].has_bone && entries.entries[0].move.bone[0] == '\0' &&
          entries.entries[0].move.position[0] == '\0');
    // a new one that is off: no move at all
    CHECK(aae_viewmodel_store(&entries, "viewmodel_p0_move2", false, &one) && entries.count == 3 &&
          !entries.entries[2].active && !entries.entries[2].has_bone && entries.entries[2].move.bone[0] == '\0');
    // names are told apart by their first 64 characters only
    char name[80], other[80];
    memset(name, 'n', 70), name[70] = '\0';
    memcpy(other, name, sizeof(other)), other[69] = 'o';
    CHECK(aae_viewmodel_store(&entries, name, true, &one) && entries.count == 4 &&
          strncmp(entries.entries[3].name, name, 64) == 0);
    CHECK(aae_viewmodel_store(&entries, other, true, &two) && entries.count == 4 &&
          strcmp(entries.entries[3].move.bone, "j_bolt") == 0);
    other[63] = 'o';
    CHECK(aae_viewmodel_store(&entries, other, true, &two) && entries.count == 5);
    // many of them
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof(name), "viewmodel_p1_move%d", i);
        CHECK(aae_viewmodel_store(&entries, name, i % 2 == 0, &one));
    }
    CHECK(entries.count == 45 && entries.capacity >= 45 && entries.entries[44].active == false &&
          strcmp(entries.entries[43].name, "viewmodel_p1_move38") == 0 && entries.entries[43].has_bone);
    aae_viewmodel_entries_clear(&entries);
    CHECK(entries.count == 0 && entries.capacity == 0 && entries.entries == NULL);
}

// ---- viewmodel_move.cfg

static struct {
    int count;
    int local_client[16];
    aae_viewmodel_move_t move[16];
} read_back;
static void each_move(void *context, int local_client, const aae_viewmodel_move_t *move) {
    CHECK(context == &read_back);
    if (read_back.count < 16)
        read_back.local_client[read_back.count] = local_client, read_back.move[read_back.count] = *move;
    read_back.count++;
}
static void read_config(const char *text) {
    memset(&read_back, 0, sizeof(read_back));
    aae_viewmodel_read_move_config(text, strlen(text), each_move, &read_back);
}
static bool read_as(int which, int local_client, const char *bone, const char *position, const char *angles) {
    return which < read_back.count && read_back.local_client[which] == local_client &&
           strcmp(read_back.move[which].bone, bone) == 0 && strcmp(read_back.move[which].position, position) == 0 &&
           strcmp(read_back.move[which].angles, angles) == 0;
}

static void test_config(void) {
    // lines with and without the local client, in order
    read_config("tag_ads 1 2 3 4 5 6\nviewmodel_p1 j_bolt 0 0 -3 0 0 0\nviewmodel_p0 tag_camera 7 8 9 1 2 3\n");
    CHECK(read_back.count == 3 && read_as(0, 0, "tag_ads", "1 2 3", "4 5 6") &&
          read_as(1, 1, "j_bolt", "0 0 -3", "0 0 0") && read_as(2, 0, "tag_camera", "7 8 9", "1 2 3"));
    // comments, white space around, Windows line ends, a last line without one, empty lines
    read_config("// the mod's moves\r\n\r\n   tag_ads 1 2 3 4 5 6   // in front\r\n\t\n\vviewmodel_p2 x 1 2 3 4 5 6");
    CHECK(read_back.count == 2 && read_as(0, 0, "tag_ads", "1 2 3", "4 5 6") && read_as(1, 2, "x", "1 2 3", "4 5 6"));
    // a comment begins at the first "//", even inside a piece; only one '\r' goes before the '\n'
    read_config("tag_ads 1 2 3 4 5 6//x\nj_bolt//1 2 3 4 5 6 7\ntag_ads 1 2 3 4 5 6\r\r\n");
    CHECK(read_back.count == 2 && read_as(0, 0, "tag_ads", "1 2 3", "4 5 6") && read_as(1, 0, "tag_ads", "1 2 3", "4 5 6"));
    read_config("a/b 1 2 3 4 5 6 /x\n/ 1 2 3 4 5 6");
    CHECK(read_back.count == 2 && read_as(0, 0, "a/b", "1 2 3", "4 5 6") && read_as(1, 0, "/", "1 2 3", "4 5 6"));
    // ('\r' is white space to the pieces anyway: a line ending in "\r\r" or with one inside reads the same)
    read_config("tag_ads 1 2 3 4 5 6\r\rx");
    CHECK(read_back.count == 1 && read_as(0, 0, "tag_ads", "1 2 3", "4 5 6"));
    // pieces after the eighth don't count; too few, a number that isn't one or a local client that isn't: no move
    read_config("viewmodel_p0 tag_ads 1 2 3 4 5 6 7 8\ntag_ads 1 2 3 4 5\nviewmodel_p0 tag_ads 1 2 3 4 5\n"
                "tag_ads 1 2 3 4 5 six\nviewmodel_px tag_ads 1 2 3 4 5 6\nviewmodel_p tag_ads 1 2 3 4 5 6\n"
                "viewmodel_p99999999999 tag_ads 1 2 3 4 5 6\nviewmodel_p7 tag_ads 1 2 3 4 5 6");
    CHECK(read_back.count == 2 && read_as(0, 0, "tag_ads", "1 2 3", "4 5 6") && read_as(1, 7, "tag_ads", "1 2 3", "4 5 6"));
    // what begins with viewmodel_p is the local client, read as T7Overcharged reads whole numbers
    read_config("viewmodel_p-1 a 1 2 3 4 5 6\nviewmodel_p+3 b 1 2 3 4 5 6\nviewmodel_p2x c 1 2 3 4 5 6\n"
                "viewmodel_player d 1 2 3 4 5 6\nViewmodel_p1 1 2 3 4 5 6\nViewmodel_p1 e 1 2 3 4 5 6");
    CHECK(read_back.count == 4 && read_as(0, -1, "a", "1 2 3", "4 5 6") && read_as(1, 3, "b", "1 2 3", "4 5 6") &&
          read_as(2, 2, "c", "1 2 3", "4 5 6") && read_as(3, 0, "Viewmodel_p1", "1 2 3", "4 5 6"));
    // numbers as the dvars' are
    read_config("tag_ads 23.450000762939 -0.1 1e-05 1.5abc +2 0x10\n");
    CHECK(read_back.count == 1 && read_as(0, 0, "tag_ads", "23.45 -0.1 1e-05", "1.5 2 16"));
    read_config("");
    CHECK(read_back.count == 0);
    // only the length given counts
    memset(&read_back, 0, sizeof(read_back));
    aae_viewmodel_read_move_config("tag_ads 1 2 3 4 5 6\nj_bolt 1 2 3 4 5 6\n", 20, each_move, &read_back);
    CHECK(read_back.count == 1 && read_as(0, 0, "tag_ads", "1 2 3", "4 5 6"));
    memset(&read_back, 0, sizeof(read_back));
    aae_viewmodel_read_move_config("tag_ads 1 2 3 4 5 67", 19, each_move, &read_back);
    CHECK(read_back.count == 1 && read_as(0, 0, "tag_ads", "1 2 3", "4 5 6"));
}

// ---- the hook against the made-up game

static void test_hook(void) {
    set_mod(true);
    neutral_moves(0);
    // the HUD's neutral moves move tags by nothing, in the order of the dvars
    frame();
    CHECK(original.calls == 1 && original.tags_before == 0 && original.cg == a_cg && original.ps == a_ps &&
          original.obj == &view_model && original.weapon == 0x123456789aULL && original.cent == a_cent);
    CHECK(tags == 5 && tagged(0, "tag_cambone", 0, -0.0f, 0, 0, 0, 0) && tagged(1, "tag_camera", 0, 0, 0, 0, 0, 0) &&
          tagged(2, "tag_fill_light", 0, 0, 0, 0, 0, 0) && tagged(3, "tag_flashlight", 0, 0, 0, 0, 0, 0) &&
          tagged(4, "tag_gasmask", 0, 0, 0, 0, 0, 0));
    CHECK(signbit(tag[0].translation[1]) && bone_asks.index_in == 0xfffe && bone_asks.models == -1 &&
          bone_asks.obj == &view_model);
    // the gun position option: "tag_ads <Y - 100> <Z - 100> <X - 100> <Y rotation> <Z rotation> <X rotation>"
    set_text("viewmodel_p0_move0", "tag_ads 20 -5.5 10 3 0 15");
    frame();
    CHECK(tags == 5 && tagged(0, "tag_ads", 10, -20, -5.5f, 3, 0, 15) && tagged(1, "tag_camera", 0, 0, 0, 0, 0, 0));
    // a bone the model doesn't have: no tag
    set_text("viewmodel_p0_move1", "tag_nothing 1 1 1 1 1 1");
    frame();
    CHECK(tags == 4 && tagged(0, "tag_ads", 10, -20, -5.5f, 3, 0, 15) && tagged(1, "tag_fill_light", 0, 0, 0, 0, 0, 0));
    // the numbers keep six digits: what "%g" prints of the Lua's 23.450000762939
    set_text("viewmodel_p0_move1", "j_bolt 23.450000762939 1234567 0 0 0 0");
    frame();
    CHECK(tagged(1, "j_bolt", 0, -23.45f, 1234570.0f, 0, 0, 0));
    // switched off with "skip", '!' or '#': not moved; the move is kept for when it is on again
    set_text("viewmodel_p0_move0", "skip");
    frame();
    CHECK(tags == 4 && tagged(0, "j_bolt", 0, -23.45f, 1234570.0f, 0, 0, 0));
    set_text("viewmodel_p0_move0", "tag_ads");
    frame();
    CHECK(tags == 4 && tagged(0, "j_bolt", 0, -23.45f, 1234570.0f, 0, 0, 0));
    set_text("viewmodel_p0_move0", "tag_ads 1 2 3 4 5 6");
    frame();
    CHECK(tags == 5 && tagged(0, "tag_ads", 3, -1, 2, 4, 5, 6));
    set_text("viewmodel_p0_move0", "!tag_ads 9 9 9 9 9 9");
    frame();
    CHECK(tags == 4 && tagged(0, "j_bolt", 0, -23.45f, 1234570.0f, 0, 0, 0));
    set_text("viewmodel_p0_move0", "#");
    frame();
    CHECK(tags == 4);
    set_text("viewmodel_p0_move0", "tag_cambone 0 0 0 0 0 0");
    frame();
    CHECK(tags == 5 && tagged(0, "tag_cambone", 0, 0, 0, 0, 0, 0));
    // fewer than seven pieces: on, without a bone
    set_text("viewmodel_p0_move0", "tag_ads 1 2");
    frame();
    CHECK(tags == 4 && tagged(0, "j_bolt", 0, -23.45f, 1234570.0f, 0, 0, 0));
    set_text("viewmodel_p0_move0", "!tag_ads 1 2 3 4 5 6");
    frame();
    CHECK(tags == 4);
    // a number that isn't one: the dvar is left out, its move stays as it last was
    set_text("viewmodel_p0_move0", "tag_ads 1 2 3 4 5 6");
    frame();
    set_text("viewmodel_p0_move0", "tag_ads 7 7 7 7 7 seven");
    frame();
    CHECK(tags == 5 && tagged(0, "tag_ads", 3, -1, 2, 4, 5, 6));
    // reading ends at the first dvar that isn't there, or is empty, or isn't a text one; the moves of those and of
    // every one after stay as they last were
    set_text("viewmodel_p0_move2", "tag_flashlight 1 1 1 1 1 1");
    remove_dvar("viewmodel_p0_move1");
    frame();
    CHECK(tags == 5 && tagged(0, "tag_ads", 3, -1, 2, 4, 5, 6) && tagged(1, "j_bolt", 0, -23.45f, 1234570.0f, 0, 0, 0) &&
          tagged(2, "tag_fill_light", 0, 0, 0, 0, 0, 0));
    set_text("viewmodel_p0_move1", "");
    frame();
    CHECK(tags == 5 && tagged(2, "tag_fill_light", 0, 0, 0, 0, 0, 0));
    set_bool("viewmodel_p0_move1", true);
    text_asks_of_others = 0;
    frame();
    CHECK(tags == 5 && tagged(2, "tag_fill_light", 0, 0, 0, 0, 0, 0) && text_asks_of_others == 0);
    set_text("viewmodel_p0_move1", "j_bolt 0 0 0 0 0 0");
    frame();
    CHECK(tags == 5 && tagged(1, "j_bolt", 0, 0, 0, 0, 0, 0) && tagged(2, "tag_flashlight", 1, -1, 1, 1, 1, 1));
    // a sixth and later ones are read as they come
    set_text("viewmodel_p0_move5", "j_shoulder_le 0 0 -100000 0 0 0");
    frame();
    CHECK(tags == 6 && tagged(5, "j_shoulder_le", -100000.0f, -0.0f, 0, 0, 0, 0));
    remove_dvar("viewmodel_p0_move5");

    // another local client's view model: its own dvars and moves
    view_model.bytes[0x123] = 1;
    frame();
    CHECK(tags == 0);
    set_text("viewmodel_p1_move0", "tag_ads 1 0 0 0 0 0");
    frame();
    CHECK(tags == 1 && tagged(0, "tag_ads", 0, -1, 0, 0, 0, 0));
    view_model.bytes[0x123] = 0;
    frame();
    CHECK(tags == 6 && tagged(0, "tag_ads", 3, -1, 2, 4, 5, 6));
    // not a local client's: nothing (and only 0 to 3 are)
    for (int which = -1; which <= 4; which += 5) {
        view_model.bytes[0x123] = (uint8_t)which;
        original.calls = 0;
        frame();
        CHECK(tags == 0 && original.calls == 1);
    }
    view_model.bytes[0x123] = 3;
    frame();
    CHECK(tags == 0);
    set_text("viewmodel_p3_move0", "j_bolt 0 0 1 0 0 0");
    frame();
    CHECK(tags == 1 && tagged(0, "j_bolt", 1, -0.0f, 0, 0, 0, 0));
    char too_long[400];
    memset(too_long, 'b', 300);
    snprintf(too_long + 300, sizeof(too_long) - 300, " 1 2 3 4 5 6");
    set_text("viewmodel_p3_move1", too_long);
    string_asks = 0;
    frame();
    CHECK(tags == 1 && string_asks == 1);
    remove_dvar("viewmodel_p3_move0");
    remove_dvar("viewmodel_p3_move1");
    view_model.bytes[0x123] = 0;

    // viewmodel_disabled, as the game reads a bool
    set_bool("viewmodel_disabled", true);
    original.calls = 0;
    frame();
    CHECK(tags == 0 && original.calls == 1);
    set_bool("viewmodel_disabled", false);
    frame();
    CHECK(tags == 6);
    set_text("viewmodel_disabled", "1");
    frame();
    CHECK(tags == 0);
    set_text("viewmodel_disabled", "12abc");
    frame();
    CHECK(tags == 0);
    set_text("viewmodel_disabled", "0");
    frame();
    CHECK(tags == 6);
    set_text("viewmodel_disabled", "yes");
    frame();
    CHECK(tags == 6);
    set_text("viewmodel_disabled", "0.5");
    frame();
    CHECK(tags == 6);
    set_text("viewmodel_disabled", "-1");
    frame();
    CHECK(tags == 0);
    set_kind("viewmodel_disabled", 5);
    text_asks_of_others = 0;
    frame();
    CHECK(tags == 6 && text_asks_of_others == 0);
    // FrameStats' detail times this work (thread_stats.h): the same bones are set with it on
    thread_stats_init();
    frame();
    CHECK(tags == 6 && text_asks_of_others == 0);
    thread_stats_phases_on = false;
    remove_dvar("viewmodel_disabled");

    // not the main thread, the front end's level, no cg_t or no DObj, the mod without the plugin: only the game's
    original.calls = 0;
    main_thread = false;
    frame();
    CHECK(tags == 0 && original.calls == 1);
    main_thread = true;
    ui_level = true;
    frame();
    CHECK(tags == 0 && original.calls == 2);
    ui_level = false;
    tags = 0, thread_asks = 0;
    fake_CG_UpdateViewModelDynamicBones(NULL, a_ps, &view_model, 1, a_cent);
    fake_CG_UpdateViewModelDynamicBones(a_cg, a_ps, NULL, 1, a_cent);
    CHECK(tags == 0 && original.calls == 4 && original.obj == NULL && thread_asks == 0);
    set_mod(false);
    frame();
    CHECK(tags == 0 && original.calls == 5 && thread_asks == 0);
    set_mod(true);
    frame();
    CHECK(tags == 6 && thread_asks == 1 && ui_asks > 0);
    // the bone's name went through the game's strings each time
    string_asks = 0;
    frame();
    CHECK(string_asks == 6);
    remove_dvars();
    forget_moves();
}

// viewmodel_move.cfg in the mod's T7Overcharged folder: read once, its moves before the dvars'
static void test_config_file(void) {
    set_mod(true);
    move_config_from[0] = '\0'; // as at the game's start: the hook test's first frame read it, when there was none
    write_file(mod_folder, "viewmodel_move.cfg",
               "viewmodel_p0 tag_ads 1 2 3 4 5 6 // the gun\nviewmodel_p1 j_bolt 0 0 -3 0 0 0\ntag_camera 5 0 0 0 0 0\n"
               "viewmodel_p4 j_bolt 1 1 1 1 1 1\nviewmodel_p-1 j_bolt 1 1 1 1 1 1\n");
    set_text("viewmodel_p0_move0", "tag_ads 9 9 9 9 9 9");
    frame();
    CHECK(tags == 3 && tagged(0, "tag_ads", 3, -1, 2, 4, 5, 6) && tagged(1, "tag_camera", 0, -5, 0, 0, 0, 0) &&
          tagged(2, "tag_ads", 9, -9, 9, 9, 9, 9));
    // read once: changing it changes nothing
    write_file(mod_folder, "viewmodel_move.cfg", "j_bolt 1 1 1 1 1 1\n");
    frame();
    CHECK(tags == 3 && tagged(0, "tag_ads", 3, -1, 2, 4, 5, 6));
    // switched off with the dvars
    set_bool("viewmodel_disabled", true);
    frame();
    CHECK(tags == 0);
    remove_dvar("viewmodel_disabled");
    // the second local client's line is its own
    view_model.bytes[0x123] = 1;
    frame();
    CHECK(tags == 1 && tagged(0, "j_bolt", -3, -0.0f, 0, 0, 0, 0));
    view_model.bytes[0x123] = 0;

    // another mod's folder: read from there (and the first one's moves are gone)
    char other[sizeof(mod_folder)], first[sizeof(mod_folder)], command[sizeof(mod_folder) + 64];
    snprintf(first, sizeof(first), "%s", mod_folder);
    snprintf(other, sizeof(other), "%s/other/%s", mod_folder, strrchr(mod_folder, '/') + 1);
    snprintf(command, sizeof(command), "mkdir -p '%s/T7Overcharged/plugins'", other);
    if (system(command) != 0)
        failures++;
    snprintf(command, sizeof(command), "%s/%s", other, Plugin_Path);
    close(open(command, O_CREAT | O_WRONLY, 0644));
    snprintf(mod_folder, sizeof(mod_folder), "%s", other);
    set_mod(true);
    frame();
    CHECK(tags == 1 && tagged(0, "tag_ads", 9, -9, 9, 9, 9, 9));
    write_file(mod_folder, "viewmodel_move.cfg", "j_bolt 1 1 1 1 1 1\n");
    view_model.bytes[0x123] = 1;
    frame();
    CHECK(tags == 0);
    view_model.bytes[0x123] = 0;
    // back to the first: read again, as it is now
    snprintf(mod_folder, sizeof(mod_folder), "%s", first);
    set_mod(true);
    frame();
    CHECK(tags == 2 && tagged(0, "j_bolt", 1, -1, 1, 1, 1, 1) && tagged(1, "tag_ads", 9, -9, 9, 9, 9, 9));
    // without the file: none
    snprintf(command, sizeof(command), "%s/T7Overcharged/viewmodel_move.cfg", other);
    unlink(command);
    snprintf(mod_folder, sizeof(mod_folder), "%s", other);
    set_mod(true);
    frame();
    CHECK(tags == 1 && tagged(0, "tag_ads", 9, -9, 9, 9, 9, 9));
    snprintf(mod_folder, sizeof(mod_folder), "%s", first);
    set_mod(true);
    remove_dvars();
    forget_moves();
}

// ---- viewmodel_hide.cfg and the bullets' rules

static struct {
    int count;
    aae_viewmodel_hide_t hide[16];
} hides_read;
static void each_hide(void *context, const aae_viewmodel_hide_t *hide) {
    CHECK(context == &hides_read);
    if (hides_read.count < 16)
        hides_read.hide[hides_read.count] = *hide;
    hides_read.count++;
}
static void read_hides(const char *text, size_t length) {
    memset(&hides_read, 0, sizeof(hides_read));
    aae_viewmodel_read_hide_config(text, length, each_hide, &hides_read);
}
static bool hide_is(int which, int local_client, const char *prefix, const char *suffix, const char *left, int start,
                    int end, int width) {
    const aae_viewmodel_hide_t *hide = &hides_read.hide[which];
    return which < hides_read.count && hide->local_client == local_client && strcmp(hide->prefix, prefix) == 0 &&
           strcmp(hide->suffix, suffix) == 0 && strcmp(hide->left_suffix, left) == 0 && hide->start == start &&
           hide->end == end && hide->width == width;
}
static bool bone_is(int which, bool left, int number, const char *expected) {
    char name[128];
    aae_viewmodel_bullet_bone(&hides_read.hide[which], left, number, name);
    return strcmp(name, expected) == 0;
}

static void test_hide_config(void) {
    // the mod's own file
    static const char mods[] =
        "viewmodel_hide j_bullet0 \"\" \"\" 1 11 // IW8 CHAIN SAW\r\n"
        "viewmodel_hide j_b_ \"\" \"\" 08 01 // IW8 RPM\r\n"
        "viewmodel_hide j_bullet \"\" \"\" 01 20 // T9 MG82\r\n"
        "viewmodel_hide ammo _jnt \"\" 01 20 // T4 Browning 30CAL\r\n"
        "viewmodel_hide tag_bullet_deplete_sqtl_ _animate1 \"\" 00 20 // T9 RPD\r\n"
        "viewmodel_hide tag_bullet0 _animate \"\" 11 15 // T7 BRM";
    read_hides(mods, strlen(mods));
    CHECK(hides_read.count == 6 && hide_is(0, 0, "j_bullet0", "", "", 1, 11, 1) && hide_is(1, 0, "j_b_", "", "", 8, 1, 2) &&
          hide_is(2, 0, "j_bullet", "", "", 1, 20, 2) && hide_is(3, 0, "ammo", "_jnt", "", 1, 20, 2) &&
          hide_is(4, 0, "tag_bullet_deplete_sqtl_", "_animate1", "", 0, 20, 2) &&
          hide_is(5, 0, "tag_bullet0", "_animate", "", 11, 15, 2));
    // the bones: the number to the width of <start>, the suffix of the hand
    CHECK(bone_is(0, false, 1, "j_bullet01") && bone_is(0, false, 11, "j_bullet011") && bone_is(2, false, 7, "j_bullet07") &&
          bone_is(2, false, 20, "j_bullet20") && bone_is(3, false, 5, "ammo05_jnt") && bone_is(3, true, 5, "ammo05") &&
          bone_is(4, false, 0, "tag_bullet_deplete_sqtl_00_animate1") && bone_is(5, false, 12, "tag_bullet012_animate"));
    CHECK(aae_viewmodel_bullets(&hides_read.hide[0]) == 11 && aae_viewmodel_bullets(&hides_read.hide[1]) == 0 &&
          aae_viewmodel_bullets(&hides_read.hide[4]) == 21);
    // local clients; lines that aren't for one, or are too short, or whose numbers aren't any
    static const char clients_text[] =
        "viewmodel_hide_p1 a \"\" \"\" 1 2\nviewmodel_hide_p+2 b \"\" \"\" 1 2\nviewmodel_hide_p-1 c \"\" \"\" 1 2\n"
        "viewmodel_hide_p3x d \"\" \"\" 1 2\nviewmodel_hide_p e \"\" \"\" 1 2\nviewmodel_hide_px f \"\" \"\" 1 2\n"
        "viewmodel_hidex g \"\" \"\" 1 2\nViewmodel_hide h \"\" \"\" 1 2\nviewmodel_hide i \"\" \"\" 1\n"
        "viewmodel_hide j \"\" \"\" one 2\nviewmodel_hide k \"\" \"\" 1 two\nviewmodel_hide l \"\" \"\" 1 99999999999\n"
        "viewmodel_hid m \"\" \"\" 1 2\n// viewmodel_hide n \"\" \"\" 1 2\nviewmodel_hide o \"\" \"\" +3 1e5 extra pieces\n";
    read_hides(clients_text, strlen(clients_text));
    CHECK(hides_read.count == 5 && hide_is(0, 1, "a", "", "", 1, 2, 1) && hide_is(1, 2, "b", "", "", 1, 2, 1) &&
          hide_is(2, -1, "c", "", "", 1, 2, 1) && hide_is(3, 3, "d", "", "", 1, 2, 1) && hide_is(4, 0, "o", "", "", 3, 1, 1));
    // quotes come off only as a pair around the whole piece; white space of every kind; a comment inside a piece
    static const char quoted[] = "\t viewmodel_hide\v\"x\" \"_s\" \"_l\"\f001\r12 \n"
                                 "viewmodel_hide \" \"\"\" \"a\"b 1 2\nviewmodel_hide p//art \"\" \"\" 1 2\n";
    read_hides(quoted, strlen(quoted));
    CHECK(hides_read.count == 2 && hide_is(0, 0, "x", "_s", "_l", 1, 12, 3) && hide_is(1, 0, "\"", "\"", "\"a\"b", 1, 2, 1));
    CHECK(bone_is(0, false, 7, "x007_s") && bone_is(0, true, 12, "x012_l") && bone_is(0, false, -3, "x-03_s") &&
          bone_is(0, false, 1234, "x1234_s"));
    read_hides("viewmodel_hide a/b c/ /d 1 2 /e\n", 32);
    CHECK(hides_read.count == 1 && hide_is(0, 0, "a/b", "c/", "/d", 1, 2, 1));
    // the width is every digit of <start> as written, counted in a byte
    char text[1200];
    char zeros[400];
    memset(zeros, '0', sizeof(zeros));
    snprintf(text, sizeof(text), "viewmodel_hide w \"\" \"\" %.299s2 4\nviewmodel_hide v \"\" \"\" %.255s1 4\n", zeros, zeros);
    read_hides(text, strlen(text));
    CHECK(hides_read.count == 2 && hide_is(0, 0, "w", "", "", 2, 4, 44) && hide_is(1, 0, "v", "", "", 1, 4, 1));
    read_hides("viewmodel_hide u \"\" \"\" 7-8 9\n", 29);
    CHECK(hides_read.count == 1 && hide_is(0, 0, "u", "", "", 7, 9, 2));
    // a name of 127 characters at most; a '\0' ends a piece's name; only the length given counts
    snprintf(text, sizeof(text), "viewmodel_hide %.200s _suffix \"\" 1 2\n", zeros);
    read_hides(text, strlen(text));
    char name[128];
    aae_viewmodel_bullet_bone(&hides_read.hide[0], false, 2, name);
    CHECK(hides_read.count == 1 && strlen(hides_read.hide[0].prefix) == 127 && strlen(name) == 127);
    static const char with_nul[] = "viewmodel_hide j_b\0ad _s\0x \"\" 1 2\nviewmodel_hide q \"\" \"\" 1 2";
    read_hides(with_nul, sizeof(with_nul) - 1);
    CHECK(hides_read.count == 2 && hide_is(0, 0, "j_b", "_s", "", 1, 2, 1) && hide_is(1, 0, "q", "", "", 1, 2, 1));
    read_hides(with_nul, 33);
    CHECK(hides_read.count == 1 && hide_is(0, 0, "j_b", "_s", "", 1, 2, 1));
    read_hides("", 0);
    CHECK(hides_read.count == 0);

    // how many bullets a line names, in 32 bits
    aae_viewmodel_hide_t hide = { .start = 1, .end = 3 };
    CHECK(aae_viewmodel_bullets(&hide) == 3);
    hide.start = 3, hide.end = 3;
    CHECK(aae_viewmodel_bullets(&hide) == 1);
    hide.start = 4;
    CHECK(aae_viewmodel_bullets(&hide) == 0);
    hide.start = -2, hide.end = 2;
    CHECK(aae_viewmodel_bullets(&hide) == 5);
    hide.start = INT_MIN, hide.end = INT_MAX;
    CHECK(aae_viewmodel_bullets(&hide) == 0);
    hide.start = 1;
    CHECK(aae_viewmodel_bullets(&hide) == INT_MAX);
    hide.start = INT_MAX, hide.end = INT_MIN;
    CHECK(aae_viewmodel_bullets(&hide) == 2 - 0);
    // which are hidden: from the clip's count on, all with an empty clip
    CHECK(aae_viewmodel_bullet_hidden(5, 5) && aae_viewmodel_bullet_hidden(12, 5) && !aae_viewmodel_bullet_hidden(4, 5));
    CHECK(aae_viewmodel_bullet_hidden(0, 0) && aae_viewmodel_bullet_hidden(-1, 0) && aae_viewmodel_bullet_hidden(20, 0));
    CHECK(!aae_viewmodel_bullet_hidden(-1, 3) && aae_viewmodel_bullet_hidden(3, -2) && !aae_viewmodel_bullet_hidden(0, 1));
}

// ---- the bullets in the made-up game

// the made-up player's clips: the gun in hand's, and the second gun's of a dual wield one
static int clip_right = 30, clip_left = 30, ammo_asks;
static bool dual_wield;
static const void *ammo_ps;
#define Weapon_Right 0x123456789aULL
#define Weapon_Left 0x5555ULL
static int game_ammo(void *ps, uint64_t weapon) {
    ammo_asks++;
    ammo_ps = ps;
    return weapon == Weapon_Right ? clip_right : weapon == Weapon_Left ? clip_left : -99;
}
static bool game_dual_wield(uint64_t weapon) {
    return weapon == Weapon_Right && dual_wield;
}
static uint64_t game_dual_wield_weapon(uint64_t weapon) {
    return weapon == Weapon_Right ? Weapon_Left : 0;
}
static bool hidden(int which, const char *bone) {
    return tagged(which, bone, -1000.0f, -0.0f, 0, 0, 0, 0) && signbit(tag[which].translation[1]);
}

static void test_bullets(void) {
    set_mod(true);
    gun_position = false, bullet_depletion = true;
    hide_config_from[0] = '\0';
    // (no viewmodel_move.cfg any more, read anew)
    char path[sizeof(mod_folder) + 64];
    snprintf(path, sizeof(path), "%s/T7Overcharged/viewmodel_move.cfg", mod_folder);
    unlink(path);
    move_config_from[0] = '\0';
    write_file(mod_folder, "viewmodel_hide.cfg",
               "// the belt\nviewmodel_hide j_bullet \"\" _le 01 12 // twelve, the left hand's own\n"
               "viewmodel_hide j_b_ \"\" \"\" 08 01\nviewmodel_hide_p1 j_bullet \"\" \"\" 1 3\n"
               "viewmodel_hide_p4 j_bullet \"\" \"\" 1 3\nviewmodel_hide_p-1 j_bullet \"\" \"\" 1 3\n");
    neutral_moves(0);
    // a full clip of 30: none of the twelve
    clip_right = 30, ammo_asks = 0, string_asks = 0;
    frame();
    CHECK(tags == 0 && ammo_asks == 1 && ammo_ps == a_ps && string_asks == 0 && original.calls > 0);
    // 12: the twelfth goes, which this belt doesn't have
    clip_right = 12;
    frame();
    CHECK(tags == 0 && string_asks == 1 && strcmp(strings[string_count - 1], "j_bullet12") == 0);
    // 5: the fifth to the twelfth, those of them the model has, in order
    clip_right = 5;
    frame();
    CHECK(tags == 6 && hidden(0, "j_bullet05") && hidden(1, "j_bullet06") && hidden(5, "j_bullet10"));
    // empty: all of them
    clip_right = 0;
    frame();
    CHECK(tags == 10 && hidden(0, "j_bullet01") && hidden(9, "j_bullet10"));
    clip_right = -4;
    frame();
    CHECK(tags == 10);
    // behind the moves
    gun_position = true;
    set_text("viewmodel_p0_move0", "tag_ads 1 2 3 4 5 6");
    clip_right = 9;
    frame();
    CHECK(tags == 7 && tagged(0, "tag_ads", 3, -1, 2, 4, 5, 6) && tagged(4, "tag_gasmask", 0, -0.0f, 0, 0, 0, 0) &&
          hidden(5, "j_bullet09") && hidden(6, "j_bullet10"));
    gun_position = false;
    // a dual wield weapon: the second gun with its own clip and the left hand's bones
    dual_wield = true;
    clip_left = 2;
    frame();
    CHECK(tags == 4 && hidden(0, "j_bullet09") && hidden(1, "j_bullet10") && hidden(2, "j_bullet02_le") &&
          hidden(3, "j_bullet03_le"));
    clip_left = 3, clip_right = 30;
    frame();
    CHECK(tags == 1 && hidden(0, "j_bullet03_le"));
    dual_wield = false;
    // bullet_depletion_disabled, as the game reads a bool
    clip_right = 8;
    set_bool("bullet_depletion_disabled", true);
    ammo_asks = 0;
    frame();
    CHECK(tags == 0 && ammo_asks == 0);
    set_text("bullet_depletion_disabled", "1");
    frame();
    CHECK(tags == 0);
    set_text("bullet_depletion_disabled", "0");
    frame();
    CHECK(tags == 3 && hidden(0, "j_bullet08"));
    set_bool("bullet_depletion_disabled", false);
    frame();
    CHECK(tags == 3);
    remove_dvar("bullet_depletion_disabled");
    // the second local client's line
    view_model.bytes[0x123] = 1;
    clip_right = 2;
    frame();
    CHECK(tags == 2 && hidden(0, "j_bullet2") && hidden(1, "j_bullet3"));
    view_model.bytes[0x123] = 0;
    // read once
    write_file(mod_folder, "viewmodel_hide.cfg", "viewmodel_hide j_bullet0 \"\" \"\" 1 3\n");
    clip_right = 0;
    frame();
    CHECK(tags == 10);
    // without the mod, off the main thread, or in the front end: nothing
    set_mod(false);
    frame();
    CHECK(tags == 0);
    set_mod(true);
    main_thread = false;
    frame();
    CHECK(tags == 0);
    main_thread = true;
    ui_level = true;
    frame();
    CHECK(tags == 0);
    ui_level = false;
    // with the gun position on as well and this off: only the moves
    gun_position = true, bullet_depletion = false;
    frame();
    CHECK(tags == 5 && tagged(0, "tag_ads", 3, -1, 2, 4, 5, 6));
    // another mod's folder: its file
    char other[sizeof(mod_folder)], first[sizeof(mod_folder)];
    snprintf(first, sizeof(first), "%s", mod_folder);
    snprintf(other, sizeof(other), "%s/other/%s", mod_folder, strrchr(mod_folder, '/') + 1);
    snprintf(mod_folder, sizeof(mod_folder), "%s", other);
    set_mod(true);
    gun_position = false, bullet_depletion = true;
    frame();
    CHECK(tags == 0);
    view_model.bytes[0x123] = 1;
    frame();
    CHECK(tags == 0);
    view_model.bytes[0x123] = 0;
    // back: read again, as it is now (its first three, which are all hidden with an empty clip)
    snprintf(mod_folder, sizeof(mod_folder), "%s", first);
    set_mod(true);
    frame();
    CHECK(tags == 3 && hidden(0, "j_bullet01") && hidden(2, "j_bullet03") && strcmp(hide_config_from, first) == 0);
    remove_dvars();
    forget_moves();
    gun_position = true, bullet_depletion = false;
}

static void capture_begin(void);
static void capture_end(void);

// a game whose DObjSetLocalTag writes into the numbers it was handed (the real one only reads them)
static void game_set_local_tag_scribbles(DObj *obj, int32_t *part_bits, int index, const float *translation,
                                         const float *angles) {
    game_set_local_tag(obj, part_bits, index, translation, angles);
    ((float *)translation)[0] = 555.0f, ((float *)angles)[2] = 666.0f;
}

// A hidden bullet's move is read from the two texts once, when the piece starts, not for every bullet; each bullet
// gets the same numbers as reading them would give, and the game a copy of its own.
static void test_hide_move(void) {
    float translation[3], rotation[3];
    CHECK(aae_viewmodel_transform(Hide_Position, Hide_Angles, translation, rotation));
    hide_move.read = false;
    memset(hide_move.translation, 0, sizeof(hide_move.translation));
    capture_begin();
    aae_viewmodel_init(0x100000000, 0, 1);
    capture_end();
    CHECK(hide_move.read && memcmp(hide_move.translation, translation, sizeof(translation)) == 0 &&
          memcmp(hide_move.rotation, rotation, sizeof(rotation)) == 0 && hide_move.translation[0] == -1000.0f &&
          signbit(hide_move.translation[1]) && hide_move.rotation[2] == 0.0f);
    // the made-up game again (init pointed everything at the real game's addresses)
    loaded_mod = loaded_name;
    Dvar_FindByHash = game_find;
    Dvar_GetString = game_string;
    Sys_IsMainThread = game_main_thread;
    Com_IsRunningUILevel = game_ui_level;
    DObjGetLocalClientIndex = game_local_client;
    GScr_AllocString = game_alloc_string;
    DObjGetBoneIndex = game_bone_index;
    DObjSetLocalTag = game_set_local_tag;
    BG_GetAmmoInClip = game_ammo;
    BG_IsDualWield = game_dual_wield;
    BG_GetDualWieldWeapon = game_dual_wield_weapon;
    set_mod(true);
    gun_position = false, bullet_depletion = true;
    write_file(mod_folder, "viewmodel_hide.cfg", "viewmodel_hide j_bullet \"\" \"\" 01 12\n");
    hide_config_from[0] = '\0';
    clip_right = 5;
    frame();
    CHECK(tags == 6 && hidden(0, "j_bullet05") && hidden(5, "j_bullet10"));
    // the numbers kept are the ones every bullet gets: the texts aren't read again
    const float kept_translation[3] = { 7, 8, 9 }, kept_rotation[3] = { 10, 11, 12 };
    memcpy(hide_move.translation, kept_translation, sizeof(kept_translation));
    memcpy(hide_move.rotation, kept_rotation, sizeof(kept_rotation));
    frame();
    CHECK(tags == 6 && tagged(0, "j_bullet05", 7, 8, 9, 10, 11, 12) && tagged(5, "j_bullet10", 7, 8, 9, 10, 11, 12));
    // a game that wrote into what it was handed changes neither the next bullet's numbers nor the kept ones
    DObjSetLocalTag = game_set_local_tag_scribbles;
    frame();
    CHECK(tags == 6 && tagged(0, "j_bullet05", 7, 8, 9, 10, 11, 12) && tagged(5, "j_bullet10", 7, 8, 9, 10, 11, 12) &&
          memcmp(hide_move.translation, kept_translation, sizeof(kept_translation)) == 0 &&
          memcmp(hide_move.rotation, kept_rotation, sizeof(kept_rotation)) == 0);
    DObjSetLocalTag = game_set_local_tag;
    // not read (it always is): each bullet reads the texts itself, with the same tags as the kept numbers give
    hide_move.read = false;
    frame();
    CHECK(tags == 6 && hidden(0, "j_bullet05") && hidden(1, "j_bullet06") && hidden(5, "j_bullet10"));
    read_hide_move();
    CHECK(hide_move.read);
    frame();
    CHECK(tags == 6 && hidden(0, "j_bullet05") && hidden(1, "j_bullet06") && hidden(5, "j_bullet10"));
    // the moves of the gun position still read their own texts (no viewmodel_move.cfg: only the dvars' moves)
    char path[sizeof(mod_folder) + 64];
    snprintf(path, sizeof(path), "%s/T7Overcharged/viewmodel_move.cfg", mod_folder);
    unlink(path);
    move_config_from[0] = '\0';
    gun_position = true;
    neutral_moves(0);
    set_text("viewmodel_p0_move0", "tag_ads 1 2 3 4 5 6");
    frame();
    CHECK(tags == 11 && tagged(0, "tag_ads", 3, -1, 2, 4, 5, 6) && hidden(5, "j_bullet05") &&
          hidden(10, "j_bullet10"));
    remove_dvars();
    forget_moves();
    gun_position = true, bullet_depletion = false;
}

// ---- the debug lines (BO3MACFIX_VIEWMODELDEBUG=1), which the checks in the game read

// what the code prints between the two calls
static char printed[16384];
static int saved_stdout = -1;
static char capture_path[4096 + 64];
static void capture_begin(void) {
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    int file = open(capture_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    dup2(file, STDOUT_FILENO);
    close(file);
}
static void capture_end(void) {
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    memset(printed, 0, sizeof(printed));
    FILE *file = fopen(capture_path, "rb");
    if (file != NULL) {
        size_t length = fread(printed, 1, sizeof(printed) - 1, file);
        printed[length] = '\0';
        fclose(file);
    }
}
static int times_printed(const char *line) {
    int count = 0;
    for (const char *at = printed; (at = strstr(at, line)) != NULL; at++)
        count++;
    return count;
}

static void test_debug(void) {
    set_mod(true);
    remove_dvars();
    forget_moves();
    gun_position = true, bullet_depletion = true;
    clip_right = 5, dual_wield = false;
    hide_config_from[0] = '\0', move_config_from[0] = '\0';
    write_file(mod_folder, "viewmodel_hide.cfg", "viewmodel_hide j_bullet \"\" _le 01 12\nviewmodel_hide x \"\" \"\" 1 1\n");
    write_file(mod_folder, "viewmodel_move.cfg", "j_bolt 0 0 -2 0 0 0\n");
    set_text("viewmodel_p0_move0", "tag_ads 6 -4 0 0 0 10");
    set_text("viewmodel_p0_move1", "tag_nothing 1 1 1 1 1 1");
    // off: not a line
    debug = 0;
    capture_begin();
    frame();
    capture_end();
    CHECK(printed[0] == '\0' && tags > 0);
    hide_config_from[0] = '\0', move_config_from[0] = '\0';
    // on: each thing once, when it is new
    debug = 1;
    capture_begin();
    frame();
    frame();
    capture_end();
    char line[sizeof(mod_folder) + 128];
    snprintf(line, sizeof(line), "view model: %s/T7Overcharged/viewmodel_move.cfg: 1 moves for local client 0\n",
             mod_folder);
    CHECK(times_printed(line) == 1);
    snprintf(line, sizeof(line), "view model: %s/T7Overcharged/viewmodel_hide.cfg: 2 lines of bullets for local client 0\n",
             mod_folder);
    CHECK(times_printed(line) == 1);
    CHECK(times_printed("view model: viewmodel_p0_move0 is \"tag_ads 6 -4 0 0 0 10\"\n") == 1);
    CHECK(times_printed("view model: viewmodel_p0_move1 is \"tag_nothing 1 1 1 1 1 1\"\n") == 1);
    CHECK(times_printed("view model: local client 0: j_bolt (bone 2) moved by -2 -0 0, turned 0 0 0\n") == 1);
    CHECK(times_printed("view model: local client 0: tag_ads (bone 1) moved by 0 -6 -4, turned 0 0 10\n") == 1);
    CHECK(times_printed("view model: local client 0: tag_nothing is no bone of the view model\n") == 1);
    CHECK(times_printed("view model: bullet_depletion_disabled is 0\n") == 1);
    CHECK(times_printed("view model: local client 0, right hand: 5 in the clip: hides 6 bullet bones (j_bullet05 to "
                        "j_bullet10); 2 more of its listed ones are no bones of this view model\n") == 1);
    // changes
    capture_begin();
    clip_right = 30;
    set_text("viewmodel_p0_move0", "tag_ads 6 -4 0 0 0 11");
    set_bool("bullet_depletion_disabled", true);
    frame();
    set_bool("bullet_depletion_disabled", false);
    dual_wield = true, clip_left = 0;
    frame();
    set_text("viewmodel_p0_move1", "");
    frame();
    frame();
    capture_end();
    CHECK(times_printed("view model: viewmodel_p0_move0 is \"tag_ads 6 -4 0 0 0 11\"\n") == 1);
    CHECK(times_printed("view model: local client 0: tag_ads (bone 1) moved by 0 -6 -4, turned 0 0 11\n") == 1);
    CHECK(times_printed("view model: bullet_depletion_disabled is 1: no bullet is hidden\n") == 1);
    CHECK(times_printed("view model: bullet_depletion_disabled is 0\n") == 1);
    CHECK(times_printed("view model: local client 0, right hand: 30 in the clip: hides 0 bullet bones (- to -); 0 more "
                        "of its listed ones are no bones of this view model\n") == 1);
    CHECK(times_printed("view model: local client 0, left hand: 0 in the clip: hides 3 bullet bones (j_bullet01_le to "
                        "j_bullet03_le); 10 more of its listed ones are no bones of this view model\n") == 1);
    CHECK(times_printed("view model: viewmodel_p0_move1 is empty: the reading ends there, and the moves kept from it "
                        "and the dvars after it stay as they were\n") == 1);
    CHECK(times_printed("view model: viewmodel_p0_move1 is \"") == 0);
    debug = 0;
    char path[sizeof(mod_folder) + 64];
    snprintf(path, sizeof(path), "%s/T7Overcharged/viewmodel_move.cfg", mod_folder);
    unlink(path);
    remove_dvars();
    forget_moves();
    dual_wield = false;
    gun_position = true, bullet_depletion = false;
}

static void test_init(void) {
    const uint64_t base = 0x100000000;
    aae_viewmodel_init(base, 0, 0);
    CHECK(hooks == 0 && !gun_position && !bullet_depletion);
    // either setting is enough for the hook, and each is kept and says so
    for (int which = 1; which <= 3; which++) {
        hooks = 0;
        capture_begin();
        aae_viewmodel_init(base, which & 1 ? 7 : 0, which & 2 ? 9 : 0);
        capture_end();
        CHECK(hooks == 1 && gun_position == ((which & 1) != 0) && bullet_depletion == ((which & 2) != 0));
        CHECK(times_printed("view model: AAEGunPosition on: ") == ((which & 1) != 0) &&
              times_printed("view model: AAEBulletDepletion on: ") == ((which & 2) != 0));
    }
    CHECK(hooks == 1 && hooked[0].address == (void *)(base + ADDR_CG_UpdateViewModelDynamicBones) &&
          hooked[0].replacement == (void *)fake_CG_UpdateViewModelDynamicBones &&
          hooked[0].original == (void **)&orig_CG_UpdateViewModelDynamicBones);
    CHECK(BG_GetAmmoInClip == (void *)(base + ADDR_BG_GetAmmoInClip) &&
          BG_IsDualWield == (void *)(base + ADDR_BG_IsDualWield) &&
          BG_GetDualWieldWeapon == (void *)(base + ADDR_BG_GetDualWieldWeapon) && ADDR_BG_GetAmmoInClip == 0xf5326a &&
          ADDR_BG_IsDualWield == 0x1382008 && ADDR_BG_GetDualWieldWeapon == 0x138472c);
    CHECK(Dvar_FindByHash == (void *)(base + ADDR_Dvar_FindByHash) &&
          Dvar_GetString == (void *)(base + ADDR_Dvar_GetString) &&
          Sys_IsMainThread == (void *)(base + ADDR_Sys_IsMainThread) &&
          Com_IsRunningUILevel == (void *)(base + ADDR_Com_IsRunningUILevel) &&
          DObjGetLocalClientIndex == (void *)(base + ADDR_DObjGetLocalClientIndex) &&
          GScr_AllocString == (void *)(base + ADDR_GScr_AllocString) &&
          DObjGetBoneIndex == (void *)(base + ADDR_DObjGetBoneIndex) &&
          DObjSetLocalTag == (void *)(base + ADDR_DObjSetLocalTag) &&
          loaded_mod == (const char *)(base + ADDR_Mods_LoadedName));
    // the places, as they were read from the game's code
    CHECK(ADDR_CG_UpdateViewModelDynamicBones == 0xd37a72 && ADDR_DObjGetBoneIndex == 0xf74d4f &&
          ADDR_DObjSetLocalTag == 0xf74879 && ADDR_DObjGetLocalClientIndex == 0xf94a1b &&
          ADDR_GScr_AllocString == 0x1172afc && ADDR_Sys_IsMainThread == 0xd05c19 &&
          ADDR_Com_IsRunningUILevel == 0xe71737);
    CHECK(Local_Clients == 4 && Dvar_Type_Bool == 1 && Dvar_Type_String == 8 && dvar_Type == 0x1c &&
          dvar_Value == 0x28 && Part_Bits * 4 == 0x30);
    CHECK(strcmp(Hide_Position, "0 0 -1000") == 0 && strcmp(Hide_Angles, "0 0 0") == 0 &&
          strcmp(Config_Folder, "T7Overcharged") == 0 && strcmp(Hide_Config, "viewmodel_hide.cfg") == 0 &&
          strcmp(Move_Config, "viewmodel_move.cfg") == 0);
}

int main(void) {
    char template[] = "/tmp/bo3macfix-viewmodel-test-XXXXXX";
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
    snprintf(capture_path, sizeof(capture_path), "%s/printed.txt", template);

    test_numbers();
    test_read();
    test_store();
    test_config();
    test_hide_config();
    test_init();
    loaded_mod = loaded_name;
    Dvar_FindByHash = game_find;
    Dvar_GetString = game_string;
    Sys_IsMainThread = game_main_thread;
    Com_IsRunningUILevel = game_ui_level;
    DObjGetLocalClientIndex = game_local_client;
    GScr_AllocString = game_alloc_string;
    DObjGetBoneIndex = game_bone_index;
    DObjSetLocalTag = game_set_local_tag;
    BG_GetAmmoInClip = game_ammo;
    BG_IsDualWield = game_dual_wield;
    BG_GetDualWieldWeapon = game_dual_wield_weapon;
    gun_position = true, bullet_depletion = false;
    test_hook();
    test_config_file();
    test_bullets();
    snprintf(capture_path, sizeof(capture_path), "%s/printed.txt", template);
    test_debug();
    test_hide_move();

    snprintf(command, sizeof(command), "rm -rf '%s'", template);
    if (system(command) != 0)
        failures++;
    if (failures != 0) {
        printf("test_aae_viewmodel: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_viewmodel: all passed\n");
    return 0;
}
