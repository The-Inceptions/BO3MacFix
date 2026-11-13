// aae_vmwidget.c: the plugin's tables, its sums for a widget's point, its two methods and the updates they give an
// element, against a made-up game and Lua stack.
#include <fcntl.h>
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

// a writer on another thread, played between the copy of a bone and the second look at its count
static void (*between_reads)(const void *bone);
#define VMWIDGET_BETWEEN_READS(bone)                                                                                   \
    do {                                                                                                               \
        if (between_reads != NULL)                                                                                     \
            between_reads(bone);                                                                                       \
    } while (0)

// and a reader on another thread, looking while a bone is written: the count must be odd then
static int written_while_odd, written_while_even;
#define VMWIDGET_WHILE_WRITING(bone)                                                                                   \
    do {                                                                                                               \
        if (((bone)->sequence & 1) != 0)                                                                               \
            written_while_odd++;                                                                                       \
        else                                                                                                           \
            written_while_even++;                                                                                      \
    } while (0)

#include "../source/aae_plugin.c"
#include "../source/aae_vmwidget.c"

#include "vmwidget_cases.h"
// one turn the plugin's own update worked out (wip/agents/vua/wine, "one"): the bone at 2000 -200 100 turned by
// (0.3, -0.5, 0.2, 0.8), the offset 10 -5 1.5
#define Odd_Point_X 0x44facb4b
#define Odd_Point_Y 0xc34c8788
#define Odd_Point_Z 0x42d84b4b

