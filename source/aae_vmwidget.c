// All-around Enhancement 4's widgets on the first person weapon: the plugin's viewmodel_ui_anchor component.
//
// The game's UI elements have a method setupGeneric3DWidget(x, y, z): the element is then drawn each frame at the
// place on the screen that point of the world is at. The plugin adds two methods next to it for widgets that sit on a
// bone (a "tag") of the first person weapon model instead:
//   element:setupViewmodelTagWidget(tag, x, y, z)
//       the element is drawn where the bone is, moved by (x, y, z) in the bone's own directions; while there is no
//       such place (no weapon model yet, a tag the model hasn't got) the element isn't updated at all;
//   element:setupViewmodelTagWidgetLine(tag, x, y, z, callback)
//       each frame callback(place) is called with a table {screen x, screen y} of that point (always the same
//       table), or with nil while there is none; the element's own update runs first. An error in callback switches
//       it off until the element is set up again. Where this differs from the plugin: a line forgets the Lua VM it was
//       set up in when LUI starts up again (aae_vmwidget_forget_lines says why).
// The tag is "tag_weapon_right" when none is given. Neither returns anything. All-around Enhancement 4.1.7 calls
// neither: they are here so that the Mac has what the plugin has, for a version of the mod that uses them.
//
// The methods. The game keeps a list of the UI element methods, {name, function, next}, built by static
// initializers; LUI's start-up registers every entry of it in the new Lua VM. The plugin links its two entries in
// right behind setupGeneric3DWidget's. The Mac's list is built after BO3MacFix has started, so here LUI's start-up
// is hooked: before each new VM registers the list, the two entries are put in when the mod with its plugin is
// loaded and taken out otherwise. (On Windows they exist in every VM made after the plugin was loaded, also after
// leaving the mod.)
//
// The bones. Each frame the game computes the weapon model's skeleton. The plugin redirects two of the game's calls
// of DObjCalcSkel to a function of its own that calls it and then copies, for every tag a widget uses, the bone's
// rotation and position out of the local client's weapon model (it notes which model that is in a hook of the view
// model's update). Here aae_skeleton.c hooks DObjCalcSkel for this and the grenade line's piece (the plugin does
// that one's work first, in the same function) and calls widgets_after_skeleton after those two calls; the weapon
// model is read from the game's view weapon state at that moment. The copies are kept under a sequence count, as the
// plugin keeps them: the UI reads them on another path of the frame (the two calls run on the game's job threads).
//
// Each element set up this way gets an update function of the plugin's (the element's +0x110), which the game calls
// every frame with (local client, element, root element, two numbers, the UI's lua_State). The one for a widget puts
// the bone's point into the element's 3D point and hands on to the game's own 3D widget update, which projects it,
// places the element and runs its usual update. The one for a line works out the place on the screen with the
// game's projection and calls the Lua function.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aae_plugin.h"
#include "aae_skeleton.h"
#include "aae_vmwidget.h"
#include "dobby.h"
#include "offsets.h"
#include "prepatch.h"

// the sums below are the plugin's, in its order: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Default_Tag "tag_weapon_right"
#define Generic_Method_Name "setupGeneric3DWidget"
#define Offset_Most 1000000.0f   // an offset further from 0 than this counts as 0
#define Coordinate_Most 10000000.0f // a point or screen place must be nearer to 0 than this
#define Turn_Scale 2.0f
#define Rotation_Least 0.25f // a bone's rotation (quaternion) must have a squared length between these
#define Rotation_Most 4.0f
#define Bones_Most 0x180    // a model with more bones than this, or none, is left alone
#define Probes 0x800        // the plugin looks at no more places than this in a table, or entries in the method list
#define No_Lookup (-2)
#define Not_Found (-1)
#define Lua_Registry (-10000)
#define String_Type_Plugin 3 // the kind of script string the plugin asks for, with user 0
#define All_Parts 0xffff

// the game's model (DObj) and its skeleton
#define Model_Tree 0x18
#define Model_Skeleton 0x58     // pointer to 0x20 bytes per bone: rotation at +0, position at +0x10
#define Model_Computed 0xc8     // bit 0x80000000 >> (bone & 31) of word bone >> 5: the bone has been computed
#define Model_Bones 0x120       // uint16_t
#define Bone_Size 0x20
#define Bone_Position 0x10
// a UI element
#define Element_Point 0x78   // the 3D point setupGeneric3DWidget sets, three floats
#define Element_Update 0x110 // its update function
// a HavokScript table: its array part
#define Table_Array 0x28
#define Table_Array_Size 0x30
#define Hks_Table 5
#define Hks_Function_Lua 6
#define Hks_Function_C 9
#define View_Weapon_State_Size 0x3a0 // the game's per local client view weapon state, the weapon model's DObj at +0

// ---- the plugin's rules, without the game

static bool is_finite(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7fffffff) < 0x7f800000;
}

// the plugin's test for a value it hands on: finite, and nearer to 0 than 10,000,000
static bool within(float value) {
    return is_finite(value) && fabsf(value) < Coordinate_Most;
}

// a HavokScript function: a Lua one, or one of the two kinds the C side has
static bool is_function(uint32_t type) {
    uint32_t kind = type & 0xf;
    return kind == Hks_Function_Lua || kind - Hks_Function_C <= 1;
}

uint32_t aae_vmwidget_tag_slot(uint32_t name) {
    return (name * (uint32_t)-0x4f) & (AAE_VMWIDGET_TAG_SLOTS - 1);
}

