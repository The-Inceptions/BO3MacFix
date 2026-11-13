// All-around Enhancement 4's widgets on the first person weapon: the two UI element methods its Windows plugin adds
// (AAEViewmodelWidgets). See aae_vmwidget.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "havok.h"

void aae_vmwidget_init(uint64_t game_base_address, int enabled);

// ---- what the plugin keeps, and its rules, without the game (for the tests)

#define AAE_VMWIDGET_LOCAL_CLIENTS 4
#define AAE_VMWIDGET_TAGS 128      // tags the widgets can sit on
#define AAE_VMWIDGET_TAG_SLOTS 256 // places of the tags' lookup table
#define AAE_VMWIDGET_WIDGETS 1024  // elements of each kind of widget
#define AAE_VMWIDGET_ELEMENT_SLOTS 2048

// A bone of the weapon model as it was last computed for one local client. sequence is odd while it is being
// written: a reader that sees it odd, or sees it change while it reads, has a torn copy.
typedef struct {
    uint32_t sequence;
    uint32_t valid;
    float position[3];
    float rotation[4]; // a quaternion: x, y, z, w
} aae_vmwidget_bone_t;

// Where a tag's bone is in the weapon model a local client had when it was looked for.
typedef struct {
    const void *model; // the model's DObj
    const void *tree;  // and that DObj's tree, which changes with the model's parts
    int16_t index;     // -2: not looked for yet, -1: the model has no such bone
} aae_vmwidget_lookup_t;

typedef struct {
    uint32_t name; // the tag's script string
    aae_vmwidget_bone_t bone[AAE_VMWIDGET_LOCAL_CLIENTS];
    aae_vmwidget_lookup_t lookup[AAE_VMWIDGET_LOCAL_CLIENTS];
} aae_vmwidget_tag_t;

// what the game calls to update a UI element each frame: (local client, element, the root element, two numbers, the
// UI's lua_State)
typedef void (*aae_vmwidget_update_t)(int local_client, uint8_t *element, uint64_t root, uint64_t fourth,
                                      uint64_t fifth, lua_State *s);

// an element set up with setupViewmodelTagWidget
typedef struct {
    const void *element;
    aae_vmwidget_tag_t *tag;
    float offset[3];
} aae_vmwidget_widget_t;

// an element set up with setupViewmodelTagWidgetLine
typedef struct {
    const void *element;
    aae_vmwidget_tag_t *tag;
    lua_State *lua;                 // the Lua state that set it up
    aae_vmwidget_update_t before;   // the element's own update from before, called first each frame
    float offset[3];
    int32_t function_reference;     // the Lua function's registry reference, -2 for none yet
    int32_t table_reference;        // the table it is handed, -2 for none yet
    uint32_t off;                   // switched off by an error of its function
    uint32_t calling;               // its function is being called
    HksObject function, table;      // their values, as they were on the stack
} aae_vmwidget_line_t;

typedef struct {
    aae_vmwidget_tag_t tag[AAE_VMWIDGET_TAGS];
    uint32_t tag_count;
    uint16_t tag_slot[AAE_VMWIDGET_TAG_SLOTS]; // a tag's place + 1, 0 for none
    _Atomic int32_t users[AAE_VMWIDGET_TAGS]; // widgets on each tag
    _Atomic uint64_t active[AAE_VMWIDGET_TAGS / 64]; // the tags with users, whose bones are kept up to date
    aae_vmwidget_widget_t widget[AAE_VMWIDGET_WIDGETS];
    uint32_t widget_count;
    uint16_t widget_slot[AAE_VMWIDGET_ELEMENT_SLOTS];
    aae_vmwidget_line_t line[AAE_VMWIDGET_WIDGETS];
    uint32_t line_count;
    uint16_t line_slot[AAE_VMWIDGET_ELEMENT_SLOTS];
    struct {
        const void *model, *tree;
    } noted[AAE_VMWIDGET_LOCAL_CLIENTS]; // each local client's weapon model, as last seen
} aae_vmwidget_state_t;

// where a tag starts looking in the tags' table, and an element in the widgets' tables
uint32_t aae_vmwidget_tag_slot(uint32_t name);
uint32_t aae_vmwidget_element_slot(const void *element);
// the tag with this script string, made when there's none and there is room; NULL when there isn't
aae_vmwidget_tag_t *aae_vmwidget_tag(aae_vmwidget_state_t *state, uint32_t name);
// the widget of this element, made when there's none and there is room; NULL when there isn't
aae_vmwidget_widget_t *aae_vmwidget_widget(aae_vmwidget_state_t *state, const void *element);
aae_vmwidget_line_t *aae_vmwidget_line(aae_vmwidget_state_t *state, const void *element);
// a widget moves from the tag in *tag to this one: the old one loses a user, the new one gains one
void aae_vmwidget_move(aae_vmwidget_state_t *state, aae_vmwidget_tag_t **tag, aae_vmwidget_tag_t *to);

// Argument number `argument` of a Lua call (base and top are its stack's ends) as an offset: the number, when it is
// there, is a number, is finite and isn't further from 0 than 1,000,000; 0 otherwise.
float aae_vmwidget_offset(const HksObject *base, const HksObject *top, int argument);
// A copy of the bone a tag has for this local client, unless it isn't valid or is being written.
bool aae_vmwidget_read(const aae_vmwidget_tag_t *tag, int local_client, aae_vmwidget_bone_t *copy);
// The point a widget is at: the bone's position, plus the offset turned by the bone's rotation unless the offset is
// all 0. false when that isn't a finite point within 10,000,000 units.
bool aae_vmwidget_point(const aae_vmwidget_bone_t *bone, const float offset[3], float point[3]);
// whether a place on the screen the game worked out can be handed on: both finite and within 10,000,000
bool aae_vmwidget_on_screen(const float screen[2]);

// The game is about to compute a model's skeleton for this local client's weapon, or has: when the model or its tree
// isn't the one last seen, every active tag's bone for that client is invalidated and looked for again.
void aae_vmwidget_note(aae_vmwidget_state_t *state, int local_client, const uint8_t *model);
// what the game's DObjGetBoneIndex does: (model, script string, *index in and out, which parts), nonzero when found
typedef int (*aae_vmwidget_bone_index_t)(const uint8_t *model, uint32_t name, int16_t *index, uint32_t parts);
// The game has computed this model's skeleton: when it is a local client's weapon model, every active tag's bone is
// copied from it.
void aae_vmwidget_skeleton(aae_vmwidget_state_t *state, const uint8_t *model, aae_vmwidget_bone_index_t bone_index);

// The game's list of the UI element methods: LUI's start-up registers every entry in it.
typedef struct aae_vmwidget_method {
    const char *name;
    lua_CFunction function;
    struct aae_vmwidget_method *next;
} aae_vmwidget_method_t;
// Puts first and then second right behind the entry of setupGeneric3DWidget (when that entry is the game's: the name
// and the function given), or takes them out of the list. Returns whether they are in it afterwards.
bool aae_vmwidget_splice(aae_vmwidget_method_t *generic, lua_CFunction generic_function, aae_vmwidget_method_t *first,
                         aae_vmwidget_method_t *second, bool wanted);