// ---- what the code asks of the rest of BO3MacFix
static char mod_folder[4096];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
static struct {
    void *address, *replacement, **original;
} hooked[8];
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 8)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    return 0;
}
static bool was_hooked(uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 8; i++)
        if (hooked[i].address == (void *)(0x100000000 + (uint64_t)place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
}

static bool same_bits(float a, float b) {
    return memcmp(&a, &b, sizeof(a)) == 0;
}
static float from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

// ---- the made-up game: a Lua state with its stack, UI elements and the game's functions

static uint8_t lua_memory[0x100];
static lua_State *const L = (lua_State *)lua_memory;
static lua_State *const other_L = (lua_State *)(lua_memory + 0x80);
static HksObject stack[32];
static uint8_t elements[4][0x1a0];
#define Light_Userdata 2

static void stack_of(lua_State *s, int arguments) {
    HKS_BASE(s) = stack;
    HKS_TOP(s) = stack + arguments;
}
static void put(int argument, uint32_t type, void *pointer) {
    memset(&stack[argument - 1], 0, sizeof(stack[0]));
    stack[argument - 1].t = type;
    stack[argument - 1].v.ptr = pointer;
}
static void put_number(int argument, float number) {
    memset(&stack[argument - 1], 0, sizeof(stack[0]));
    stack[argument - 1].t = HKS_TNUMBER;
    stack[argument - 1].v.number = number;
}
// a call element:method(tag, x, y, z [, function]) with what its arguments are
static void call_args(uint8_t *element, const char *tag, float x, float y, float z) {
    put(1, Light_Userdata, element);
    if (tag != NULL)
        put(2, HKS_TSTRING, (void *)tag);
    else
        put(2, HKS_TNIL, NULL);
    put_number(3, x);
    put_number(4, y);
    put_number(5, z);
}

// what the game's functions were asked
static struct {
    int to_element, tolstring, strings, tables, refs, unrefs, pcalls, generic, projections, lookups, befores;
    int last_index, string_user, string_type, table_array, table_hash, ref_table, pcall_arguments, pcall_results,
        pcall_handler;
    uint32_t string_length;
    size_t *tolstring_length;
    char string_text[64];
    int unref_table[8], unref_reference[8];
    HksObject popped[8];
    HksObject pcall_function, pcall_argument;
    HksObject *pcall_top;
    int generic_client, before_client;
    uint8_t *generic_element, *before_element;
    uint64_t generic_root, generic_fourth, generic_fifth, before_root, before_fourth, before_fifth;
    lua_State *generic_lua, *before_lua;
    float generic_point[3], projected[3];
    int projection_client;
    uint32_t lookup_name, lookup_parts;
    const uint8_t *lookup_model;
} asked;

// the game's answers
static uint32_t string_answer = 0x1234; // 0: no script string
static int next_reference = 3, table_reference_answer, function_reference_answer;
static bool references_answered; // whether the two answers above are used instead of next_reference
static int pcall_answer;
static uint32_t made_type = Hks_Table; // what lua_createtable pushes
static bool made_without_table;
static float screen_answer[2] = { 640.0f, 360.0f };
static bool projection_writes = true;
static uint8_t table_memory[0x40];
static HksObject table_array[4];

static uint8_t *element_answer; // when set, the element whatever the stack holds
static uint8_t *fake_to_element(lua_State *s, int index) {
    asked.to_element++;
    asked.last_index = index;
    if (element_answer != NULL)
        return element_answer;
    HksObject *argument = HKS_BASE(s) + (index - 1);
    if (argument >= HKS_TOP(s) || argument->t != Light_Userdata)
        return NULL;
    return argument->v.ptr;
}
static char converted[4][32];
static const char *fake_tolstring(lua_State *s, HksObject *object, size_t *length) {
    (void)s;
    asked.tolstring++;
    asked.tolstring_length = length;
    if ((object->t & 0xf) == HKS_TSTRING)
        return object->v.ptr;
    if ((object->t & 0xf) != HKS_TNUMBER)
        return NULL;
    // a number becomes its text, there on the stack
    char *text = converted[asked.tolstring % 4];
    snprintf(text, sizeof(converted[0]), "%.14g", (double)object->v.number);
    object->t = HKS_TSTRING;
    object->v.ptr = text;
    return text;
}
static uint32_t fake_string(const char *text, uint32_t user, uint32_t length, int type) {
    asked.strings++;
    snprintf(asked.string_text, sizeof(asked.string_text), "%s", text);
    asked.string_user = (int)user;
    asked.string_length = length;
    asked.string_type = type;
    if (string_answer == 0)
        return 0;
    // a string of its own for each text
    uint32_t hash = 0;
    for (const char *c = text; *c != '\0'; c++)
        hash = hash * 31 + (uint8_t)*c;
    return string_answer + (hash & 0xfff);
}
static void fake_createtable(lua_State *s, int array, int hash) {
    asked.tables++;
    asked.table_array = array;
    asked.table_hash = hash;
    HksObject *top = HKS_TOP(s);
    memset(top, 0, sizeof(*top));
    top->t = made_type;
    top->v.ptr = made_without_table ? NULL : table_memory;
    HKS_TOP(s) = top + 1;
}
static int fake_ref(lua_State *s, int table) {
    asked.ref_table = table;
    HKS_TOP(s)--;
    memcpy(&asked.popped[asked.refs % 8], HKS_TOP(s), sizeof(HksObject));
    asked.refs++;
    if (references_answered)
        return asked.refs == 1 ? function_reference_answer : table_reference_answer;
    return next_reference++;
}
static void fake_unref(lua_State *s, int table, int reference) {
    (void)s;
    asked.unref_table[asked.unrefs % 8] = table;
    asked.unref_reference[asked.unrefs % 8] = reference;
    asked.unrefs++;
}
static void line_update(int local_client, uint8_t *element, uint64_t root, uint64_t fourth, uint64_t fifth,
                        lua_State *s);
static uint8_t *reenter; // the element whose update the made-up function runs once more, from inside the call
static int fake_pcall(lua_State *s, int arguments, int results, int handler) {
    if (reenter != NULL) {
        uint8_t *element = reenter;
        reenter = NULL;
        line_update(0, element, 0, 0, 0, s);
    }
    asked.pcalls++;
    asked.pcall_arguments = arguments;
    asked.pcall_results = results;
    asked.pcall_handler = handler;
    asked.pcall_top = HKS_TOP(s);
    memcpy(&asked.pcall_function, HKS_TOP(s) - 2, sizeof(HksObject));
    memcpy(&asked.pcall_argument, HKS_TOP(s) - 1, sizeof(HksObject));
    // what the game leaves: nothing on success, the error message on failure
    HKS_TOP(s) -= 2;
    if (pcall_answer != 0)
        HKS_TOP(s)++;
    return pcall_answer;
}
static void fake_generic(int local_client, uint8_t *element, uint64_t root, uint64_t fourth, uint64_t fifth,
                         lua_State *s) {
    asked.generic++;
    asked.generic_client = local_client;
    asked.generic_element = element;
    asked.generic_root = root;
    asked.generic_fourth = fourth;
    asked.generic_fifth = fifth;
    asked.generic_lua = s;
    memcpy(asked.generic_point, element + Element_Point, sizeof(asked.generic_point));
}
static void fake_before(int local_client, uint8_t *element, uint64_t root, uint64_t fourth, uint64_t fifth,
                        lua_State *s) {
    asked.befores++;
    asked.before_client = local_client;
    asked.before_element = element;
    asked.before_root = root;
    asked.before_fourth = fourth;
    asked.before_fifth = fifth;
    asked.before_lua = s;
}
static void fake_projection(int local_client, const float world[3], float screen[2]) {
    asked.projections++;
    asked.projection_client = local_client;
    memcpy(asked.projected, world, sizeof(asked.projected));
    if (projection_writes)
        memcpy(screen, screen_answer, sizeof(screen_answer));
}

// ---- made-up models (DObj): a tree, a skeleton, computed bits and bones by name
#define Model_Size 0x200
typedef struct {
    uint8_t bytes[Model_Size];
    float skeleton[8][8]; // 0x20 bytes a bone: rotation, position, a weight
    uint32_t names[8];    // the bone names (script strings) of the model, by index
    int found_answer;     // when not 0, DObjGetBoneIndex answers this instead of looking (and leaves the index)
} model_t;
static model_t models[3];
static uint8_t trees[3][16];

static void set_model(model_t *model, const void *tree, uint16_t bones, bool with_skeleton) {
    const void *skeleton = with_skeleton ? (const void *)model->skeleton : NULL;
    memcpy(model->bytes + Model_Tree, &tree, sizeof(tree));
    memcpy(model->bytes + Model_Skeleton, &skeleton, sizeof(skeleton));
    memcpy(model->bytes + Model_Bones, &bones, sizeof(bones));
}
static void set_computed(model_t *model, int bone, bool computed) {
    uint32_t bits;
    memcpy(&bits, model->bytes + Model_Computed + (size_t)(bone >> 5) * 4, sizeof(bits));
    bits = computed ? bits | (0x80000000u >> (bone & 31)) : bits & ~(0x80000000u >> (bone & 31));
    memcpy(model->bytes + Model_Computed + (size_t)(bone >> 5) * 4, &bits, sizeof(bits));
}
static void set_bone(model_t *model, int bone, uint32_t name, const float rotation[4], const float position[3]) {
    model->names[bone] = name;
    memcpy(model->skeleton[bone], rotation, 4 * sizeof(float));
    memcpy(model->skeleton[bone] + 4, position, 3 * sizeof(float));
    set_computed(model, bone, true);
}
static model_t *model_of(const uint8_t *bytes) {
    for (int i = 0; i < 3; i++)
        if (bytes == models[i].bytes)
            return &models[i];
    return NULL;
}
static int16_t index_seen; // what the index was when DObjGetBoneIndex was asked
static int fake_bone_index(const uint8_t *bytes, uint32_t name, int16_t *index, uint32_t parts) {
    asked.lookups++;
    asked.lookup_model = bytes;
    asked.lookup_name = name;
    asked.lookup_parts = parts;
    index_seen = *index;
    model_t *model = model_of(bytes);
    if (model == NULL)
        return 0;
    if (model->found_answer != 0) {
        *index = (int16_t)model->found_answer;
        return 1;
    }
    for (int16_t i = 0; i < 8; i++) {
        if (model->names[i] == name) {
            *index = i;
            return 1;
        }
    }
    *index = -1;
    return 0;
}

static const float identity[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

// the game's view weapon state and local clients, for the hook
static uint8_t view_states[2 * View_Weapon_State_Size];
static uint8_t *view_states_pointer = view_states;
static int clients_in_game = 1;
static void set_weapon_model(int client, const model_t *model) {
    const uint8_t *bytes = model != NULL ? model->bytes : NULL;
    memcpy(view_states + (size_t)client * View_Weapon_State_Size, &bytes, sizeof(bytes));
}

// everything back to as it was at the start, with the made-up game's functions in place
static void fresh(void) {
    memset(&state, 0, sizeof(state));
    memset(&asked, 0, sizeof(asked));
    memset(elements, 0, sizeof(elements));
    memset(models, 0, sizeof(models));
    memset(table_memory, 0, sizeof(table_memory));
    memset(table_array, 0, sizeof(table_array));
    memset(view_states, 0, sizeof(view_states));
    HksObject *array = table_array;
    uint32_t size = 2;
    memcpy(table_memory + Table_Array, &array, sizeof(array));
    memcpy(table_memory + Table_Array_Size, &size, sizeof(size));
    string_answer = 0x1234;
    next_reference = 3;
    references_answered = false;
    pcall_answer = 0;
    made_type = Hks_Table;
    made_without_table = false;
    screen_answer[0] = 640.0f, screen_answer[1] = 360.0f;
    projection_writes = true;
    between_reads = NULL;
    element_answer = NULL;
    clients_in_game = 1;
    ready = true;
    LUI_ToElement = fake_to_element;
    hks_obj_tolstring = fake_tolstring;
    SL_GetStringOfSize = fake_string;
    hksi_lua_createtable = fake_createtable;
    hksi_luaL_ref = fake_ref;
    hksi_luaL_unref = fake_unref;
    hksi_lua_pcall = fake_pcall;
    LUI_Generic3DUpdate = fake_generic;
    CG_WorldToScreen = fake_projection;
    DObjGetBoneIndex = fake_bone_index;
    view_weapon_states = &view_states_pointer;
    local_client_count = &clients_in_game;
}

// ---- the plugin's tables

static aae_vmwidget_state_t tables; // for the tests of the tables alone

static void test_slots(void) {
    // the plugin's hashes: (name * -0x4f) & 0xff, and of an element's address h = (p >> 13) ^ p, then
    // ((h >> 31) ^ (h >> 4)) & 0x7ff (worked out apart from the code)
    CHECK(aae_vmwidget_tag_slot(1) == 177 && aae_vmwidget_tag_slot(2) == 98 && aae_vmwidget_tag_slot(0x100) == 0 &&
          aae_vmwidget_tag_slot(0x1234) == 244 && aae_vmwidget_tag_slot(0xffffffff) == 79);
    CHECK(aae_vmwidget_element_slot((void *)0x1000) == 256);
    CHECK(aae_vmwidget_element_slot((void *)0x123456789a) == 1414);
    CHECK(aae_vmwidget_element_slot((void *)0x7fff12345678) == 900);
    CHECK(aae_vmwidget_element_slot((void *)0x1119c73c0) == 2032);
    CHECK(aae_vmwidget_element_slot((void *)UINTPTR_MAX) == 0);
}

static void test_tags(void) {
    memset(&tables, 0, sizeof(tables));
    aae_vmwidget_tag_t *a = aae_vmwidget_tag(&tables, 1);
    CHECK(a == &tables.tag[0] && a->name == 1 && tables.tag_count == 1 && tables.tag_slot[177] == 1);
    // a new tag's bones are to be looked for, for every local client; nothing else of it is touched
    for (int client = 0; client < AAE_VMWIDGET_LOCAL_CLIENTS; client++)
        CHECK(a->lookup[client].index == No_Lookup && a->lookup[client].model == NULL && a->bone[client].valid == 0 &&
              a->bone[client].sequence == 0);
    CHECK(aae_vmwidget_tag(&tables, 1) == a && tables.tag_count == 1);
    // the same place: the next free one after it
    aae_vmwidget_tag_t *b = aae_vmwidget_tag(&tables, 1 + 0x100);
    CHECK(b == &tables.tag[1] && tables.tag_slot[178] == 2 && aae_vmwidget_tag(&tables, 0x101) == b);
    // the place after the last wraps to the first
    aae_vmwidget_tag_t *c = aae_vmwidget_tag(&tables, 0xffffffff);
    CHECK(c == &tables.tag[2] && tables.tag_slot[79] == 3);
    uint32_t wraps = 0;
    while (aae_vmwidget_tag_slot(wraps) != 0xff && wraps < 0x10000)
        wraps++;
    aae_vmwidget_tag_t *last = aae_vmwidget_tag(&tables, wraps), *wrapped = aae_vmwidget_tag(&tables, wraps + 0x100);
    CHECK(tables.tag_slot[0xff] == 4 && tables.tag_slot[0] == 5 && last == &tables.tag[3] && wrapped == &tables.tag[4]);
    CHECK(aae_vmwidget_tag(&tables, wraps + 0x100) == wrapped);
    // room for 128: the 129th isn't made, the ones there are still found
    for (uint32_t name = 1000; tables.tag_count < AAE_VMWIDGET_TAGS && name < 2000; name++)
        CHECK(aae_vmwidget_tag(&tables, name) != NULL);
    CHECK(aae_vmwidget_tag(&tables, 999999) == NULL && tables.tag_count == AAE_VMWIDGET_TAGS);
    CHECK(aae_vmwidget_tag(&tables, 1) == a && aae_vmwidget_tag(&tables, 0x101) == b);
}

static void test_records(void) {
    memset(&tables, 0, sizeof(tables));
    uint8_t *element = (uint8_t *)0x1000;
    aae_vmwidget_widget_t *w = aae_vmwidget_widget(&tables, element);
    CHECK(w == &tables.widget[0] && w->element == element && tables.widget_count == 1 && tables.widget_slot[256] == 1);
    CHECK(aae_vmwidget_widget(&tables, element) == w && widget_of(&tables, element, false) == w);
    // looking without making: none, and nothing made
    CHECK(widget_of(&tables, (void *)0x2000, false) == NULL && tables.widget_count == 1);
    // two in one place: the second goes to the next one
    uint8_t *same = (uint8_t *)(0x1000 + ((uint64_t)0x800 << 4)); // h >> 4 grows by 0x800: the same place
    CHECK(aae_vmwidget_element_slot(same) == 256);
    aae_vmwidget_widget_t *w2 = aae_vmwidget_widget(&tables, same);
    CHECK(w2 == &tables.widget[1] && tables.widget_slot[257] == 2 && widget_of(&tables, same, false) == w2);
    // the place after 1023 is 1024: the tables have 2048
    uint8_t *at_1023[2] = { NULL, NULL };
    for (uint64_t address = 0x100000; at_1023[1] == NULL && address < 0x10000000; address += 8) {
        if (aae_vmwidget_element_slot((void *)address) != 1023)
            continue;
        if (at_1023[0] == NULL)
            at_1023[0] = (uint8_t *)address;
        else
            at_1023[1] = (uint8_t *)address;
    }
    aae_vmwidget_widget(&tables, at_1023[0]);
    aae_vmwidget_widget(&tables, at_1023[1]);
    CHECK(at_1023[1] != NULL && tables.widget_slot[1023] == 3 && tables.widget_slot[1024] == 4 &&
          tables.widget_slot[0] == 0);
    CHECK(widget_of(&tables, at_1023[1], false) == &tables.widget[3]);
    // a line starts with no references (-2) and nothing else
    aae_vmwidget_line_t *l = aae_vmwidget_line(&tables, element);
    CHECK(l == &tables.line[0] && l->element == element && l->function_reference == -2 && l->table_reference == -2 &&
          l->lua == NULL && l->before == NULL && l->tag == NULL && l->off == 0 && l->calling == 0 &&
          tables.line_slot[256] == 1 && line_of(&tables, element, false) == l);
    CHECK(line_of(&tables, (void *)0x2000, false) == NULL && tables.line_count == 1);
    // room for 1024 of each
    for (uint64_t address = 0x10000; tables.widget_count < AAE_VMWIDGET_WIDGETS && address < 0x200000; address += 0x1a0)
        CHECK(aae_vmwidget_widget(&tables, (void *)address) != NULL);
    CHECK(aae_vmwidget_widget(&tables, (void *)0x9999990) == NULL && aae_vmwidget_widget(&tables, element) == w);
    for (uint64_t address = 0x10000; tables.line_count < AAE_VMWIDGET_WIDGETS && address < 0x200000; address += 0x1a0)
        CHECK(aae_vmwidget_line(&tables, (void *)address) != NULL);
    CHECK(aae_vmwidget_line(&tables, (void *)0x9999990) == NULL && aae_vmwidget_line(&tables, element) == l);
}

static bool bones_reset(const aae_vmwidget_tag_t *tag, uint32_t sequence) {
    for (int client = 0; client < AAE_VMWIDGET_LOCAL_CLIENTS; client++)
        if (tag->bone[client].sequence != sequence || tag->bone[client].valid != 0 ||
            tag->lookup[client].index != No_Lookup || tag->lookup[client].model != NULL ||
            tag->lookup[client].tree != NULL)
            return false;
    return true;
}
static void make_valid(aae_vmwidget_tag_t *tag) {
    for (int client = 0; client < AAE_VMWIDGET_LOCAL_CLIENTS; client++) {
        tag->bone[client].valid = 1;
        tag->lookup[client] = (aae_vmwidget_lookup_t){ (void *)1, (void *)2, 7 };
    }
}

static void test_move(void) {
    memset(&tables, 0, sizeof(tables));
    aae_vmwidget_tag_t *a = aae_vmwidget_tag(&tables, 10), *b = aae_vmwidget_tag(&tables, 11);
    aae_vmwidget_tag_t *first = NULL, *second = NULL;
    make_valid(a);
    // the first user: what the tag kept is from before, its bit goes on
    aae_vmwidget_move(&tables, &first, a);
    CHECK(first == a && tables.users[0] == 1 && tables.active[0] == 1 && bones_reset(a, 2));
    // a second user leaves it as it is
    make_valid(a);
    aae_vmwidget_move(&tables, &second, a);
    CHECK(second == a && tables.users[0] == 2 && tables.active[0] == 1 && a->bone[0].valid == 1 &&
          a->lookup[3].index == 7 && a->bone[0].sequence == 2);
    // to the same tag: nothing
    aae_vmwidget_move(&tables, &second, a);
    CHECK(tables.users[0] == 2 && a->bone[0].valid == 1);
    // a tag's only user moved to it again: nothing either
    aae_vmwidget_tag_t *c = aae_vmwidget_tag(&tables, 12), *lone = NULL;
    aae_vmwidget_move(&tables, &lone, c);
    make_valid(c);
    aae_vmwidget_move(&tables, &lone, c);
    CHECK(tables.users[2] == 1 && c->bone[2].valid == 1 && c->lookup[2].index == 7 && tables.active[0] == 5);
    aae_vmwidget_move(&tables, &lone, NULL);
    // one moves on: the old one keeps its bit while it has a user
    aae_vmwidget_move(&tables, &first, b);
    CHECK(first == b && tables.users[0] == 1 && tables.users[1] == 1 && tables.active[0] == 3 && bones_reset(b, 2));
    // the last user goes: its bit too
    aae_vmwidget_move(&tables, &second, NULL);
    CHECK(second == NULL && tables.users[0] == 0 && tables.active[0] == 2);
    // a count already at 0 isn't taken below it
    aae_vmwidget_tag_t *stray = a;
    aae_vmwidget_move(&tables, &stray, NULL);
    CHECK(stray == NULL && tables.users[0] == 0 && tables.active[0] == 2);
    // a tag beyond the first 64 has its bit in the second word
    aae_vmwidget_tag_t *far = NULL;
    for (uint32_t name = 100; tables.tag_count < 70 && name < 1000; name++)
        far = aae_vmwidget_tag(&tables, name);
    aae_vmwidget_tag_t *user = NULL;
    aae_vmwidget_move(&tables, &user, far);
    CHECK(tables.users[69] == 1 && tables.active[1] == (uint64_t)1 << 5);
    aae_vmwidget_move(&tables, &user, NULL);
    CHECK(tables.users[69] == 0 && tables.active[1] == 0);
    // its first user again: a reset again (the count goes on by 2)
    make_valid(a);
    aae_vmwidget_move(&tables, &user, a);
    CHECK(bones_reset(a, 4) && tables.active[0] == 3);
}

// ---- the plugin's rules for arguments, bones and points

static void test_offset(void) {
    memset(stack, 0, sizeof(stack));
    put_number(3, 12.5f);
    put_number(4, -1000000.0f);
    put_number(5, 1000000.0f);
    CHECK(aae_vmwidget_offset(stack, stack + 5, 3) == 12.5f);
    CHECK(aae_vmwidget_offset(stack, stack + 5, 4) == -1000000.0f && aae_vmwidget_offset(stack, stack + 5, 5) == 1e6f);
    // only what is on the stack
    CHECK(aae_vmwidget_offset(stack, stack + 4, 5) == 0.0f && aae_vmwidget_offset(stack, stack + 4, 4) == -1e6f);
    CHECK(aae_vmwidget_offset(NULL, stack + 5, 3) == 0.0f && aae_vmwidget_offset(stack, NULL, 3) == 0.0f);
    // the whole type word must be a number's
    stack[2].t = 0x13;
    CHECK(aae_vmwidget_offset(stack, stack + 5, 3) == 0.0f);
    stack[2].t = HKS_TSTRING;
    CHECK(aae_vmwidget_offset(stack, stack + 5, 3) == 0.0f);
    // the next float beyond a million, not a number, endless
    put_number(3, from_bits(0x49742401));
    CHECK(aae_vmwidget_offset(stack, stack + 5, 3) == 0.0f);
    put_number(3, -from_bits(0x49742401));
    CHECK(aae_vmwidget_offset(stack, stack + 5, 3) == 0.0f);
    put_number(3, NAN);
    CHECK(aae_vmwidget_offset(stack, stack + 5, 3) == 0.0f);
    put_number(3, INFINITY);
    CHECK(aae_vmwidget_offset(stack, stack + 5, 3) == 0.0f);
    put_number(3, -0.0f);
    CHECK(same_bits(aae_vmwidget_offset(stack, stack + 5, 3), -0.0f));
}

static aae_vmwidget_tag_t read_tag;
static void a_writer_goes_on(const void *bone) {
    aae_vmwidget_bone_t *b = (aae_vmwidget_bone_t *)bone;
    b->sequence += 2;
}
static void a_writer_starts(const void *bone) {
    aae_vmwidget_bone_t *b = (aae_vmwidget_bone_t *)bone;
    b->sequence += 1;
}
static void a_writer_changes_only_values(const void *bone) {
    aae_vmwidget_bone_t *b = (aae_vmwidget_bone_t *)bone;
    b->position[0] = 99.0f;
}

static void test_read(void) {
    memset(&read_tag, 0, sizeof(read_tag));
    aae_vmwidget_bone_t *bone = &read_tag.bone[2], copy;
    const float position[3] = { 1, 2, 3 }, rotation[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
    memcpy(bone->position, position, sizeof(position));
    memcpy(bone->rotation, rotation, sizeof(rotation));
    bone->sequence = 6;
    bone->valid = 1;
    CHECK(aae_vmwidget_read(&read_tag, 2, &copy) && memcmp(copy.position, position, sizeof(position)) == 0 &&
          memcmp(copy.rotation, rotation, sizeof(rotation)) == 0);
    // another local client's has nothing
    CHECK(!aae_vmwidget_read(&read_tag, 1, &copy));
    // being written, or not valid
    bone->sequence = 7;
    CHECK(!aae_vmwidget_read(&read_tag, 2, &copy));
    bone->sequence = 6, bone->valid = 0;
    CHECK(!aae_vmwidget_read(&read_tag, 2, &copy));
    bone->valid = 1;
    // a writer went on while it was read, or began: a torn copy
    between_reads = a_writer_goes_on;
    CHECK(!aae_vmwidget_read(&read_tag, 2, &copy));
    between_reads = a_writer_starts;
    CHECK(!aae_vmwidget_read(&read_tag, 2, &copy));
    // values changed without the count can't be seen: what was copied before counts
    bone->sequence = 6;
    between_reads = a_writer_changes_only_values;
    CHECK(aae_vmwidget_read(&read_tag, 2, &copy) && copy.position[0] == 1.0f);
    between_reads = NULL;
}

static aae_vmwidget_bone_t bone_at(float px, float py, float pz, float qx, float qy, float qz, float qw) {
    aae_vmwidget_bone_t bone = { 0, 1, { px, py, pz }, { qx, qy, qz, qw } };
    return bone;
}

static void test_point(void) {
    float point[3];
    // no offset: the bone's position as it is, without any test of it
    aae_vmwidget_bone_t far = bone_at(2e7f, NAN, 3.0f, 0, 0, 0, 0);
    const float none[3] = { 0.0f, -0.0f, 0.0f };
    CHECK(aae_vmwidget_point(&far, none, point) && point[0] == 2e7f && isnan(point[1]) && point[2] == 3.0f);
    // the rotation that leaves directions alone: the offset is added as it is
    aae_vmwidget_bone_t still = bone_at(10, 20, 30, 0, 0, 0, 1);
    const float offset[3] = { 1, 2, 3 };
    CHECK(aae_vmwidget_point(&still, offset, point) && point[0] == 11 && point[1] == 22 && point[2] == 33);
    // half a turn about z: x and y turn round
    aae_vmwidget_bone_t half = bone_at(10, 20, 30, 0, 0, 1, 0);
    CHECK(aae_vmwidget_point(&half, offset, point) && point[0] == 9 && point[1] == 18 && point[2] == 33);
    // a longer rotation turns the same way: its length is divided out
    aae_vmwidget_bone_t longer = bone_at(10, 20, 30, 0, 0, 2, 0);
    CHECK(aae_vmwidget_point(&longer, offset, point) && point[0] == 9 && point[1] == 18 && point[2] == 33);
    // half a turn about x
    aae_vmwidget_bone_t about_x = bone_at(0, 0, 0, 1, 0, 0, 0);
    CHECK(aae_vmwidget_point(&about_x, offset, point) && point[0] == 1 && point[1] == -2 && point[2] == -3);
    // a quarter about y, with a length of 4 (the plugin's sums give exact numbers here)
    aae_vmwidget_bone_t quarter = bone_at(0, 0, 0, 0, 2, 0, 2);
    const float along_x[3] = { 1, 0, 0 };
    CHECK(aae_vmwidget_point(&quarter, along_x, point) && point[0] == 0 && point[1] == 0 && point[2] == -1);
    // too far, or not a number: no point
    aae_vmwidget_bone_t edge = bone_at(9999998.0f, 0, 0, 0, 0, 0, 1);
    CHECK(aae_vmwidget_point(&edge, along_x, point) && point[0] == 9999999.0f);
    const float two[3] = { 2, 0, 0 };
    CHECK(!aae_vmwidget_point(&edge, two, point));
    aae_vmwidget_bone_t below = bone_at(0, -9999998.0f, 0, 0, 0, 0, 1);
    const float down[3] = { 0, -2, 0 }, deep[3] = { 0, 0, -2 };
    CHECK(!aae_vmwidget_point(&below, down, point));
    aae_vmwidget_bone_t under = bone_at(0, 0, -9999998.0f, 0, 0, 0, 1);
    CHECK(!aae_vmwidget_point(&under, deep, point) && aae_vmwidget_point(&under, down, point));
    aae_vmwidget_bone_t empty = bone_at(0, 0, 0, 0, 0, 0, 0);
    CHECK(!aae_vmwidget_point(&empty, offset, point));
    // the plugin's own sums, bit for bit, for a few of the cases compared with it (vmwidget_cases.h)
    aae_vmwidget_bone_t odd = bone_at(from_bits(0x44fa0000), from_bits(0xc3480000), from_bits(0x42c80000),
                                      from_bits(0x3e99999a), from_bits(0xbf000000), from_bits(0x3e4ccccd),
                                      from_bits(0x3f4ccccd));
    const float bits_offset[3] = { from_bits(0x41200000), from_bits(0xc0a00000), from_bits(0x3fc00000) };
    CHECK(aae_vmwidget_point(&odd, bits_offset, point) && same_bits(point[0], from_bits(Odd_Point_X)) &&
          same_bits(point[1], from_bits(Odd_Point_Y)) && same_bits(point[2], from_bits(Odd_Point_Z)));
}

static void test_clients(void) {
    CHECK(local_client_kept(0) && local_client_kept(3) && !local_client_kept(4) && !local_client_kept(-1));
}

static void test_screen(void) {
    CHECK(aae_vmwidget_on_screen((float[2]){ 640.0f, 360.0f }));
    CHECK(aae_vmwidget_on_screen((float[2]){ -9999999.0f, 9999999.0f }));
    CHECK(!aae_vmwidget_on_screen((float[2]){ 10000000.0f, 0.0f }));
    CHECK(!aae_vmwidget_on_screen((float[2]){ 0.0f, -10000000.0f }));
    CHECK(!aae_vmwidget_on_screen((float[2]){ NAN, 0.0f }));
    CHECK(!aae_vmwidget_on_screen((float[2]){ 0.0f, INFINITY }));
}

// ---- the weapon model and its bones

// a state with tags 0 and 1 in use (by made-up widgets), tag 2 made but unused
static aae_vmwidget_tag_t *tag0, *tag1, *tag2;
static void tags_in_use(aae_vmwidget_state_t *s) {
    memset(s, 0, sizeof(*s));
    tag0 = aae_vmwidget_tag(s, 0x500);
    tag1 = aae_vmwidget_tag(s, 0x501);
    tag2 = aae_vmwidget_tag(s, 0x502);
    static aae_vmwidget_tag_t *users[2];
    users[0] = users[1] = NULL;
    aae_vmwidget_move(s, &users[0], tag0);
    aae_vmwidget_move(s, &users[1], tag1);
}

static void test_note(void) {
    tags_in_use(&tables);
    set_model(&models[0], trees[0], 8, true);
    set_model(&models[1], trees[1], 8, true);
    make_valid(tag0), make_valid(tag1), make_valid(tag2);
    // a first model: what the active tags kept for this local client is from before
    aae_vmwidget_note(&tables, 1, models[0].bytes);
    CHECK(tables.noted[1].model == models[0].bytes && tables.noted[1].tree == trees[0]);
    CHECK(tag0->bone[1].valid == 0 && tag0->bone[1].sequence == 4 && tag0->lookup[1].index == No_Lookup &&
          tag0->lookup[1].model == NULL && tag1->bone[1].valid == 0);
    // other local clients, and tags nobody uses, are left alone
    CHECK(tag0->bone[0].valid == 1 && tag0->lookup[0].index == 7 && tag2->bone[1].valid == 1 &&
          tag2->lookup[1].index == 7);
    // the same model again: nothing
    make_valid(tag0);
    aae_vmwidget_note(&tables, 1, models[0].bytes);
    CHECK(tag0->bone[1].valid == 1 && tag0->lookup[1].index == 7);
    // the same model with another tree: as a new model
    set_model(&models[0], trees[2], 8, true);
    aae_vmwidget_note(&tables, 1, models[0].bytes);
    CHECK(tag0->bone[1].valid == 0 && tables.noted[1].tree == trees[2]);
    // no model at all is noted as well
    make_valid(tag0);
    aae_vmwidget_note(&tables, 1, NULL);
    CHECK(tables.noted[1].model == NULL && tables.noted[1].tree == NULL && tag0->bone[1].valid == 0);
    // a local client beyond the fourth: nothing
    make_valid(tag0);
    aae_vmwidget_note(&tables, 4, models[1].bytes);
    aae_vmwidget_note(&tables, -1, models[1].bytes);
    CHECK(tag0->bone[1].valid == 1 && tag0->bone[0].valid == 1);
    // a tag beyond the ones made, with its bit on, is left alone
    atomic_fetch_or(&tables.active[0], (uint64_t)1 << 9);
    tables.tag[9].bone[0].valid = 1;
    aae_vmwidget_note(&tables, 0, models[1].bytes);
    CHECK(tables.tag[9].bone[0].valid == 1 && tag0->bone[0].valid == 0);
}

static const float turned[4] = { 0.0f, 0.6f, 0.0f, 0.8f }, at[3] = { 100.0f, -20.0f, 55.5f };
static bool kept_as(const aae_vmwidget_tag_t *tag, int client, const float rotation[4], const float position[3]) {
    const aae_vmwidget_bone_t *bone = &tag->bone[client];
    return bone->valid == 1 && (bone->sequence & 1) == 0 && memcmp(bone->rotation, rotation, 16) == 0 &&
           memcmp(bone->position, position, 12) == 0;
}

static void test_skeleton(void) {
    tags_in_use(&tables);
    model_t *model = &models[0];
    memset(models, 0, sizeof(models));
    set_model(model, trees[0], 8, true);
    set_bone(model, 3, 0x500, turned, at);
    set_bone(model, 5, 0x502, turned, at);
    aae_vmwidget_note(&tables, 0, model->bytes);
    memset(&asked, 0, sizeof(asked));
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    // tag 0 is found at bone 3 and kept, after the count was odd while it was written
    CHECK(kept_as(tag0, 0, turned, at) && tag0->bone[0].sequence == 6 && tag0->lookup[0].index == 3 &&
          tag0->lookup[0].model == model->bytes && tag0->lookup[0].tree == trees[0]);
    // the plugin's question: the tag's string, an index of -2, all parts
    CHECK(asked.lookups == 2 && asked.lookup_parts == 0xffff && index_seen == -2);
    // tag 1 isn't in the model: -1, and what it kept isn't valid; tag 2 has no user and isn't looked for
    CHECK(tag1->lookup[0].index == -1 && tag1->bone[0].valid == 0 && tag1->bone[0].sequence == 6 &&
          tag2->lookup[0].index == No_Lookup && tag2->bone[0].valid == 0);
    // the next time the bones are taken from where they were found
    const float moved[3] = { 1.0f, 2.0f, 3.0f };
    set_bone(model, 3, 0x500, turned, moved);
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(asked.lookups == 2 && kept_as(tag0, 0, turned, moved) && tag0->bone[0].sequence == 8);
    // a bone the game hasn't computed this time is left as it was kept
    set_computed(model, 3, false);
    set_bone(model, 3, 0x500, turned, at);
    set_computed(model, 3, false);
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(kept_as(tag0, 0, turned, moved));
    set_computed(model, 3, true);
    // a model that isn't a local client's weapon model: nothing
    set_model(&models[1], trees[1], 8, true);
    set_bone(&models[1], 3, 0x500, identity, at);
    aae_vmwidget_skeleton(&tables, models[1].bytes, fake_bone_index);
    CHECK(kept_as(tag0, 0, turned, moved) && asked.lookups == 2);
    // the second local client's: its own bones
    aae_vmwidget_note(&tables, 1, models[1].bytes);
    aae_vmwidget_skeleton(&tables, models[1].bytes, fake_bone_index);
    CHECK(kept_as(tag0, 1, identity, at) && kept_as(tag0, 0, turned, moved));
    // a model both clients have: the first one's only
    aae_vmwidget_note(&tables, 2, model->bytes);
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(tag0->bone[2].valid == 0 && kept_as(tag0, 0, turned, at));
    aae_vmwidget_note(&tables, 2, NULL);
    // its tree changed since it was noted: nothing (the note hasn't seen the change yet)
    set_model(model, trees[2], 8, true);
    set_bone(model, 3, 0x500, turned, moved);
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(kept_as(tag0, 0, turned, at));
    // once noted: looked for again in the new tree
    aae_vmwidget_note(&tables, 0, model->bytes);
    int before = asked.lookups;
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(asked.lookups == before + 2 && kept_as(tag0, 0, turned, moved));
    // no tree, no skeleton, no bones or too many: nothing at all
    const float other[3] = { 7.0f, 7.0f, 7.0f };
    set_bone(model, 3, 0x500, turned, other);
    set_model(model, NULL, 8, true);
    tables.noted[0].tree = NULL;
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(kept_as(tag0, 0, turned, moved));
    set_model(model, trees[2], 8, false);
    tables.noted[0].tree = trees[2];
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(kept_as(tag0, 0, turned, moved));
    set_model(model, trees[2], 0, true);
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(kept_as(tag0, 0, turned, moved));
    // not even looked for again: what was kept stays
    tag0->lookup[0].model = NULL;
    before = asked.lookups;
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(asked.lookups == before && kept_as(tag0, 0, turned, moved) && tag0->lookup[0].model == NULL);
    tag0->lookup[0].model = model->bytes;
    set_model(model, trees[2], 0x181, true);
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(kept_as(tag0, 0, turned, moved));
    set_model(model, trees[2], 0x180, true);
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(kept_as(tag0, 0, turned, other));
    // a bone index beyond the model's bones counts as not found; at the last bone it is found
    set_model(model, trees[2], 4, true);
    tag0->lookup[0].model = NULL;
    model->found_answer = 4;
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(tag0->lookup[0].index == -1 && tag0->bone[0].valid == 0);
    tag0->lookup[0].model = NULL;
    model->found_answer = 3;
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(tag0->lookup[0].index == 3 && kept_as(tag0, 0, turned, other));
    model->found_answer = 0;
    // a cached index the model has fewer bones for now: skipped
    set_model(model, trees[2], 3, true);
    tag0->bone[0].valid = 0;
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(tag0->bone[0].valid == 0 && tag0->lookup[0].index == 3);
    set_model(model, trees[2], 8, true);
    // without a tag in use nothing is looked at
    tables.active[0] = 0;
    tag0->lookup[0].model = NULL;
    before = asked.lookups;
    aae_vmwidget_skeleton(&tables, model->bytes, fake_bone_index);
    CHECK(asked.lookups == before && tag0->bone[0].valid == 0);
    tables.active[0] = 3;
    // without the game's function, or for no model: nothing
    aae_vmwidget_skeleton(&tables, model->bytes, NULL);
    CHECK(tag0->bone[0].valid == 0);
    aae_vmwidget_skeleton(&tables, NULL, fake_bone_index);
    CHECK(tag0->bone[0].valid == 0);
}

// a bone is only ever written with its count odd, and a tag beyond the first 64 is kept like the others
static void test_writing(void) {
    tags_in_use(&tables);
    memset(models, 0, sizeof(models));
    set_model(&models[0], trees[0], 8, true);
    set_bone(&models[0], 3, 0x500, turned, at);
    written_while_odd = written_while_even = 0;
    aae_vmwidget_note(&tables, 0, models[0].bytes);
    aae_vmwidget_skeleton(&tables, models[0].bytes, fake_bone_index);
    CHECK(kept_as(tag0, 0, turned, at) && written_while_odd > 2 && written_while_even == 0);
    aae_vmwidget_tag_t *far = NULL;
    for (uint32_t name = 0x600; tables.tag_count < 70 && name < 0x1000; name++)
        far = aae_vmwidget_tag(&tables, name);
    aae_vmwidget_tag_t *user = NULL;
    aae_vmwidget_move(&tables, &user, far);
    set_bone(&models[0], 4, far->name, identity, at);
    aae_vmwidget_skeleton(&tables, models[0].bytes, fake_bone_index);
    CHECK(kept_as(far, 0, identity, at) && far->lookup[0].index == 4 && written_while_even == 0);
}

// the plugin's tests of a bone before it is kept
static bool keeps(const float rotation[4], const float position[3]) {
    tags_in_use(&tables);
    memset(models, 0, sizeof(models));
    set_model(&models[0], trees[0], 8, true);
    set_bone(&models[0], 3, 0x500, rotation, position);
    aae_vmwidget_note(&tables, 0, models[0].bytes);
    aae_vmwidget_skeleton(&tables, models[0].bytes, fake_bone_index);
    return tag0->bone[0].valid == 1;
}

static void test_bone_rules(void) {
    CHECK(keeps(turned, at));
    // the rotation's squared length must be above a quarter and below 4
    CHECK(!keeps((float[4]){ 0.5f, 0.0f, 0.0f, 0.0f }, at));
    CHECK(keeps((float[4]){ 0.5f, 0.0f, 0.0f, from_bits(0x3a800000) }, at));
    CHECK(!keeps((float[4]){ 0.0f, 2.0f, 0.0f, 0.0f }, at));
    CHECK(keeps((float[4]){ 0.0f, 0.0f, from_bits(0x3fffffff), 0.0f }, at));
    CHECK(!keeps((float[4]){ 0.0f, 0.0f, 0.0f, 0.0f }, at));
    // every part of it a number
    CHECK(!keeps((float[4]){ NAN, 0.0f, 0.0f, 1.0f }, at));
    CHECK(!keeps((float[4]){ 0.0f, INFINITY, 0.0f, 1.0f }, at));
    CHECK(!keeps((float[4]){ 0.0f, 0.0f, -INFINITY, 1.0f }, at));
    CHECK(!keeps((float[4]){ 0.0f, 0.0f, 0.0f, NAN }, at));
    // at the edges the order of the sum decides: ((x*x + y*y) + z*z) + w*w gives 0.25000003 and 3.9999998 for these,
    // which are kept; summed as (x*x + y*y) + (z*z + w*w) they come to 0.25 and 4
    CHECK(keeps((float[4]){ from_bits(0xbe90bcf2), from_bits(0xbe130f78), from_bits(0x3e9a9194), from_bits(0xbe774b94) },
                at));
    CHECK(keeps((float[4]){ from_bits(0x3f7b7752), from_bits(0xbfbfc9e2), from_bits(0xbf1e356d), from_bits(0x3f238bd1) },
                at));
    // a squared length that is too big to be a number
    CHECK(!keeps((float[4]){ 3e19f, 3e19f, 0.0f, 0.0f }, at));
    // the position: numbers, within 10,000,000
    CHECK(keeps(identity, (float[3]){ 9999999.0f, -9999999.0f, 9999999.0f }));
    CHECK(!keeps(identity, (float[3]){ 10000000.0f, 0.0f, 0.0f }));
    CHECK(!keeps(identity, (float[3]){ 0.0f, -10000000.0f, 0.0f }));
    CHECK(!keeps(identity, (float[3]){ 0.0f, 0.0f, 10000000.0f }));
    CHECK(!keeps(identity, (float[3]){ NAN, 0.0f, 0.0f }));
    CHECK(!keeps(identity, (float[3]){ 0.0f, INFINITY, 0.0f }));
    CHECK(!keeps(identity, (float[3]){ 0.0f, 0.0f, NAN }));
}

// ---- the method list

static aae_vmwidget_method_t list[3];
static int generic_stand_in(lua_State *s) {
    (void)s;
    return 0;
}
static int other_method(lua_State *s) {
    (void)s;
    return 1;
}
static aae_vmwidget_method_t ours_first, ours_second;
static void game_list(void) {
    list[0] = (aae_vmwidget_method_t){ "setGraphMode", other_method, &list[1] };
    list[1] = (aae_vmwidget_method_t){ "setupGeneric3DWidget", generic_stand_in, &list[2] };
    list[2] = (aae_vmwidget_method_t){ "setupRenderGraph", other_method, NULL };
    ours_first = (aae_vmwidget_method_t){ "first", other_method, NULL };
    ours_second = (aae_vmwidget_method_t){ "second", other_method, NULL };
}

static void test_splice(void) {
    game_list();
    // right behind the game's entry: first, second, then what was behind it
    CHECK(aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, true));
    CHECK(list[1].next == &ours_first && ours_first.next == &ours_second && ours_second.next == &list[2] &&
          list[0].next == &list[1] && list[2].next == NULL);
    // there already: left as it is
    CHECK(aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, true));
    CHECK(list[1].next == &ours_first && ours_first.next == &ours_second && ours_second.next == &list[2]);
    // taken out
    CHECK(!aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, false));
    CHECK(list[1].next == &list[2] && list[2].next == NULL);
    CHECK(!aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, false) && list[1].next == &list[2]);
    // and back in
    CHECK(aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, true) &&
          ours_second.next == &list[2]);
    // the first there but not followed by the second: the plugin leaves it, and says so
    game_list();
    list[2].next = &ours_first;
    CHECK(!aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, true) && list[1].next == &list[2]);
    // only the second there: left alone
    game_list();
    list[2].next = &ours_second;
    CHECK(!aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, true) && list[1].next == &list[2]);
    // taken out from anywhere along the list
    game_list();
    list[2].next = &ours_first;
    ours_first.next = &ours_second;
    CHECK(!aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, false) && list[2].next == NULL);
    // an entry that isn't the game's: by its name, or by its function
    game_list();
    CHECK(!aae_vmwidget_splice(&list[0], other_method, &ours_first, &ours_second, true) && list[0].next == &list[1]);
    CHECK(!aae_vmwidget_splice(&list[1], other_method, &ours_first, &ours_second, true) && list[1].next == &list[2]);
    list[1].name = NULL;
    CHECK(!aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, true) && list[1].next == &list[2]);
    // the plugin looks at 2048 entries behind the game's: the second as the 2048th is seen, as the 2049th not
    game_list();
    static aae_vmwidget_method_t many[Probes];
    list[1].next = &many[0];
    for (int i = 0; i < Probes - 2; i++)
        many[i].next = &many[i + 1];
    many[Probes - 2].next = &ours_second;
    ours_second.next = NULL;
    CHECK(!aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, true) && list[1].next == &many[0]);
    many[Probes - 2].next = &many[Probes - 1];
    many[Probes - 1].next = &ours_second;
    CHECK(aae_vmwidget_splice(&list[1], generic_stand_in, &ours_first, &ours_second, true) &&
          list[1].next == &ours_first && ours_second.next == &many[0]);
}

