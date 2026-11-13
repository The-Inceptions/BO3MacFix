// aae_viewmodel.c: T7Overcharged's reading of the dvars and of viewmodel_move.cfg, its rules for the moves of the view
// model's bones, and the hook, against a made-up game.
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
static const char *const bone_names[] = { "tag_origin", "tag_ads", "j_bolt", "tag_cambone", "tag_camera",
                                          "tag_fill_light", "tag_flashlight", "tag_gasmask", "j_shoulder_le",
                                          "tag_weapon_left" };
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
    set_kind("viewmodel_disabled", 5);
    text_asks_of_others = 0;
    frame();
    CHECK(tags == 6 && text_asks_of_others == 0);
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
               "viewmodel_p0 tag_ads 1 2 3 4 5 6 // the gun\nviewmodel_p1 j_bolt 0 0 -3 0 0 0\ntag_camera 5 0 0 0 0 0\n");
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

static void test_init(void) {
    const uint64_t base = 0x100000000;
    aae_viewmodel_init(base, 0);
    CHECK(hooks == 0);
    aae_viewmodel_init(base, 1);
    CHECK(hooks == 1 && hooked[0].address == (void *)(base + ADDR_CG_UpdateViewModelDynamicBones) &&
          hooked[0].replacement == (void *)fake_CG_UpdateViewModelDynamicBones &&
          hooked[0].original == (void **)&orig_CG_UpdateViewModelDynamicBones);
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

    test_numbers();
    test_read();
    test_store();
    test_config();
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
    test_hook();
    test_config_file();

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
