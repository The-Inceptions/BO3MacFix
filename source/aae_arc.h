// All-around Enhancement 4's line that shows where a grenade will fly (AAEGrenadeArc). See aae_arc.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void aae_arc_init(uint64_t game_base_address, int enabled);

// ---- T7Overcharged's part: whether the game predicts an arc at all

// T7Overcharged's bool dvars as the mod's script leaves them: 1 or 0, or -1 while there is none
#define Aae_Arc_No_Dvar (-1)
// What T7Overcharged's arc test answers for a local client's player state and the weapon the game asks about.
// force_arc and requires_tmode are its two dvars (force_arc: yes for anything; requires_tmode stands where the game
// asks whether the campaign's Tactical Mode is on: with it 0 no grenade gets an arc). weapon_definition is the
// game's BG_GetWeaponDef, weapon_ammo its BG_WeaponAmmo; each is only asked when T7Overcharged's test would.
bool aae_arc_predicts(int force_arc, int requires_tmode, const uint8_t *player_state, uint64_t weapon,
                      const uint8_t *(*weapon_definition)(uint64_t weapon),
                      int (*weapon_ammo)(const uint8_t *player_state, uint64_t weapon));
// A dvar's text as T7Overcharged's bool dvar would hold it: the game's own rule, a number other than 0.
int aae_arc_dvar_switch(const char *text);

// ---- the plugin's part (a): which weapon the arc is worked out for

// aae_arc_weapondef's text as the plugin takes it: fewer than 128 characters of letters, digits, _ - . and /, not
// empty. False for anything else (the weapon in hand is used then).
bool aae_arc_weapon_name(const char *text, char name[128]);
// What the plugin finds wrong with predicting the original weapon's arc with another definition: a reason in its own
// words, or NULL when there is nothing.
const char *aae_arc_definition_problem(const uint8_t *original, const uint8_t *replacement);
// The definition the arc is worked out with: the one aae_arc_weapondef names (definition_named gives it, NULL for
// none) when it passes the checks above, else the original.
const uint8_t *aae_arc_prediction_definition(const uint8_t *original, const char *dvar_text,
                                            const uint8_t *(*definition_named)(const char *name));

// ---- the plugin's part (b): the line starts at the hand

// a point the plugin works with: every coordinate a number and closer to 0 than 10,000,000
bool aae_arc_usable(const float point[3]);
// The plugin's bend of the arc's first points onto its curve, from the start (anchor) given: for n > 7 the first
// k - 1 points behind the start (k = (n + 3) / 4, from 4 to 16) become a cubic curve that leaves the start towards
// the arc and joins it smoothly at point k. Returns k, or 0 when nothing was changed.
int aae_arc_bend(float (*points)[3], const float anchor[3], int n);
// The points the first segment of a loop is drawn with when its start is to be the anchor: the segment's n points
// (from 1 to 60) with its end as point n, each usable, bent as above, the anchor as point 0. False to draw the
// segment as the game has it.
bool aae_arc_anchored_points(const float anchor[3], const float (*points)[3], int n, const float end[3],
                             float (*out)[3]);
// aae_arc_tag's text as the plugin takes it: fewer than 64 characters of letters, digits and _ (anything else counts
// as no text), and its key, the 64-bit FNV-1a of that text.
uint64_t aae_arc_tag(const char *text, char tag[64]);

// what the plugin keeps to find a bone of a model again: the model, its tree, the bone's name and the bone's index
typedef struct {
    const void *object, *tree;
    uint32_t name;
    int16_t index;
} aae_arc_bone_cache_t;
// The world position of a bone of the view model: the index from the cache (or the game's DObjGetBoneIndex, its
// result then kept), within the model's bone count, among the bones the game has worked out this frame (the bits at
// object + 0xc8), its translation in the skeleton usable.
bool aae_arc_bone_position(const uint8_t *object, const void *tree, int16_t bone_count, const uint8_t *skeleton,
                           uint32_t name, aae_arc_bone_cache_t *cache,
                           int (*bone_index)(const uint8_t *object, uint32_t name, uint16_t *index),
                           float position[3]);

// ---- the plugin's part (c): guards

// whether the game may add another segment to a local client's arc (5 at most, local clients 0 to 3)
bool aae_arc_may_add_segment(int local_client, int segments);
// whether the game may draw the arc's end marker: it writes it behind the last segment, so not when that is the fifth
bool aae_arc_may_draw_marker(uint8_t marker, int segments);