// ---- the two methods

static aae_vmwidget_tag_t *tag_named(const char *name) {
    uint32_t hash = 0;
    for (const char *c = name; *c != '\0'; c++)
        hash = hash * 31 + (uint8_t)*c;
    for (uint32_t i = 0; i < state.tag_count; i++)
        if (state.tag[i].name == 0x1234 + (hash & 0xfff))
            return &state.tag[i];
    return NULL;
}
static aae_vmwidget_update_t update_of(const uint8_t *element) {
    aae_vmwidget_update_t update;
    memcpy(&update, element + Element_Update, sizeof(update));
    return update;
}
static void set_update_of(uint8_t *element, aae_vmwidget_update_t update) {
    memcpy(element + Element_Update, &update, sizeof(update));
}

static void test_widget_method(void) {
    fresh();
    stack_of(L, 5);
    call_args(elements[0], NULL, 1.5f, -2.0f, 3.0f);
    CHECK(lua_tag_widget(L) == 0);
    // the element is argument 1; argument 2, nil here, gives no text: the right hand's tag
    CHECK(asked.to_element == 1 && asked.last_index == 1 && asked.tolstring == 1 && asked.tolstring_length == NULL);
    CHECK(asked.strings == 1 && strcmp(asked.string_text, "tag_weapon_right") == 0 && asked.string_user == 0 &&
          asked.string_length == 17 && asked.string_type == 3);
    aae_vmwidget_widget_t *widget = widget_of(&state, elements[0], false);
    aae_vmwidget_tag_t *right = tag_named("tag_weapon_right");
    CHECK(widget != NULL && right != NULL && widget->tag == right && widget->offset[0] == 1.5f &&
          widget->offset[1] == -2.0f && widget->offset[2] == 3.0f);
    CHECK(update_of(elements[0]) == widget_update && state.users[right - state.tag] == 1 && state.active[0] == 1);
    // a tag by name, and a number as the text it becomes there on the stack
    call_args(elements[1], "tag_flash", 0, 0, 0);
    lua_tag_widget(L);
    CHECK(strcmp(asked.string_text, "tag_flash") == 0 && widget_of(&state, elements[1], false)->tag == tag_named("tag_flash"));
    call_args(elements[1], NULL, 0, 0, 0);
    put_number(2, 5.0f);
    lua_tag_widget(L);
    CHECK(strcmp(asked.string_text, "5") == 0 && stack[1].t == HKS_TSTRING && strcmp(stack[1].v.ptr, "5") == 0);
    // the tag moved: tag_flash has no user left, its bit is off
    aae_vmwidget_tag_t *flash = tag_named("tag_flash");
    CHECK(state.users[flash - state.tag] == 0 && (state.active[0] & ((uint64_t)1 << (flash - state.tag))) == 0);
    // an empty text: the right hand's
    call_args(elements[1], "", 0, 0, 0);
    lua_tag_widget(L);
    CHECK(strcmp(asked.string_text, "tag_weapon_right") == 0);
    // only the element: the right hand's tag, no offset
    stack_of(L, 1);
    call_args(elements[2], "tag_flash", 9, 9, 9);
    lua_tag_widget(L);
    widget = widget_of(&state, elements[2], false);
    CHECK(widget != NULL && widget->tag == right && widget->offset[0] == 0.0f && widget->offset[1] == 0.0f &&
          widget->offset[2] == 0.0f && strcmp(asked.string_text, "tag_weapon_right") == 0);
    // what isn't a number is no offset
    stack_of(L, 5);
    call_args(elements[2], NULL, 4, 5, 6);
    put(4, HKS_TSTRING, "7");
    stack[4].t = 0x13;
    lua_tag_widget(L);
    CHECK(widget->offset[0] == 4.0f && widget->offset[1] == 0.0f && widget->offset[2] == 0.0f);
    // not an element: nothing asked further, nothing set up
    int strings = asked.strings;
    put(1, HKS_TNUMBER, NULL);
    CHECK(lua_tag_widget(L) == 0 && asked.strings == strings);
    // not ready: not even the element is asked for
    ready = false;
    int elements_asked = asked.to_element;
    call_args(elements[3], NULL, 1, 1, 1);
    CHECK(lua_tag_widget(L) == 0 && asked.to_element == elements_asked && update_of(elements[3]) == NULL);
    ready = true;
    CHECK(lua_tag_widget(NULL) == 0 && asked.to_element == elements_asked);
    // no script string: nothing
    string_answer = 0;
    CHECK(lua_tag_widget(L) == 0 && widget_of(&state, elements[3], false) == NULL && update_of(elements[3]) == NULL);
    string_answer = 0x1234;
    // no room for its tag: nothing; no room for the element: its tag is made all the same
    uint32_t count = state.tag_count;
    state.tag_count = AAE_VMWIDGET_TAGS;
    call_args(elements[3], "tag_new", 1, 1, 1);
    CHECK(lua_tag_widget(L) == 0 && widget_of(&state, elements[3], false) == NULL);
    state.tag_count = count;
    uint32_t widgets = state.widget_count;
    state.widget_count = AAE_VMWIDGET_WIDGETS;
    CHECK(lua_tag_widget(L) == 0 && widget_of(&state, elements[3], false) == NULL && tag_named("tag_new") != NULL &&
          update_of(elements[3]) == NULL);
    state.widget_count = widgets;
}