uint32_t aae_vmwidget_element_slot(const void *element) {
    uint64_t address = (uint64_t)(uintptr_t)element, mixed = (address >> 13) ^ address;
    return (uint32_t)((mixed >> 31) ^ (mixed >> 4)) & (AAE_VMWIDGET_ELEMENT_SLOTS - 1);
}

// The tests' chance to look at a bone while it is being written; nothing in the game.
#ifndef VMWIDGET_WHILE_WRITING
#define VMWIDGET_WHILE_WRITING(bone)
#endif

// the bone of a tag for one local client is not to be used: written as the plugin writes it, the count odd in between
static void invalidate(aae_vmwidget_bone_t *bone) {
    uint32_t sequence = __atomic_load_n(&bone->sequence, __ATOMIC_RELAXED);
    __atomic_store_n(&bone->sequence, sequence + 1, __ATOMIC_RELAXED);
    VMWIDGET_WHILE_WRITING(bone);
    __atomic_store_n(&bone->valid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&bone->sequence, sequence + 2, __ATOMIC_RELEASE);
}

static void forget_lookup(aae_vmwidget_lookup_t *lookup) {
    lookup->model = NULL;
    lookup->tree = NULL;
    lookup->index = No_Lookup;
}

aae_vmwidget_tag_t *aae_vmwidget_tag(aae_vmwidget_state_t *state, uint32_t name) {
    uint32_t slot = aae_vmwidget_tag_slot(name);
    for (uint32_t probe = 0; probe < AAE_VMWIDGET_TAG_SLOTS; probe++) {
        uint16_t entry = state->tag_slot[slot];
        if (entry == 0) {
            if (state->tag_count >= AAE_VMWIDGET_TAGS)
                return NULL;
            aae_vmwidget_tag_t *tag = &state->tag[state->tag_count];
            state->tag_count++;
            state->tag_slot[slot] = (uint16_t)state->tag_count;
            tag->name = name;
            for (int client = 0; client < AAE_VMWIDGET_LOCAL_CLIENTS; client++)
                tag->lookup[client].index = No_Lookup;
            return tag;
        }
        if (state->tag[entry - 1].name == name)
            return &state->tag[entry - 1];
        slot = (slot + 1) & (AAE_VMWIDGET_TAG_SLOTS - 1);
    }
    return NULL;
}

// The record of an element in one of the two tables of widgets (by the plugin's way: open addressing, places
// numbered from 1); made when there's none, if make. The tables have twice as many places as records, so a free
// place is always found before the probes run out.
static uint16_t *element_place(uint16_t *slots, const void *const *keys, size_t key_stride, const void *element,
                               bool *found) {
    uint32_t slot = aae_vmwidget_element_slot(element);
    for (uint32_t probe = 0; probe < Probes; probe++) {
        uint16_t entry = slots[slot];
        if (entry == 0) {
            *found = false;
            return &slots[slot];
        }
        const void *key = *(const void *const *)((const uint8_t *)keys + (size_t)(entry - 1) * key_stride);
        if (key == element) {
            *found = true;
            return &slots[slot];
        }
        slot = (slot + 1) & (AAE_VMWIDGET_ELEMENT_SLOTS - 1);
    }
    *found = false;
    return NULL;
}

static aae_vmwidget_widget_t *widget_of(aae_vmwidget_state_t *state, const void *element, bool make) {
    bool found;
    uint16_t *place = element_place(state->widget_slot, (const void *const *)&state->widget[0].element,
                                    sizeof(state->widget[0]), element, &found);
    if (place == NULL)
        return NULL;
    if (found)
        return &state->widget[*place - 1];
    if (!make || state->widget_count >= AAE_VMWIDGET_WIDGETS)
        return NULL;
    aae_vmwidget_widget_t *widget = &state->widget[state->widget_count];
    state->widget_count++;
    *place = (uint16_t)state->widget_count;
    widget->element = element;
    return widget;
}

static aae_vmwidget_line_t *line_of(aae_vmwidget_state_t *state, const void *element, bool make) {
    bool found;
    uint16_t *place = element_place(state->line_slot, (const void *const *)&state->line[0].element,
                                    sizeof(state->line[0]), element, &found);
    if (place == NULL)
        return NULL;
    if (found)
        return &state->line[*place - 1];
    if (!make || state->line_count >= AAE_VMWIDGET_WIDGETS)
        return NULL;
    aae_vmwidget_line_t *line = &state->line[state->line_count];
    state->line_count++;
    *place = (uint16_t)state->line_count;
    line->element = element;
    line->function_reference = No_Lookup;
    line->table_reference = No_Lookup;
    memset(&line->function, 0, sizeof(line->function));
    memset(&line->table, 0, sizeof(line->table));
    return line;
}

aae_vmwidget_widget_t *aae_vmwidget_widget(aae_vmwidget_state_t *state, const void *element) {
    return widget_of(state, element, true);
}

aae_vmwidget_line_t *aae_vmwidget_line(aae_vmwidget_state_t *state, const void *element) {
    return line_of(state, element, true);
}

