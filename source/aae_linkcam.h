// All-around Enhancement 4's left-hand gestures follow the view, as its Windows plugin makes them (AAEGestures). See
// aae_linkcam.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void aae_linkcam_init(uint64_t game_base_address, int enabled);

#define Aae_Linkcam_Max 4 // the most entities the game lets client scripts link to a local client's camera

// The game's list of the entities linked to a local client's camera, as its cg_t has it: how many there are, their
// numbers, how each one follows the view (its mode) and where it sits seen from the camera (its offset).
typedef struct {
    int32_t count;
    int32_t entnum[Aae_Linkcam_Max];
    uint32_t mode[Aae_Linkcam_Max];
    float offset[Aae_Linkcam_Max][3];
} aae_linkcam_list_t;

// Where this entity is in the list (its first entry), -1 when it isn't, or when the list doesn't hold 1 to 4.
int aae_linkcam_find(const aae_linkcam_list_t *list, int32_t entnum);
// Takes every entry of this entity out of a list of 1 to 4, the others keeping their order, and clears the entries
// that become free at the end. How many were taken out.
int aae_linkcam_remove(aae_linkcam_list_t *list, int32_t entnum);

// The plugin's copy of a list's entity numbers, by which it tells a linked entity from the others when one is drawn.
typedef struct {
    int32_t count;
    int32_t entnum[Aae_Linkcam_Max];
} aae_linkcam_ids_t;
// takes the copy anew: a list of 1 to 4 is copied, anything else (or no list) leaves none
void aae_linkcam_note(aae_linkcam_ids_t *ids, const aae_linkcam_list_t *list);
bool aae_linkcam_noted(const aae_linkcam_ids_t *ids, int32_t entnum);

// The angles a linked entity gets, by its mode: its own with the view's pitch (0), yaw (1) or roll (2); its own (5);
// the view's (3, 4 and every other). True when its axis is to be made from those angles, false when it is the view's.
bool aae_linkcam_angles(uint32_t mode, const float own[3], const float view[3], float angles[3]);
// Where it goes: the view's origin and its offset along the three directions of the axis, in the plugin's order.
void aae_linkcam_origin(const float offset[3], const float view_origin[3], const float axis[9], float origin[3]);
// Whether it is moved there: the six numbers are all numbers (none infinite or NaN) and one of them differs, bit for
// bit, from where it is now.
bool aae_linkcam_moves(const float origin[3], const float angles[3], const float now_origin[3],
                       const float now_angles[3]);

// How the game draws an entity (the last argument of its function for that): a word of flags, and with flag 8 a
// pointer at +0x18 to more.
#define Aae_Linkcam_Lighting_Size 32
#define Aae_Linkcam_Extra_Size 48

// A linked entity held back from being drawn until the view of the frame is worked out: everything the game's
// drawing function was given, with copies of what its last argument points to.
typedef struct {
    void *object;
    const uint8_t *cg;
    uint8_t *cent;
    int32_t entnum;
    uint32_t flags;
    _Alignas(16) uint8_t lighting[Aae_Linkcam_Lighting_Size];
    _Alignas(16) uint8_t extra[Aae_Linkcam_Extra_Size];
    bool had_lighting, had_extra;
    int32_t time; // the cg_t's time when it was held back
} aae_linkcam_slot_t;
typedef struct {
    uint32_t count;
    aae_linkcam_slot_t slot[Aae_Linkcam_Max];
} aae_linkcam_held_t;
// Holds one back: in the slot that has this entity already, or else in the next one. When all four are taken it
// isn't kept (and isn't drawn in that frame).
void aae_linkcam_hold(aae_linkcam_held_t *held, void *object, const uint8_t *cg, uint8_t *cent, int32_t entnum,
                      uint32_t flags, const uint8_t *lighting, int32_t time);