static void line_args(uint8_t *element, const char *tag, uint32_t function_type) {
    stack_of(L, 6);
    call_args(element, tag, 1.0f, 2.0f, 3.0f);
    put(6, function_type, (void *)0x77);
}

static void test_line_method(void) {
    fresh();
    line_args(elements[0], "tag_flash", Hks_Function_Lua);
    CHECK(lua_tag_widget_line(L) == 0);
    aae_vmwidget_line_t *line = line_of(&state, elements[0], false);
    CHECK(line != NULL && line->tag == tag_named("tag_flash") && line->lua == L && line->offset[0] == 1.0f &&
          line->offset[1] == 2.0f && line->offset[2] == 3.0f && line->off == 0 && line->calling == 0);
    // the function and a new table with room for 2 are kept by the registry's references
    CHECK(asked.refs == 2 && asked.ref_table == -10000 && asked.tables == 1 && asked.table_array == 2 &&
          asked.table_hash == 0);
    CHECK(asked.popped[0].t == Hks_Function_Lua && asked.popped[0].v.ptr == (void *)0x77 &&
          asked.popped[1].t == Hks_Table && asked.popped[1].v.ptr == table_memory);
    CHECK(line->function_reference == 3 && line->table_reference == 4 && line->function.t == Hks_Function_Lua &&
          line->function.v.ptr == (void *)0x77 && line->table.t == Hks_Table && line->table.v.ptr == table_memory);
    CHECK(line->before == NULL && update_of(elements[0]) == line_update && HKS_TOP(L) == stack + 6);
    CHECK(asked.unrefs == 0);
    // set up again from the same Lua state: the old references are given back first
    line_args(elements[0], "tag_flash", Hks_Function_Lua);
    lua_tag_widget_line(L);
    CHECK(asked.unrefs == 2 && asked.unref_table[0] == -10000 && asked.unref_reference[0] == 3 &&
          asked.unref_table[1] == -10000 && asked.unref_reference[1] == 4 && line->function_reference == 5 &&
          line->table_reference == 6);
    // a reference of 0 is given back as well
    line->function_reference = 0, line->table_reference = 0;
    line_args(elements[0], "tag_flash", Hks_Function_Lua);
    lua_tag_widget_line(L);
    CHECK(asked.unrefs == 4 && asked.unref_reference[2] == 0 && asked.unref_reference[3] == 0);
    asked.unrefs = 2;
    // from another one: not
    HKS_BASE(other_L) = stack;
    HKS_TOP(other_L) = stack + 6;
    lua_tag_widget_line(other_L);
    CHECK(asked.unrefs == 2 && line->lua == other_L && line->function_reference == 9);
    // the element's own update is kept to run first, but never the line's own
    line_args(elements[1], NULL, Hks_Function_Lua);
    set_update_of(elements[1], fake_before);
    lua_tag_widget_line(L);
    line = line_of(&state, elements[1], false);
    CHECK(line->before == fake_before && line->tag == tag_named("tag_weapon_right"));
    line_args(elements[1], NULL, Hks_Function_Lua);
    lua_tag_widget_line(L);
    CHECK(line->before == fake_before && update_of(elements[1]) == line_update);
    // a switched-off line is on again when it is set up again
    line->off = 1, line->calling = 1;
    line_args(elements[1], NULL, Hks_Function_Lua);
    lua_tag_widget_line(L);
    CHECK(line->off == 0 && line->calling == 0);
    // the C side's kinds of function, with the type word kept whole
    line_args(elements[2], NULL, Hks_Function_C);
    lua_tag_widget_line(L);
    CHECK(line_of(&state, elements[2], false)->function.t == Hks_Function_C);
    line_args(elements[2], NULL, Hks_Function_C + 1);
    lua_tag_widget_line(L);
    CHECK(line_of(&state, elements[2], false)->function.t == Hks_Function_C + 1);
    line_args(elements[2], NULL, 0x16);
    lua_tag_widget_line(L);
    CHECK(line_of(&state, elements[2], false)->function.t == 0x16);
    // anything else as argument 6, or none: nothing, not even its tag is made
    static const uint32_t not_functions[] = { HKS_TNIL, Hks_Table, 7, Hks_Function_C + 2, 8, HKS_TSTRING };
    for (size_t i = 0; i < sizeof(not_functions) / sizeof(not_functions[0]); i++) {
        int strings = asked.strings, refs = asked.refs;
        line_args(elements[3], "tag_other", not_functions[i]);
        CHECK(lua_tag_widget_line(L) == 0 && asked.strings == strings && asked.refs == refs &&
              line_of(&state, elements[3], false) == NULL && tag_named("tag_other") == NULL);
    }
    line_args(elements[3], "tag_other", Hks_Function_Lua);
    HKS_TOP(L) = stack + 5;
    CHECK(lua_tag_widget_line(L) == 0 && line_of(&state, elements[3], false) == NULL && tag_named("tag_other") == NULL);
    // no script string for its tag, or no room for it: nothing, no reference taken
    string_answer = 0;
    int refs_before = asked.refs;
    line_args(elements[3], "tag_other", Hks_Function_Lua);
    CHECK(lua_tag_widget_line(L) == 0 && asked.refs == refs_before && line_of(&state, elements[3], false) == NULL);
    string_answer = 0x1234;
    uint32_t tags_made = state.tag_count;
    state.tag_count = AAE_VMWIDGET_TAGS;
    CHECK(lua_tag_widget_line(L) == 0 && asked.refs == refs_before && line_of(&state, elements[3], false) == NULL);
    state.tag_count = tags_made;
    // the function's reference not taken: nothing more
    references_answered = true, function_reference_answer = -1, table_reference_answer = 9;
    asked.refs = 0;
    int tables = asked.tables, unrefs = asked.unrefs;
    line_args(elements[3], NULL, Hks_Function_Lua);
    CHECK(lua_tag_widget_line(L) == 0 && asked.tables == tables && asked.unrefs == unrefs &&
          line_of(&state, elements[3], false) == NULL);
    // the table's not taken: the function's is given back
    asked.refs = 0;
    function_reference_answer = 12, table_reference_answer = -1;
    line_args(elements[3], NULL, Hks_Function_Lua);
    CHECK(lua_tag_widget_line(L) == 0 && asked.unrefs == unrefs + 1 && asked.unref_reference[unrefs % 8] == 12 &&
          line_of(&state, elements[3], false) == NULL);
    references_answered = false;
    // what was made isn't a table, or has none: the function's reference is given back
    made_type = HKS_TSTRING;
    unrefs = asked.unrefs;
    int refs = asked.refs;
    line_args(elements[3], NULL, Hks_Function_Lua);
    CHECK(lua_tag_widget_line(L) == 0 && asked.unrefs == unrefs + 1 && asked.refs == refs + 1 &&
          line_of(&state, elements[3], false) == NULL);
    made_type = Hks_Table | 0x20;
    line_args(elements[3], NULL, Hks_Function_Lua);
    CHECK(lua_tag_widget_line(L) == 0 && line_of(&state, elements[3], false) != NULL);
    made_type = Hks_Table, made_without_table = true;
    unrefs = asked.unrefs;
    line_args(elements[2], NULL, Hks_Function_Lua);
    CHECK(lua_tag_widget_line(L) == 0 && asked.unrefs == unrefs + 1);
    made_without_table = false;
    // no room for the element: both references given back
    uint32_t lines = state.line_count;
    state.line_count = AAE_VMWIDGET_WIDGETS;
    unrefs = asked.unrefs;
    int next = next_reference;
    line_args((uint8_t *)elements + 8, NULL, Hks_Function_Lua);
    put(1, Light_Userdata, (uint8_t *)elements + 8);
    CHECK(lua_tag_widget_line(L) == 0 && asked.unrefs == unrefs + 2 && asked.unref_reference[unrefs % 8] == next &&
          asked.unref_reference[(unrefs + 1) % 8] == next + 1);
    state.line_count = lines;
    // not ready, no element, no stack: nothing
    ready = false;
    refs = asked.refs;
    line_args(elements[3], NULL, Hks_Function_Lua);
    CHECK(lua_tag_widget_line(L) == 0 && asked.refs == refs);
    ready = true;
    put(1, HKS_TNIL, NULL);
    CHECK(lua_tag_widget_line(L) == 0 && asked.refs == refs);
    put(1, Light_Userdata, elements[3]);
    element_answer = elements[3];
    HksObject *base = HKS_BASE(L);
    HKS_BASE(L) = NULL;
    CHECK(lua_tag_widget_line(L) == 0 && asked.refs == refs);
    HKS_BASE(L) = base;
    HKS_TOP(L) = NULL;
    CHECK(lua_tag_widget_line(L) == 0 && asked.refs == refs);
    element_answer = NULL;
    CHECK(lua_tag_widget_line(NULL) == 0 && asked.refs == refs);
    // without the game's luaL_ref or lua_createtable: nothing kept
    stack_of(L, 6);
    hksi_luaL_ref = NULL;
    CHECK(lua_tag_widget_line(L) == 0 && line_of(&state, elements[3], false)->function_reference != -2);
    hksi_luaL_ref = fake_ref;
    hksi_lua_createtable = NULL;
    unrefs = asked.unrefs;
    line_args(elements[0], NULL, Hks_Function_Lua);
    CHECK(lua_tag_widget_line(L) == 0 && asked.unrefs == unrefs + 1);
    hksi_lua_createtable = fake_createtable;
}