// LUI starts up: a new Lua VM is made, and the one the lines were set up in is gone, its registry with it.
//
// The plugin doesn't do this. It keeps a line's Lua state and its two registry references until the element is set
// up again, and then, if that is from the same Lua state, gives those references back after it has taken the new
// ones. LUI starts up again at every map's start and at map_restart; the new Lua state is made at the same address
// as the old one, and its first references come out with the same numbers (both seen in the game: the state at
// 0xe675b0188 and the line's references 516 and 517, before and after a map_restart). The elements come from a pool
// that start-up empties and hands out again, so an element of the new VM can be at the address of a line from
// before. Set up as a line, the plugin would then give 516 and 517 back in the new VM: the references that line has
// just taken for itself. Its function and table would be left to the garbage collector while the line still calls
// them, and the next luaL_ref would hand the same numbers out a second time. So here the lines forget the Lua state
// and the references (and what they held) when LUI starts up. The rest of each line stays as the plugin keeps it:
// its element, its tag and offsets (the tag keeps its user, as with the plugin).
// Returns how many lines had a Lua state to forget.
int aae_vmwidget_forget_lines(aae_vmwidget_state_t *state) {
    int forgotten = 0;
    for (uint32_t i = 0; i < state->line_count && i < AAE_VMWIDGET_WIDGETS; i++) {
        aae_vmwidget_line_t *line = &state->line[i];
        if (line->lua != NULL)
            forgotten++;
        line->lua = NULL;
        line->function_reference = No_Lookup;
        line->table_reference = No_Lookup;
        memset(&line->function, 0, sizeof(line->function));
        memset(&line->table, 0, sizeof(line->table));
    }
    return forgotten;
}

void aae_vmwidget_move(aae_vmwidget_state_t *state, aae_vmwidget_tag_t **tag, aae_vmwidget_tag_t *to) {
    aae_vmwidget_tag_t *from = *tag;
    if (from == to)
        return;
    if (from != NULL) {
        size_t which = (size_t)(from - state->tag);
        if (which < AAE_VMWIDGET_TAGS && atomic_load_explicit(&state->users[which], memory_order_relaxed) != 0 &&
            atomic_fetch_sub(&state->users[which], 1) == 1)
            atomic_fetch_and(&state->active[which >> 6], ~((uint64_t)1 << (which & 63)));
    }
    if (to != NULL) {
        size_t which = (size_t)(to - state->tag);
        if (which < AAE_VMWIDGET_TAGS && atomic_fetch_add(&state->users[which], 1) == 0) {
            // its first user: what was kept of its bones is from before and is looked for again
            for (int client = 0; client < AAE_VMWIDGET_LOCAL_CLIENTS; client++) {
                invalidate(&to->bone[client]);
                forget_lookup(&to->lookup[client]);
            }
            atomic_fetch_or(&state->active[which >> 6], (uint64_t)1 << (which & 63));
        }
    }
    *tag = to;
}

float aae_vmwidget_offset(const HksObject *base, const HksObject *top, int argument) {
    if (top == NULL || base == NULL)
        return 0.0f;
    const HksObject *value = base + (argument - 1);
    // the whole type word is compared, not only its kind
    if (!(value < top) || value->t != HKS_TNUMBER)
        return 0.0f;
    float number = value->v.number;
    if (!is_finite(number) || fabsf(number) > Offset_Most)
        return 0.0f;
    return number;
}

// The tests' chance to change a bone between the copy and the second look at its count, as a writer on another
// thread can; nothing in the game.
#ifndef VMWIDGET_BETWEEN_READS
#define VMWIDGET_BETWEEN_READS(bone)
#endif

bool aae_vmwidget_read(const aae_vmwidget_tag_t *tag, int local_client, aae_vmwidget_bone_t *copy) {
    const aae_vmwidget_bone_t *bone = &tag->bone[local_client];
    uint32_t before = __atomic_load_n(&bone->sequence, __ATOMIC_ACQUIRE);
    if (before & 1)
        return false;
    if (__atomic_load_n(&bone->valid, __ATOMIC_RELAXED) == 0)
        return false;
    memcpy(copy->position, bone->position, sizeof(copy->position));
    memcpy(copy->rotation, bone->rotation, sizeof(copy->rotation));
    VMWIDGET_BETWEEN_READS(bone);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t after = __atomic_load_n(&bone->sequence, __ATOMIC_RELAXED);
    if (after != before || (after & 1))
        return false;
    copy->sequence = after;
    copy->valid = 1;
    return true;
}

bool aae_vmwidget_point(const aae_vmwidget_bone_t *bone, const float offset[3], float point[3]) {
    float x = offset[0], y = offset[1], z = offset[2];
    if (x == 0.0f && y == 0.0f && z == 0.0f) {
        memcpy(point, bone->position, 3 * sizeof(float));
        return true;
    }
    float qx = bone->rotation[0], qy = bone->rotation[1], qz = bone->rotation[2], qw = bone->rotation[3];
    float cx = qy * z - qz * y;
    float cy = qz * x - qx * z;
    float cz = qx * y - qy * x;
    // the rotation as it is kept, not made a unit one first: the scale takes its length out
    float scale = Turn_Scale / (((qy * qy + qx * qx) + qz * qz) + qw * qw);
    point[0] = (((cz * qy - cy * qz) + cx * qw) * scale + x) + bone->position[0];
    point[1] = (((cx * qz - cz * qx) + cy * qw) * scale + y) + bone->position[1];
    point[2] = (((cy * qx - cx * qy) + cz * qw) * scale + z) + bone->position[2];
    return within(point[0]) && within(point[1]) && within(point[2]);
}

bool aae_vmwidget_on_screen(const float screen[2]) {
    return within(screen[0]) && within(screen[1]);
}

// a local client the plugin keeps bones for: 0 to 3
static bool local_client_kept(int local_client) {
    return local_client >= 0 && local_client < AAE_VMWIDGET_LOCAL_CLIENTS;
}

// the active tags, in the plugin's order, as long as each is one of those made
#define for_each_active_tag(state, which)                                                                              \
    for (uint32_t word_ = 0; word_ < AAE_VMWIDGET_TAGS / 64; word_++)                                                  \
        for (uint64_t bits_ = atomic_load_explicit(&(state)->active[word_], memory_order_relaxed),                     \
                      which = 0;                                                                                       \
             bits_ != 0 && ((which = word_ * 64 + (uint32_t)__builtin_ctzll(bits_)), bits_ &= bits_ - 1, true);)   \
            if (which < (state)->tag_count)

void aae_vmwidget_note(aae_vmwidget_state_t *state, int local_client, const uint8_t *model) {
    if ((unsigned)local_client >= AAE_VMWIDGET_LOCAL_CLIENTS)
        return;
    const void *tree = NULL;
    if (model != NULL)
        memcpy(&tree, model + Model_Tree, sizeof(tree));
    if (state->noted[local_client].model == model && state->noted[local_client].tree == tree)
        return;
    state->noted[local_client].model = NULL;
    for_each_active_tag(state, which) {
        invalidate(&state->tag[which].bone[local_client]);
        forget_lookup(&state->tag[which].lookup[local_client]);
    }
    state->noted[local_client].tree = tree;
    state->noted[local_client].model = model;
}

// The tag's bone in the model: looked for again when the model or its tree isn't the one it was looked for in. A
// bone that isn't there invalidates what was kept of it.
static int16_t bone_of(aae_vmwidget_tag_t *tag, int client, const uint8_t *model, const void *tree, uint16_t bones,
                       aae_vmwidget_bone_index_t bone_index) {
    aae_vmwidget_lookup_t *lookup = &tag->lookup[client];
    if (lookup->model != model || lookup->tree != tree) {
        int16_t index = No_Lookup;
        int found = bone_index(model, tag->name, &index, All_Parts);
        if (found == 0 || index < 0 || index >= (int16_t)bones)
            index = Not_Found;
        lookup->model = model;
        lookup->tree = tree;
        lookup->index = index;
        if (index < 0)
            invalidate(&tag->bone[client]);
    }
    return lookup->index;
}

void aae_vmwidget_skeleton(aae_vmwidget_state_t *state, const uint8_t *model, aae_vmwidget_bone_index_t bone_index) {
    if ((atomic_load_explicit(&state->active[0], memory_order_relaxed) |
         atomic_load_explicit(&state->active[1], memory_order_relaxed)) == 0)
        return;
    // the first local client whose weapon model this is
    int client = 0;
    while (client < AAE_VMWIDGET_LOCAL_CLIENTS && state->noted[client].model != model)
        client++;
    if (client == AAE_VMWIDGET_LOCAL_CLIENTS || model == NULL || bone_index == NULL)
        return;
    const void *tree;
    const uint8_t *skeleton;
    uint16_t bones;
    memcpy(&tree, model + Model_Tree, sizeof(tree));
    if (tree == NULL || state->noted[client].tree != tree)
        return;
    memcpy(&bones, model + Model_Bones, sizeof(bones));
    if ((uint16_t)(bones - 1) >= Bones_Most)
        return;
    memcpy(&skeleton, model + Model_Skeleton, sizeof(skeleton));
    if (skeleton == NULL)
        return;
    for_each_active_tag(state, which) {
        aae_vmwidget_tag_t *tag = &state->tag[which];
        int16_t index = bone_of(tag, client, model, tree, bones, bone_index);
        if (index < 0 || index >= (int16_t)bones)
            continue;
        uint32_t computed;
        memcpy(&computed, model + Model_Computed + (size_t)(index >> 5) * 4, sizeof(computed));
        if ((computed & (0x80000000u >> (index & 31))) == 0)
            continue;
        float rotation[4], position[3];
        memcpy(rotation, skeleton + (size_t)index * Bone_Size, sizeof(rotation));
        memcpy(position, skeleton + (size_t)index * Bone_Size + Bone_Position, sizeof(position));
        if (!is_finite(rotation[0]) || !is_finite(rotation[1]) || !is_finite(rotation[2]) || !is_finite(rotation[3]))
            continue;
        float length = ((rotation[0] * rotation[0] + rotation[1] * rotation[1]) + rotation[2] * rotation[2]) +
                       rotation[3] * rotation[3];
        if (!is_finite(length) || !(length > Rotation_Least) || !(Rotation_Most > length))
            continue;
        if (!within(position[0]) || !within(position[1]) || !within(position[2]))
            continue;
        aae_vmwidget_bone_t *bone = &tag->bone[client];
        uint32_t sequence = __atomic_load_n(&bone->sequence, __ATOMIC_RELAXED);
        __atomic_store_n(&bone->sequence, sequence + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        VMWIDGET_WHILE_WRITING(bone);
        memcpy(bone->rotation, rotation, sizeof(rotation));
        memcpy(bone->position, position, sizeof(position));
        __atomic_store_n(&bone->valid, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&bone->sequence, sequence + 2, __ATOMIC_RELEASE);
    }
}

bool aae_vmwidget_splice(aae_vmwidget_method_t *generic, lua_CFunction generic_function, aae_vmwidget_method_t *first,
                         aae_vmwidget_method_t *second, bool wanted) {
    if (generic->name == NULL || strcmp(generic->name, Generic_Method_Name) != 0 ||
        generic->function != generic_function)
        return false;
    if (!wanted) {
        // ours are only ever right behind the game's entry, but they are looked for along the whole list
        aae_vmwidget_method_t *before = generic;
        for (uint32_t step = 0; before->next != NULL && step < Probes; step++) {
            if (before->next == first || before->next == second)
                before->next = before->next->next;
            else
                before = before->next;
        }
        return false;
    }
    // the plugin's look along the list: ours are there already when the first is and is followed by the second;
    // with only the second there it leaves the list alone
    aae_vmwidget_method_t *entry = generic->next;
    for (uint32_t step = 0; entry != NULL && step < Probes; step++) {
        if (entry == first)
            return first->next == second;
        if (entry == second)
            return false;
        entry = entry->next;
    }
    first->next = second;
    second->next = generic->next;
    generic->next = first;
    return true;
}

// ---- the game

static aae_vmwidget_state_t state;
static bool ready;
static uint64_t base_address;
static int debug; // BO3MACFIX_VMWIDGETDEBUG

// the game's
static uint8_t *(*LUI_ToElement)(lua_State *s, int index);
static const char *(*hks_obj_tolstring)(lua_State *s, HksObject *object, size_t *length);
static uint32_t (*SL_GetStringOfSize)(const char *text, uint32_t user, uint32_t length, int type);
static void (*hksi_lua_createtable)(lua_State *s, int array, int hash);
static int (*hksi_luaL_ref)(lua_State *s, int table);
static void (*hksi_luaL_unref)(lua_State *s, int table, int reference);
static int (*hksi_lua_pcall)(lua_State *s, int arguments, int results, int handler);
static aae_vmwidget_update_t LUI_Generic3DUpdate;
static void (*CG_WorldToScreen)(int local_client, const float world[3], float screen[2]);
static int (*DObjGetBoneIndex)(const uint8_t *model, uint32_t name, int16_t *index, uint32_t parts);
static aae_vmwidget_method_t *generic_method;
static lua_CFunction generic_function;
static uint8_t *const *view_weapon_states;
static const int *local_client_count;

static uint64_t now_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}