// ---- the updates the methods give an element

static aae_vmwidget_tag_t *widget_on(uint8_t *element, float x, float y, float z) {
    stack_of(L, 5);
    call_args(element, NULL, x, y, z);
    lua_tag_widget(L);
    return tag_named("tag_weapon_right");
}
static void bone_for(aae_vmwidget_tag_t *tag, int client, const float rotation[4], const float position[3]) {
    tag->bone[client].valid = 1;
    memcpy(tag->bone[client].rotation, rotation, 16);
    memcpy(tag->bone[client].position, position, 12);
}

static void test_widget_update(void) {
    fresh();
    aae_vmwidget_tag_t *tag = widget_on(elements[0], 1, 0, 0);
    const float position[3] = { 10, 20, 30 };
    bone_for(tag, 0, identity, position);
    widget_update(0, elements[0], 0x1111, 0x2222, 0x3333, L);
    // the point goes into the element, and the game's own 3D widget update gets every argument as it came
    CHECK(asked.generic == 1 && asked.generic_client == 0 && asked.generic_element == elements[0] &&
          asked.generic_root == 0x1111 && asked.generic_fourth == 0x2222 && asked.generic_fifth == 0x3333 &&
          asked.generic_lua == L && asked.generic_point[0] == 11 && asked.generic_point[1] == 20 &&
          asked.generic_point[2] == 30);
    // nothing at all without a point: not even the element's usual update
    const float marked[3] = { -5, -5, -5 };
    memcpy(elements[0] + Element_Point, marked, sizeof(marked));
    widget_update(1, elements[0], 0, 0, 0, L);
    CHECK(asked.generic == 1 && memcmp(elements[0] + Element_Point, marked, sizeof(marked)) == 0);
    widget_update(4, elements[0], 0, 0, 0, L);
    widget_update(-1, elements[0], 0, 0, 0, L);
    widget_update(0x40000000, elements[0], 0, 0, 0, L);
    widget_update(-0x40000000, elements[0], 0, 0, 0, L);
    CHECK(asked.generic == 1);
    tag->bone[0].sequence++;
    widget_update(0, elements[0], 0, 0, 0, L);
    CHECK(asked.generic == 1);
    tag->bone[0].sequence++;
    const float edge[3] = { 9999999.0f, 0, 0 };
    bone_for(tag, 0, identity, edge);
    widget_update(0, elements[0], 0, 0, 0, L);
    CHECK(asked.generic == 1 && memcmp(elements[0] + Element_Point, marked, sizeof(marked)) == 0);
    bone_for(tag, 0, identity, position);
    widget_update(0, elements[1], 0, 0, 0, L);
    widget_update(0, NULL, 0, 0, 0, L);
    CHECK(asked.generic == 1);
    LUI_Generic3DUpdate = NULL;
    widget_update(0, elements[0], 0, 0, 0, L);
    CHECK(asked.generic == 1);
    LUI_Generic3DUpdate = fake_generic;
    widget_of(&state, elements[0], false)->tag = NULL;
    widget_update(0, elements[0], 0, 0, 0, L);
    CHECK(asked.generic == 1);
    widget_of(&state, elements[0], false)->tag = tag;
    widget_update(0, elements[0], 0, 0, 0, L);
    CHECK(asked.generic == 2);
    // an offset of nothing: the bone's own position, untested
    widget_on(elements[0], 0, 0, 0);
    const float far[3] = { 2e7f, 0, 0 };
    bone_for(tag, 0, identity, far);
    widget_update(0, elements[0], 0, 0, 0, L);
    CHECK(asked.generic == 3 && asked.generic_point[0] == 2e7f);
}

static aae_vmwidget_line_t *line_on(uint8_t *element, float x, float y, float z) {
    stack_of(L, 6);
    call_args(element, NULL, x, y, z);
    put(6, Hks_Function_Lua, (void *)0x77);
    lua_tag_widget_line(L);
    HKS_TOP(L) = stack + 8;
    return line_of(&state, element, false);
}
static bool handed_the_table(float x, float y) {
    uint64_t slots[4];
    memcpy(slots, table_array, sizeof(slots));
    return asked.pcall_argument.t == Hks_Table && asked.pcall_argument.v.ptr == table_memory &&
           slots[0] == HKS_TNUMBER && slots[2] == HKS_TNUMBER && same_bits(from_bits((uint32_t)slots[1]), x) &&
           same_bits(from_bits((uint32_t)slots[3]), y) && (slots[1] >> 32) == 0 && (slots[3] >> 32) == 0;
}
static bool handed_nil(void) {
    static const HksObject nothing;
    return memcmp(&asked.pcall_argument, &nothing, sizeof(nothing)) == 0;
}
static void set_table_size(uint32_t size) {
    memcpy(table_memory + Table_Array_Size, &size, sizeof(size));
}

static void test_line_update(void) {
    fresh();
    set_update_of(elements[2], fake_before);
    aae_vmwidget_line_t *line = line_on(elements[2], 0, 0, 0);
    aae_vmwidget_tag_t *tag = line->tag;
    const float position[3] = { 100, 200, 300 };
    bone_for(tag, 0, identity, position);
    line_update(0, elements[2], 0x1111, 0x2222, 0x3333, L);
    // first the element's own update, with every argument as it came
    CHECK(asked.befores == 1 && asked.before_client == 0 && asked.before_element == elements[2] &&
          asked.before_root == 0x1111 && asked.before_fourth == 0x2222 && asked.before_fifth == 0x3333 &&
          asked.before_lua == L);
    // the point is projected, and the function is called with the table of the place on the screen
    CHECK(asked.projections == 1 && asked.projection_client == 0 && asked.projected[0] == 100 &&
          asked.projected[1] == 200 && asked.projected[2] == 300);
    CHECK(asked.pcalls == 1 && asked.pcall_arguments == 1 && asked.pcall_results == 0 && asked.pcall_handler == 0 &&
          asked.pcall_top == stack + 10 && asked.pcall_function.t == Hks_Function_Lua &&
          asked.pcall_function.v.ptr == (void *)0x77 && handed_the_table(640.0f, 360.0f));
    CHECK(HKS_TOP(L) == stack + 8 && line->calling == 0 && line->off == 0);
    // a place that isn't one: nil
    screen_answer[0] = NAN;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 2 && handed_nil() && asked.pcall_top == stack + 10);
    screen_answer[1] = 10000000.0f, screen_answer[0] = 1.0f;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 3 && handed_nil());
    // a projection that writes nothing leaves the place at 0, 0, which counts
    projection_writes = false;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 4 && handed_the_table(0.0f, 0.0f));
    projection_writes = true, screen_answer[0] = 12.5f, screen_answer[1] = -3.0f;
    // no bone: no projection, nil
    tag->bone[0].valid = 0;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.projections == 4 && asked.pcalls == 5 && handed_nil());
    line_update(5, elements[2], 0, 0, 0, L);
    CHECK(asked.projections == 4 && asked.pcalls == 6 && handed_nil());
    line_update(0x40000000, elements[2], 0, 0, 0, L);
    line_update(-0x40000000, elements[2], 0, 0, 0, L);
    CHECK(asked.projections == 4 && asked.pcalls == 8 && handed_nil());
    asked.pcalls = 6, asked.befores -= 2;
    tag->bone[0].valid = 1;
    // a point too far: nil
    line->offset[0] = 1.0f;
    const float edge[3] = { 9999999.0f, 0, 0 };
    bone_for(tag, 0, identity, edge);
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.projections == 4 && asked.pcalls == 7 && handed_nil());
    line->offset[0] = 0.0f;
    bone_for(tag, 0, identity, position);
    // another Lua state: the element's own update only
    line_update(0, elements[2], 0, 0, 0, other_L);
    CHECK(asked.befores == 8 && asked.projections == 4 && asked.pcalls == 7);
    // the function fails: switched off, the stack put back; from then on no more calls, but still projections
    pcall_answer = 1;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 8 && line->off == 1 && HKS_TOP(L) == stack + 8);
    pcall_answer = 0;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 8 && asked.projections == 6 && asked.befores == 10);
    line = line_on(elements[2], 0, 0, 0);
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 9 && line->off == 0);
    // a function that makes the element update again: not called from inside itself
    reenter = elements[2];
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 10 && line->calling == 0 && reenter == NULL);
    asked.pcalls = 9;
    // while its function runs it isn't called again
    line->calling = 1;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 9 && line->calling == 1);
    line->calling = 0;
    // no reference for the function: nothing
    line->function_reference = -1;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 9 && line->off == 0);
    line->function_reference = 0;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 10);
    // the kept value isn't a function: switched off, nothing pushed
    line->function.t = Hks_Table;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == 10 && line->off == 1 && HKS_TOP(L) == stack + 8);
    // tables that can't take the place: switched off, the stack put back
    struct {
        uint32_t type;
        bool no_table, no_array;
        uint32_t size;
    } bad[] = { { HKS_TSTRING, false, false, 2 }, { Hks_Table, true, false, 2 }, { Hks_Table, false, true, 2 },
                { Hks_Table, false, false, 1 }, { Hks_Table, false, false, 0 }, { Hks_Table, false, false, 0x100001 },
                { Hks_Table, false, false, 0xffffffff } };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        line = line_on(elements[2], 0, 0, 0);
        line->table.t = bad[i].type;
        if (bad[i].no_table)
            line->table.v.ptr = NULL;
        HksObject *array = bad[i].no_array ? NULL : table_array;
        memcpy(table_memory + Table_Array, &array, sizeof(array));
        set_table_size(bad[i].size);
        int pcalls = asked.pcalls;
        line_update(0, elements[2], 0, 0, 0, L);
        CHECK(asked.pcalls == pcalls && line->off == 1 && HKS_TOP(L) == stack + 8);
        array = table_array;
        memcpy(table_memory + Table_Array, &array, sizeof(array));
    }
    // a table with a kind in its type word's other bits, and the largest array: fine
    line = line_on(elements[2], 0, 0, 0);
    line->table.t = Hks_Table | 0x30;
    set_table_size(0x100000);
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcall_argument.t == (Hks_Table | 0x30) && line->off == 0);
    set_table_size(2);
    // but with no place to be had, nil is handed whatever the table is
    line->table.t = HKS_TSTRING;
    tag->bone[0].valid = 0;
    int pcalls = asked.pcalls;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == pcalls + 1 && handed_nil() && line->off == 0);
    tag->bone[0].valid = 1;
    // no projection to be had: nil
    line = line_on(elements[2], 0, 0, 0);
    CG_WorldToScreen = NULL;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(handed_nil());
    CG_WorldToScreen = fake_projection;
    // without the game's lua_pcall, or a top of the stack: nothing
    hksi_lua_pcall = NULL;
    pcalls = asked.pcalls;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(line->off == 0);
    hksi_lua_pcall = fake_pcall;
    HKS_TOP(L) = NULL;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.pcalls == pcalls && line->off == 0);
    HKS_TOP(L) = stack + 8;
    // an element without a line: nothing, not even an update from before
    int befores = asked.befores;
    line_update(0, elements[3], 0, 0, 0, L);
    line_update(0, NULL, 0, 0, 0, L);
    CHECK(asked.befores == befores && asked.pcalls == pcalls);
    // the line's own update as the one from before isn't called again
    line->before = line_update;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.befores == befores && asked.pcalls == pcalls + 1);
    line->before = NULL;
    line_update(0, elements[2], 0, 0, 0, L);
    CHECK(asked.befores == befores && asked.pcalls == pcalls + 2);
}