// The tag a method names: argument 2 as HavokScript makes it a text (a number becomes its text, there on the stack),
// when it gives one that isn't empty.
static const char *tag_name(lua_State *s, HksObject *base, HksObject *top) {
    const char *name = Default_Tag;
    if (top != NULL && base != NULL && base + 1 < top) {
        const char *text = hks_obj_tolstring(s, base + 1, NULL);
        if (text != NULL && text[0] != '\0')
            name = text;
    }
    return name;
}

// the plugin's SL_GetString(name, 0, 3): the length the game is given counts the closing 0
static uint32_t tag_string(const char *name) {
    return SL_GetStringOfSize(name, 0, (uint32_t)strlen(name) + 1, String_Type_Plugin);
}

// for the debug lines: each tag's name as a method was given it
static char debug_names[AAE_VMWIDGET_TAGS][32];
static void debug_name(const aae_vmwidget_tag_t *tag, const char *name) {
    if (debug)
        snprintf(debug_names[tag - state.tag], sizeof(debug_names[0]), "%s", name);
}

static void release(lua_State *s, int reference) {
    if (hksi_luaL_unref != NULL)
        hksi_luaL_unref(s, Lua_Registry, reference);
}

static void set_update(uint8_t *element, aae_vmwidget_update_t update) {
    memcpy(element + Element_Update, &update, sizeof(update));
}

static void widget_update(int local_client, uint8_t *element, uint64_t root, uint64_t fourth, uint64_t fifth,
                          lua_State *s);
static void line_update(int local_client, uint8_t *element, uint64_t root, uint64_t fourth, uint64_t fifth,
                        lua_State *s);

// element:setupViewmodelTagWidget(tag, x, y, z)
static int lua_tag_widget(lua_State *s) {
    if (!ready || s == NULL)
        return 0;
    uint8_t *element = LUI_ToElement(s, 1);
    if (element == NULL)
        return 0;
    const char *name = tag_name(s, HKS_BASE(s), HKS_TOP(s));
    uint32_t string = tag_string(name);
    if (string == 0)
        return 0;
    aae_vmwidget_tag_t *tag = aae_vmwidget_tag(&state, string);
    if (tag == NULL)
        return 0;
    debug_name(tag, name);
    aae_vmwidget_widget_t *widget = widget_of(&state, element, true);
    if (widget == NULL)
        return 0;
    const HksObject *top = HKS_TOP(s);
    float offset[3] = { 0.0f, 0.0f, 0.0f };
    if (top != NULL) {
        offset[0] = aae_vmwidget_offset(HKS_BASE(s), top, 3);
        offset[1] = aae_vmwidget_offset(HKS_BASE(s), top, 4);
        offset[2] = aae_vmwidget_offset(HKS_BASE(s), top, 5);
    }
    aae_vmwidget_move(&state, &widget->tag, tag);
    memcpy(widget->offset, offset, sizeof(offset));
    set_update(element, widget_update);
    if (debug)
        printf("viewmodel widgets: setupViewmodelTagWidget: element %p sits on %s (string %u), moved by %g %g %g\n",
               (void *)element, name, string, (double)offset[0], (double)offset[1], (double)offset[2]);
    return 0;
}

// element:setupViewmodelTagWidgetLine(tag, x, y, z, callback)
static int lua_tag_widget_line(lua_State *s) {
    if (!ready || s == NULL)
        return 0;
    uint8_t *element = LUI_ToElement(s, 1);
    if (element == NULL)
        return 0;
    HksObject *top = HKS_TOP(s), *base = HKS_BASE(s);
    if (top == NULL || base == NULL)
        return 0;
    const char *name = tag_name(s, base, top);
    top = HKS_TOP(s), base = HKS_BASE(s);
    if (top == NULL || base == NULL)
        return 0;
    HksObject function;
    if (!(base + 5 < top))
        return 0;
    memcpy(&function, base + 5, sizeof(function));
    if (!is_function(function.t))
        return 0;
    uint32_t string = tag_string(name);
    if (string == 0)
        return 0;
    aae_vmwidget_tag_t *tag = aae_vmwidget_tag(&state, string);
    if (tag == NULL)
        return 0;
    debug_name(tag, name);
    top = HKS_TOP(s);
    float offset[3] = { 0.0f, 0.0f, 0.0f };
    if (top != NULL) {
        offset[0] = aae_vmwidget_offset(HKS_BASE(s), top, 3);
        offset[1] = aae_vmwidget_offset(HKS_BASE(s), top, 4);
        offset[2] = aae_vmwidget_offset(HKS_BASE(s), top, 5);
    }
    if (hksi_luaL_ref == NULL || top == NULL)
        return 0;
    // a reference keeps the function alive, and a table of two made now is what it will be handed
    memcpy(top, &function, sizeof(function));
    HKS_TOP(s) = top + 1;
    int function_reference = hksi_luaL_ref(s, Lua_Registry);
    if (function_reference < 0)
        return 0;
    if (hksi_lua_createtable == NULL) {
        release(s, function_reference);
        return 0;
    }
    hksi_lua_createtable(s, 2, 0);
    HksObject table;
    if (HKS_TOP(s) == NULL) {
        release(s, function_reference);
        return 0;
    }
    memcpy(&table, HKS_TOP(s) - 1, sizeof(table));
    if ((table.t & 0xf) != Hks_Table || table.v.ptr == NULL) {
        release(s, function_reference);
        return 0;
    }
    int table_reference = hksi_luaL_ref(s, Lua_Registry);
    if (table_reference < 0) {
        release(s, function_reference);
        return 0;
    }
    aae_vmwidget_line_t *line = line_of(&state, element, true);
    if (line == NULL) {
        release(s, function_reference);
        release(s, table_reference);
        return 0;
    }
    // The references an element set up before had are given back, when it was set up from this same Lua state (the
    // plugin's rule; a line from a Lua VM that is gone has forgotten it, see aae_vmwidget_forget_lines).
    if (line->lua == s && hksi_luaL_unref != NULL) {
        if (debug)
            printf("viewmodel widgets: setupViewmodelTagWidgetLine: element %p was set up before from this Lua state: "
                   "its references %d and %d are given back\n", (void *)element, line->function_reference,
                   line->table_reference);
        if (line->function_reference >= 0)
            hksi_luaL_unref(s, Lua_Registry, line->function_reference);
        if (hksi_luaL_unref != NULL && line->table_reference >= 0)
            hksi_luaL_unref(s, Lua_Registry, line->table_reference);
    }
    aae_vmwidget_update_t current;
    memcpy(&current, element + Element_Update, sizeof(current));
    if (current != line_update)
        line->before = current;
    aae_vmwidget_move(&state, &line->tag, tag);
    line->function_reference = function_reference;
    line->lua = s;
    memcpy(line->offset, offset, sizeof(offset));
    line->table_reference = table_reference;
    line->off = 0;
    line->calling = 0;
    memcpy(&line->function, &function, sizeof(function));
    memcpy(&line->table, &table, sizeof(table));
    set_update(element, line_update);
    if (debug)
        printf("viewmodel widgets: setupViewmodelTagWidgetLine: element %p sits on %s (string %u), moved by %g %g %g, "
               "function %d, table %d, Lua state %p\n", (void *)element, name, string, (double)offset[0],
               (double)offset[1], (double)offset[2], function_reference, table_reference, (void *)s);
    return 0;
}

// For the debug lines: whether an element had a point last time, so that only changes are written, and when its
// place was last written (BO3MACFIX_VMWIDGETDEBUG=2).
static struct {
    const void *element;
    int had;
    uint64_t said;
} debug_seen[64];
static void debug_point(const char *kind, const void *element, int has, const float point[3], const float *screen) {
    if (!debug)
        return;
    size_t place = aae_vmwidget_element_slot(element) % (sizeof(debug_seen) / sizeof(debug_seen[0]));
    uint64_t now = now_ms();
    if (debug_seen[place].element != element) {
        debug_seen[place].element = element;
        debug_seen[place].had = -1;
        debug_seen[place].said = 0;
    }
    if (debug_seen[place].had != has) {
        debug_seen[place].had = has;
        printf("viewmodel widgets: the %s on element %p %s\n", kind, element,
               has ? "has its bone's point" : "has no point (no bone kept, or none to be had)");
    }
    if (debug >= 2 && has && now - debug_seen[place].said >= 500) {
        debug_seen[place].said = now;
        if (screen != NULL)
            printf("viewmodel widgets: the %s on element %p is at %.2f %.2f %.2f, on the screen at %.1f %.1f\n", kind,
                   element, (double)point[0], (double)point[1], (double)point[2], (double)screen[0],
                   (double)screen[1]);
        else
            printf("viewmodel widgets: the %s on element %p is at %.2f %.2f %.2f\n", kind, element, (double)point[0],
                   (double)point[1], (double)point[2]);
    }
}