// ---- the hooks

static struct {
    int calls;
    uint64_t first, second;
    bool ours_in;
} startup;
static void fake_startup(uint64_t first, uint64_t second) {
    startup.calls++;
    startup.first = first, startup.second = second;
    startup.ours_in = list[1].next == &tag_widget_method && tag_widget_method.next == &tag_widget_line_method &&
                      tag_widget_line_method.next == &list[2];
}
static uint64_t skeleton_arguments[3];
static uint64_t fake_calc_skel(uint8_t *model, uint64_t parts, uint64_t origin) {
    skeleton_arguments[0] = (uint64_t)model, skeleton_arguments[1] = parts, skeleton_arguments[2] = origin;
    return 0x5555;
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

static void test_startup(void) {
    fresh();
    game_list();
    list[1].function = generic_stand_in;
    generic_method = &list[1];
    generic_function = generic_stand_in;
    orig_LUI_Startup = fake_startup;
    set_mod(false);
    fake_LUI_Startup(7, 8);
    CHECK(startup.calls == 1 && startup.first == 7 && startup.second == 8 && !startup.ours_in);
    set_mod(true);
    fake_LUI_Startup(7, 8);
    CHECK(startup.calls == 2 && startup.ours_in);
    // the two methods by their names, with the functions behind them
    CHECK(strcmp(tag_widget_method.name, "setupViewmodelTagWidget") == 0 &&
          tag_widget_method.function == lua_tag_widget &&
          strcmp(tag_widget_line_method.name, "setupViewmodelTagWidgetLine") == 0 &&
          tag_widget_line_method.function == lua_tag_widget_line);
    fake_LUI_Startup(7, 8);
    CHECK(startup.calls == 3 && startup.ours_in);
    // the mod gone: gone with it
    set_mod(false);
    fake_LUI_Startup(1, 2);
    CHECK(startup.calls == 4 && !startup.ours_in && list[1].next == &list[2]);
}

static void test_skeleton_hook(void) {
    fresh();
    aae_vmwidget_tag_t *tag = widget_on(elements[0], 0, 0, 0);
    skeleton_returns[0] = (const void *)0x10, skeleton_returns[1] = (const void *)0x20;
    set_model(&models[0], trees[0], 8, true);
    set_bone(&models[0], 2, tag->name, turned, at);
    set_model(&models[1], trees[1], 8, true);
    set_bone(&models[1], 2, tag->name, identity, at);
    set_weapon_model(0, &models[0]);
    set_weapon_model(1, &models[1]);
    // returning anywhere else: nothing
    skeleton_computed(models[0].bytes, (const void *)0x30);
    CHECK(tag->bone[0].valid == 0 && state.noted[0].model == NULL);
    // to one of the two: the local clients' weapon models are noted and the bones kept
    skeleton_computed(models[0].bytes, (const void *)0x10);
    CHECK(kept_as(tag, 0, turned, at) && state.noted[0].model == models[0].bytes && state.noted[1].model == NULL);
    skeleton_computed(models[1].bytes, (const void *)0x20);
    CHECK(tag->bone[1].valid == 0 && state.noted[1].model == NULL);
    // as many local clients as the game has
    clients_in_game = 2;
    skeleton_computed(models[1].bytes, (const void *)0x20);
    CHECK(kept_as(tag, 1, identity, at) && state.noted[1].model == models[1].bytes);
    // a client without a weapon model keeps its note
    set_weapon_model(1, NULL);
    skeleton_computed(models[0].bytes, (const void *)0x10);
    CHECK(state.noted[1].model == models[1].bytes);
    // no view weapon states yet: nothing noted
    view_states_pointer = NULL;
    state.noted[0].model = NULL;
    skeleton_computed(models[0].bytes, (const void *)0x10);
    CHECK(state.noted[0].model == NULL);
    view_states_pointer = view_states;
    // no tag in use, or not ready: nothing, not even a note
    uint64_t active = state.active[0];
    state.active[0] = 0;
    skeleton_computed(models[0].bytes, (const void *)0x10);
    CHECK(state.noted[0].model == NULL);
    state.active[0] = active;
    ready = false;
    skeleton_computed(models[0].bytes, (const void *)0x10);
    CHECK(state.noted[0].model == NULL);
    ready = true;
    // the hook hands every argument on and gives back what the game's function gave
    orig_DObjCalcSkel = fake_calc_skel;
    CHECK(fake_DObjCalcSkel(models[0].bytes, 0x1234, 0x5678) == 0x5555 &&
          skeleton_arguments[0] == (uint64_t)models[0].bytes && skeleton_arguments[1] == 0x1234 &&
          skeleton_arguments[2] == 0x5678);
}

static void test_init(void) {
    const uint64_t base = 0x100000000;
    ready = false;
    hooks = 0;
    aae_vmwidget_init(base, 0);
    CHECK(hooks == 0 && !ready);
    aae_vmwidget_init(base, 1);
    CHECK(ready && hooks == 2);
    CHECK(was_hooked(ADDR_LUI_Startup, (void *)fake_LUI_Startup, (void **)&orig_LUI_Startup));
    CHECK(was_hooked(ADDR_DObjCalcSkel, (void *)fake_DObjCalcSkel, (void **)&orig_DObjCalcSkel));
    CHECK(LUI_ToElement == (void *)(base + ADDR_LUI_ToElement) &&
          hks_obj_tolstring == (void *)(base + ADDR_hks_obj_tolstring) &&
          SL_GetStringOfSize == (void *)(base + ADDR_SL_GetStringOfSize) &&
          hksi_lua_createtable == (void *)(base + ADDR_hksi_lua_createtable) &&
          hksi_luaL_ref == (void *)(base + ADDR_hksi_luaL_ref) && hksi_luaL_unref == (void *)(base + ADDR_hksi_luaL_unref) &&
          hksi_lua_pcall == (void *)(base + ADDR_hksi_lua_pcall) &&
          LUI_Generic3DUpdate == (aae_vmwidget_update_t)(base + ADDR_LUI_Generic3DUpdate) &&
          CG_WorldToScreen == (void *)(base + ADDR_CG_WorldToScreen) &&
          DObjGetBoneIndex == (void *)(base + ADDR_DObjGetBoneIndex));
    CHECK(generic_method == (void *)(base + ADDR_LUI_Generic3DMethod) &&
          generic_function == (lua_CFunction)(base + ADDR_Lua_SetupGeneric3DWidget) &&
          skeleton_returns[0] == (void *)(base + ADDR_DObjCalcSkel_ViewReturn) &&
          skeleton_returns[1] == (void *)(base + ADDR_DObjCalcSkel_PoseReturn) &&
          view_weapon_states == (void *)(base + ADDR_cgViewWeaponStates) &&
          local_client_count == (void *)(base + ADDR_LOCAL_CLIENT_COUNT));
    // the places in the game's own structures, as they were read from its code, and the plugin's sizes
    CHECK(Model_Tree == 0x18 && Model_Skeleton == 0x58 && Model_Computed == 0xc8 && Model_Bones == 0x120 &&
          Bone_Size == 0x20 && Bone_Position == 0x10 && Element_Point == 0x78 && Element_Update == 0x110 &&
          Table_Array == 0x28 && Table_Array_Size == 0x30 && View_Weapon_State_Size == 0x3a0 && Lua_Registry == -10000);
    CHECK(AAE_VMWIDGET_TAGS == 128 && AAE_VMWIDGET_TAG_SLOTS == 256 && AAE_VMWIDGET_WIDGETS == 1024 &&
          AAE_VMWIDGET_ELEMENT_SLOTS == 2048 && Probes == 0x800 && sizeof(aae_vmwidget_tag_t) == 0xf8 &&
          sizeof(aae_vmwidget_bone_t) == 0x24 && sizeof(HksObject) == 16);
}

// ---- the same cases the plugin's own functions were given (see vmwidget_cases.h), and what that gave
#define Plugin_Lines 180004
#define Plugin_Checksum 0x8a786a931feefe84ULL
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
static void case_stack(const cases_widget_t *c, uint8_t *element, int arguments) {
    memset(stack, 0, sizeof(stack));
    put(1, Light_Userdata, element);
    for (int i = 0; i < 3; i++) {
        stack[2 + i].t = c->type[i];
        memcpy(&stack[2 + i].v, &c->value[i], sizeof(float));
    }
    stack_of(L, arguments);
}
static void case_answers(const cases_widget_t *c, const float position[3], const float rotation[4]) {
    aae_vmwidget_tag_t *tag = &state.tag[0];
    tag->bone[0].valid = 1;
    memcpy(tag->bone[0].position, position, 12);
    memcpy(tag->bone[0].rotation, rotation, 16);
    memcpy(screen_answer, c->screen, sizeof(screen_answer));
    projection_writes = c->writes;
    pcall_answer = c->fails;
}
static int side_widget(const cases_widget_t *c, float point[3]) {
    case_stack(c, elements[0], 5);
    lua_tag_widget(L);
    case_answers(c, c->position, c->rotation);
    int before = asked.generic;
    update_of(elements[0])(0, elements[0], 0, 0, 0, L);
    memcpy(point, asked.generic_point, 12);
    return asked.generic - before;
}
static void side_line(const cases_widget_t *c, int *projected, float point[3], int *handed, float numbers[2],
                      int *off) {
    case_stack(c, elements[1], 6);
    put(6, Hks_Function_Lua, (void *)0x77);
    lua_tag_widget_line(L);
    case_answers(c, c->position, c->rotation);
    memset(table_array, 0, sizeof(table_array));
    set_table_size(c->size);
    HKS_TOP(L) = stack + 8;
    int projections = asked.projections, pcalls = asked.pcalls;
    update_of(elements[1])(0, elements[1], 0, 0, 0, L);
    *projected = asked.projections - projections;
    memcpy(point, asked.projected, 12);
    uint32_t kind = asked.pcall_argument.t & 0xf;
    *handed = asked.pcalls == pcalls ? 0 : kind == Hks_Table ? 2 : kind == HKS_TNIL ? 1 : 3;
    uint64_t slots[4];
    memcpy(slots, table_array, sizeof(slots));
    memcpy(&numbers[0], &slots[1], sizeof(float));
    memcpy(&numbers[1], &slots[3], sizeof(float));
    if (*handed == 2 && (slots[0] != HKS_TNUMBER || slots[2] != HKS_TNUMBER || (slots[1] >> 32) != 0))
        *handed = 4;
    *off = (int)line_of(&state, elements[1], false)->off;
}
static const cases_bone_t *bone_case;
static int case_bone_index(const uint8_t *model, uint32_t name, int16_t *index, uint32_t parts) {
    (void)model, (void)name, (void)parts;
    *index = bone_case->index;
    return bone_case->found;
}
static uint8_t case_model[0x200], case_tree[16];
static float case_skeleton[0x182][8];
static int side_bone(const cases_bone_t *c, float kept[7]) {
    bone_case = c;
    void *tree = case_tree, *skeleton = c->skeleton ? (void *)case_skeleton : NULL;
    memset(case_model, 0, sizeof(case_model));
    memcpy(case_model + Model_Tree, &tree, sizeof(tree));
    memcpy(case_model + Model_Skeleton, &skeleton, sizeof(skeleton));
    memcpy(case_model + Model_Bones, &c->bones, sizeof(c->bones));
    memset(case_skeleton, 0, sizeof(case_skeleton));
    if (c->index >= 0) {
        memcpy(case_skeleton[c->index], c->rotation, 16);
        memcpy(case_skeleton[c->index] + 4, c->position, 12);
        if (c->computed && c->index < 384) {
            uint32_t bits = 0x80000000u >> (c->index & 31);
            memcpy(case_model + Model_Computed + (c->index >> 5) * 4, &bits, 4);
        }
    }
    state.noted[0].model = case_model;
    state.noted[0].tree = case_tree;
    forget_lookup(&state.tag[0].lookup[0]);
    state.tag[0].bone[0].valid = 0;
    aae_vmwidget_skeleton(&state, case_model, case_bone_index);
    memcpy(kept, state.tag[0].bone[0].rotation, 16);
    memcpy(kept + 4, state.tag[0].bone[0].position, 12);
    return (int)state.tag[0].bone[0].valid;
}

static void test_like_the_plugin(void) {
    fresh();
    cases_out.checksum = 0xcbf29ce484222325ULL, cases_out.lines = 0;
    cases_run();
    CHECK(cases_out.lines == Plugin_Lines);
    CHECK(cases_out.checksum == Plugin_Checksum);
    if (cases_out.checksum != Plugin_Checksum)
        printf("  the cases came out as %#018llx: wip/agents/vua/wine/compare.sh shows where they differ from the "
               "plugin's\n", (unsigned long long)cases_out.checksum);
}

int main(void) {
    char template[] = "/tmp/bo3macfix-vmwidget-test-XXXXXX";
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

    test_init();
    loaded_mod = loaded_name;
    test_slots();
    test_tags();
    test_records();
    test_move();
    test_offset();
    test_read();
    test_point();
    test_screen();
    test_clients();
    test_note();
    test_skeleton();
    test_bone_rules();
    test_writing();
    test_splice();
    test_widget_method();
    test_line_method();
    test_widget_update();
    test_line_update();
    test_startup();
    test_skeleton_hook();
    test_like_the_plugin();

    snprintf(command, sizeof(command), "rm -rf '%s'", template);
    if (system(command) != 0)
        failures++;
    if (failures != 0) {
        printf("test_aae_vmwidget: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_vmwidget: all passed\n");
    return 0;
}