// The update of an element set up with setupViewmodelTagWidget. Without a point the element isn't updated at all
// this frame, as with the plugin: neither placed nor given its usual update.
static void widget_update(int local_client, uint8_t *element, uint64_t root, uint64_t fourth, uint64_t fifth,
                          lua_State *s) {
    if (element == NULL || LUI_Generic3DUpdate == NULL)
        return;
    aae_vmwidget_widget_t *widget = widget_of(&state, element, false);
    if (widget == NULL || widget->tag == NULL || !local_client_kept(local_client))
        return;
    aae_vmwidget_bone_t bone;
    float point[3];
    if (!aae_vmwidget_read(widget->tag, local_client, &bone) || !aae_vmwidget_point(&bone, widget->offset, point)) {
        debug_point("widget", element, 0, NULL, NULL);
        return;
    }
    memcpy(element + Element_Point, point, sizeof(point));
    debug_point("widget", element, 1, point, NULL);
    LUI_Generic3DUpdate(local_client, element, root, fourth, fifth, s);
}

// The update of an element set up with setupViewmodelTagWidgetLine.
static void line_update(int local_client, uint8_t *element, uint64_t root, uint64_t fourth, uint64_t fifth,
                        lua_State *s) {
    if (element == NULL)
        return;
    aae_vmwidget_line_t *line = line_of(&state, element, false);
    if (line == NULL)
        return;
    if (line->before != NULL && line->before != line_update)
        line->before(local_client, element, root, fourth, fifth, s);
    if (line->lua != s)
        return;
    // the place on the screen: what the projection leaves, from 0, 0 (its own answer isn't asked)
    float screen[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, point[3];
    bool placed = false;
    aae_vmwidget_bone_t bone;
    if (line->tag != NULL && CG_WorldToScreen != NULL && local_client_kept(local_client) &&
        aae_vmwidget_read(line->tag, local_client, &bone) && aae_vmwidget_point(&bone, line->offset, point)) {
        CG_WorldToScreen(local_client, point, screen);
        placed = aae_vmwidget_on_screen(screen);
    }
    debug_point("line", element, placed, point, screen);
    if (s == NULL || line->lua != s || line->off || line->calling || line->function_reference < 0 ||
        hksi_lua_pcall == NULL)
        return;
    HksObject *top = HKS_TOP(s);
    if (top == NULL)
        return;
    // The top as a place on the stack, for after the call: the mod's function can make the VM move its stack to a
    // bigger block (0x10145885e copies it and frees the old one). The plugin keeps the pointer itself, which then
    // points into the freed block, and the next push corrupts the heap. Until the call nothing can move the stack.
    ptrdiff_t top_at = top - HKS_BASE(s);
    if (!is_function(line->function.t)) {
        line->off = 1;
        return;
    }
    memcpy(top, &line->function, sizeof(line->function));
    HKS_TOP(s) = top + 1;
    if (placed) {
        // the table's first two places become the numbers, written straight into its array as the plugin does
        uint8_t *table = line->table.v.ptr, *array = NULL;
        uint32_t size = 0;
        if ((line->table.t & 0xf) == Hks_Table && table != NULL && is_finite(screen[0]) && is_finite(screen[1])) {
            memcpy(&array, table + Table_Array, sizeof(array));
            if (array != NULL)
                memcpy(&size, table + Table_Array_Size, sizeof(size));
        }
        if (array == NULL || size - 2 > 0xffffe) {
            HKS_TOP(s) = top;
            line->off = 1;
            return;
        }
        uint32_t bits[2];
        memcpy(&bits[0], &screen[0], sizeof(bits[0]));
        memcpy(&bits[1], &screen[1], sizeof(bits[1]));
        const uint64_t numbers[4] = { HKS_TNUMBER, bits[0], HKS_TNUMBER, bits[1] };
        memcpy(array, numbers, sizeof(numbers));
        HksObject *next = HKS_TOP(s);
        if (next == NULL) {
            HKS_TOP(s) = top;
            line->off = 1;
            return;
        }
        memcpy(next, &line->table, sizeof(line->table));
        HKS_TOP(s) = next + 1;
    } else {
        memset(top + 1, 0, sizeof(*top));
        HKS_TOP(s) = top + 2;
    }
    line->calling = 1;
    int failed = hksi_lua_pcall(s, 1, 0, 0);
    line->calling = 0;
    HKS_TOP(s) = HKS_BASE(s) + top_at;
    if (failed != 0) {
        line->off = 1;
        if (debug)
            printf("viewmodel widgets: the function of the line on element %p failed: switched off\n",
                   (void *)element);
    }
}

static aae_vmwidget_method_t tag_widget_line_method = { "setupViewmodelTagWidgetLine", lua_tag_widget_line, NULL };
static aae_vmwidget_method_t tag_widget_method = { "setupViewmodelTagWidget", lua_tag_widget, NULL };

// LUI starts up: a new Lua VM is made and the UI element methods are registered in it
install_hook_name(LUI_Startup, void, uint64_t first, uint64_t second) {
    int forgotten = aae_vmwidget_forget_lines(&state);
    if (debug && forgotten > 0)
        printf("viewmodel widgets: LUI starts up again: %d lines forget the Lua VM they were set up in and its "
               "references\n", forgotten);
    bool loaded = aae_plugin_loaded();
    bool there = aae_vmwidget_splice(generic_method, generic_function, &tag_widget_method, &tag_widget_line_method,
                                     loaded);
    if (debug)
        printf("viewmodel widgets: LUI starts up, the mod with its plugin is %s: the two methods are %s\n",
               loaded ? "loaded" : "not loaded", there ? "in the UI element methods" : "not there");
    orig_LUI_Startup(first, second);
}

// the weapon model of each local client, as the game has it right now
static void note_weapon_models(void) {
    const uint8_t *views = *view_weapon_states;
    if (views == NULL)
        return;
    int clients = *local_client_count;
    for (int client = 0; client < clients && client < AAE_VMWIDGET_LOCAL_CLIENTS; client++) {
        const uint8_t *model;
        memcpy(&model, views + (size_t)client * View_Weapon_State_Size, sizeof(model));
        // the plugin's note is only taken while there is a weapon model
        if (model != NULL)
            aae_vmwidget_note(&state, client, model);
    }
}

// For the debug lines: what each tag in use has from a local client's weapon model after its skeleton was computed:
// where its bone is, whether the game computed that bone this time, whether it was kept. A line when that changes.
static void debug_bones(const uint8_t *model, int call) {
    // kept for each of the two calls apart: they compute different bones of the model (one the right hand's, the
    // other not), so a record shared by both would change at every call
    static struct {
        const void *model;
        int16_t index;
        int8_t computed, valid;
    } said[2][AAE_VMWIDGET_TAGS];
    int client = 0;
    while (client < AAE_VMWIDGET_LOCAL_CLIENTS && state.noted[client].model != model)
        client++;
    if (client == AAE_VMWIDGET_LOCAL_CLIENTS)
        return;
    uint16_t bones;
    memcpy(&bones, model + Model_Bones, sizeof(bones));
    for_each_active_tag(&state, which) {
        const aae_vmwidget_tag_t *tag = &state.tag[which];
        int16_t index = tag->lookup[client].index;
        int8_t computed = -1, valid = (int8_t)__atomic_load_n(&tag->bone[client].valid, __ATOMIC_RELAXED);
        if (index >= 0 && index < (int16_t)bones) {
            uint32_t bits;
            memcpy(&bits, model + Model_Computed + (size_t)(index >> 5) * 4, sizeof(bits));
            computed = (bits & (0x80000000u >> (index & 31))) != 0;
        }
        typeof(said[0][0]) *seen = &said[call][which];
        if (seen->model == model && seen->index == index && seen->computed == computed && seen->valid == valid)
            continue;
        seen->model = model, seen->index = index;
        seen->computed = computed, seen->valid = valid;
        char where[48];
        if (index >= 0)
            snprintf(where, sizeof(where), "bone %d of %u, %s", index, bones, computed > 0 ? "computed" : "not computed");
        else if (index == No_Lookup)
            snprintf(where, sizeof(where), "not looked for");
        else
            snprintf(where, sizeof(where), "not found among %u bones", bones);
        printf("viewmodel widgets: after the skeleton call %d, %s (tag %u) in the weapon model %p of local client %d: "
               "%s, %s\n", call + 1, debug_names[which][0] != '\0' ? debug_names[which] : "?", (unsigned)which,
               (const void *)model, client, where, valid ? "kept" : "not kept");
    }
}

// The game has computed a model's skeleton at one of the two calls the plugin redirects (call 0 or 1, aae_skeleton.c
// tells them apart): the plugin's work after them. Nothing is done for no model, and nothing while no widget uses a
// tag, which is all the time without the mod: the methods that would set one up only exist with it.
static void widgets_after_skeleton(const uint8_t *model, int call) {
    if (model == NULL || !ready ||
        (atomic_load_explicit(&state.active[0], memory_order_relaxed) |
         atomic_load_explicit(&state.active[1], memory_order_relaxed)) == 0)
        return;
    note_weapon_models();
    aae_vmwidget_skeleton(&state, model, DObjGetBoneIndex);
    if (debug && (call == 0 || call == 1)) {
        debug_bones(model, call);
        static _Atomic int said;
        if ((atomic_fetch_or(&said, 1 << call) & (1 << call)) == 0)
            printf("viewmodel widgets: bones are kept after the game's skeleton call %d, on %s thread\n", call + 1,
                   pthread_main_np() ? "the main" : "another");
    }
}

void aae_vmwidget_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    const char *env = getenv("BO3MACFIX_VMWIDGETDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    aae_plugin_init(game_base_address);
    LUI_ToElement = (void *)(base_address + ADDR_LUI_ToElement);
    hks_obj_tolstring = (void *)(base_address + ADDR_hks_obj_tolstring);
    SL_GetStringOfSize = (void *)(base_address + ADDR_SL_GetStringOfSize);
    hksi_lua_createtable = (void *)(base_address + ADDR_hksi_lua_createtable);
    hksi_luaL_ref = (void *)(base_address + ADDR_hksi_luaL_ref);
    hksi_luaL_unref = (void *)(base_address + ADDR_hksi_luaL_unref);
    hksi_lua_pcall = (void *)(base_address + ADDR_hksi_lua_pcall);
    LUI_Generic3DUpdate = (aae_vmwidget_update_t)(base_address + ADDR_LUI_Generic3DUpdate);
    CG_WorldToScreen = (void *)(base_address + ADDR_CG_WorldToScreen);
    DObjGetBoneIndex = (void *)(base_address + ADDR_DObjGetBoneIndex);
    generic_method = (aae_vmwidget_method_t *)(base_address + ADDR_LUI_Generic3DMethod);
    generic_function = (lua_CFunction)(base_address + ADDR_Lua_SetupGeneric3DWidget);
    view_weapon_states = (uint8_t *const *)(base_address + ADDR_cgViewWeaponStates);
    local_client_count = (const int *)(base_address + ADDR_LOCAL_CLIENT_COUNT);
    ready = true;
    install_hook_LUI_Startup((void *)(base_address + ADDR_LUI_Startup));
    aae_skeleton_add(base_address, AAE_SKELETON_WIDGETS, widgets_after_skeleton);
}
