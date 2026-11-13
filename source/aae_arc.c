// All-around Enhancement 4's line that shows where a grenade will fly (its option "Grenade Projection").
//
// The game can predict and draw a throw's arc: a job works out up to five segments of up to 60 points each, from the
// held weapon's speeds and fuse, and the client draws them as a line with a marker where it ends. Its own test for
// doing that at all (CG_ShouldPredictArc) says yes for an ordinary grenade only while the campaign's Tactical Mode is
// on, so in zombies never. The mod's Windows side has two pieces around it, both here, while the mod with its plugin
// is loaded and the setting AAEGrenadeArc is on:
//
// T7Overcharged's "beam" component (the mod's own build of T7Overcharged, T7O 0x1800646f0) replaces that test. Its
// rule: the bool dvar force_arc on is yes for anything; where the game asks about Tactical Mode, the bool dvar
// arc_requires_tmode (registered with true) stands instead, off meaning no grenade arc. The rest is the game's own
// rule. The mod's script sets arc_requires_tmode to 1 while a grenade is held in first person with its option at a
// line ("Prediction Line And Fuse Timer" or "Prediction Line Only"), else 0, and force_arc for two weapons that
// aren't grenades (a bow and a ball). T7Overcharged also takes out the job's own Tactical Mode test, so the job
// always works the arc out the way it does for Tactical Mode (at most 3 seconds ahead, in steps of a 50th of that,
// at least 3 ms), and guards R_AddCodeMeshDrawSurf against a material that isn't there. On the Mac those dvars are
// the script's own (string dvars): their text is read the way the game sets a bool dvar from a text.
// Of the rest of the component the mod uses nothing: the console commands force_arc_impact and arc_trType, and two
// mid-function hooks on the projectile's speed that its hook command could switch on ("exp/projectile1/2"; the
// first is made switched off and the second does nothing without it).
//
// The plugin's arc_prediction_anchor component, on top of that:
// (a) the arc can be worked out with another weapon's definition than the held one's: the one the dvar
//     aae_arc_weapondef names (the script sets it for the bow and the ball), when it passes a row of checks;
// (b) with arc_prediction_anchor::tag, the line starts at a bone of the view model, the one aae_arc_tag names or
//     else tag_weapon_right (the right hand), with its first quarter bent onto the curve;
// (c) two guards: no sixth segment, and no end marker written behind a fifth one.
//
// The places in the middle of the game's functions are detours (detour.h), only from the pre-patched executable.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aae_arc.h"
#include "aae_plugin.h"
#include "detour.h"
#include "dobby.h"
#include "gsc_obj.h"
#include "offsets.h"
#include "prepatch.h"

// the sums below are the plugin's and T7Overcharged's, in their order: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

// ---- T7Overcharged's arc test

// the player state (cg_t + 0x11a8d0 on the Mac, 0x11a8b0 on Windows), as T7Overcharged's test reads it
#define Ps_Aim_Flags 0x12     // a byte: 0x40 while the player aims
#define Ps_Flags 0x20         // a byte: 0x40 means no arc
#define Ps_Weapon_State 0x5c  // int
#define Ps_Ammo_Weapon 0x298  // the weapon BG_WeaponAmmo is asked about
#define Ps_Sights 0x2fc       // float: how far the sights are up
#define Aim_Flag 0x40
#define No_Arc_Flag 0x40
#define Sights_Up 0.85000002  // a double, as T7Overcharged compares (the game's own test: a float above 0.85f)
// the weapon definition
#define Def_Type 0x6c                 // int: 1 for a grenade
#define Def_Never_Arc 0xf8c           // int
#define Def_Arc_Without_Aim 0xf84     // int
#define Def_Arc_With_Aim 0xf88        // int
#define Def_Asks_Ammo 0x10dd          // a byte: the weapon also needs ammo
#define Weapon_Type_Grenade 1
#define State_Grenade_First 0x26      // the weapon states of a grenade being held (0x25 is excluded)
#define State_Grenade_Last 0x2e

static float as_float(const uint8_t *place) {
    float value;
    memcpy(&value, place, sizeof(value));
    return value;
}
static int32_t as_int(const uint8_t *place) {
    int32_t value;
    memcpy(&value, place, sizeof(value));
    return value;
}

bool aae_arc_predicts(int force_arc, int requires_tmode, const uint8_t *player_state, uint64_t weapon,
                      const uint8_t *(*weapon_definition)(uint64_t weapon),
                      int (*weapon_ammo)(const uint8_t *player_state, uint64_t weapon)) {
    if (force_arc != Aae_Arc_No_Dvar && force_arc != 0)
        return true;
    if (player_state == NULL || (player_state[Ps_Flags] & No_Arc_Flag) != 0)
        return false;
    if (weapon == 0 || (weapon & 0x1ff) == 0)
        return false;
    const uint8_t *definition = weapon_definition(weapon);
    // the game's BG_GetWeaponDef always has one; T7Overcharged reads it without asking
    if (definition == NULL || as_int(definition + Def_Never_Arc) != 0)
        return false;
    bool aiming = (player_state[Ps_Aim_Flags] & Aim_Flag) != 0 &&
                  (double)as_float(player_state + Ps_Sights) > Sights_Up;
    if (as_int(definition + Def_Arc_Without_Aim) != 0 && !aiming)
        return true;
    if (as_int(definition + Def_Arc_With_Aim) != 0 && aiming)
        return true;
    if (requires_tmode != Aae_Arc_No_Dvar && requires_tmode == 0)
        return false;
    if (as_int(definition + Def_Type) != Weapon_Type_Grenade)
        return false;
    int32_t state = as_int(player_state + Ps_Weapon_State);
    uint64_t ammo_weapon;
    memcpy(&ammo_weapon, player_state + Ps_Ammo_Weapon, sizeof(ammo_weapon));
    if (definition[Def_Asks_Ammo] != 0 && weapon_ammo(player_state, ammo_weapon) == 0)
        return false;
    return (uint32_t)(state - (State_Grenade_First - 1)) <= State_Grenade_Last - (State_Grenade_First - 1) &&
           state != State_Grenade_First - 1;
}

int aae_arc_dvar_switch(const char *text) {
    return atoi(text) != 0;
}

// ---- the plugin's part (a): the definition the arc is worked out with

#define Name_Size 0x80
#define Def_Fuse 0xc28               // int, ms
#define Def_Speed 0xfec              // int: the throw's speed, then up, relative up and forward (4 ints)
#define Def_Lifetime 0x1004          // float
#define Def_Impact_Explode 0x10c9    // a byte
#define Def_Timed_Detonation 0x10de  // a byte
#define Speed_Most 100000u
#define Fuse_Most 600000u
#define Lifetime_Most 100000.0f

static bool is_number(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000) != 0x7f800000;
}

// a character of a weapon's name, as the plugin lets it through
static bool weapon_name_character(unsigned char c) {
    return (unsigned char)(c - 'a') < 26 || (unsigned char)(c - 'A') < 26 || (unsigned char)(c - '0') <= 9 ||
           c == '_' || (unsigned char)(c - '-') <= 2;
}

bool aae_arc_weapon_name(const char *text, char name[Name_Size]) {
    if (text == NULL || strnlen(text, Name_Size) >= Name_Size)
        return false;
    for (size_t i = 0;; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\0') {
            name[i] = '\0';
            return i != 0;
        }
        if (!weapon_name_character(c))
            return false;
        name[i] = (char)c;
    }
}

const char *aae_arc_definition_problem(const uint8_t *original, const uint8_t *replacement) {
    if (as_int(replacement + Def_Type) != as_int(original + Def_Type))
        return "WEAPON_TYPE_MISMATCH";
    uint32_t speed[4], fuse;
    memcpy(speed, replacement + Def_Speed, sizeof(speed));
    memcpy(&fuse, replacement + Def_Fuse, sizeof(fuse));
    float lifetime = as_float(replacement + Def_Lifetime);
    uint8_t timed = replacement[Def_Timed_Detonation], impact = replacement[Def_Impact_Explode];
    if (speed[0] > Speed_Most)
        return "SPEED_INVALID";
    if (speed[1] + Speed_Most > 2 * Speed_Most)
        return "SPEED_UP_INVALID";
    if (speed[2] + Speed_Most > 2 * Speed_Most)
        return "SPEED_RELATIVE_UP_INVALID";
    if (speed[3] + Speed_Most > 2 * Speed_Most)
        return "SPEED_FORWARD_INVALID";
    if ((speed[0] | speed[1] | speed[2] | speed[3]) == 0)
        return "ALL_SPEEDS_ZERO";
    if (fuse > Fuse_Most)
        return "FUSE_INVALID";
    if (!is_number(lifetime) || 0.0f > lifetime || lifetime > Lifetime_Most)
        return "LIFETIME_INVALID";
    if (timed > 1)
        return "TIMED_DETONATION_INVALID";
    if (impact > 1)
        return "IMPACT_EXPLODE_INVALID";
    if (timed != 0 && fuse == 0)
        return "FUSE_MISSING_FOR_TIMED_DETONATION";
    return NULL;
}

// The plugin keeps the last name's lookup for a second and its verdicts for 12 definitions, re-checked against the
// game's table each time: the same answers as asking again, which is what is done here.
const uint8_t *aae_arc_prediction_definition(const uint8_t *original, const char *dvar_text,
                                            const uint8_t *(*definition_named)(const char *name)) {
    char name[Name_Size];
    if (original == NULL || !aae_arc_weapon_name(dvar_text, name))
        return original;
    const uint8_t *replacement = definition_named(name);
    if (replacement == NULL || replacement == original)
        return original;
    return aae_arc_definition_problem(original, replacement) == NULL ? replacement : original;
}

// ---- the plugin's part (b): the bend and the bone

#define Coordinate_Most 10000000.0f
#define Points_Most 60
#define Bend_Least_Length 0.001f
#define Bend_Leave 0.27f           // where the curve's first control point is, along the way to point k
#define Bend_Least_Along 0.56f     // and how far along it the second one is, at least
#define Bend_Most_Along 0.9f       // and at most
#define Bend_Most_Aside 0.2f       // how far aside of that way it may be, for the way's length

bool aae_arc_usable(const float point[3]) {
    return is_number(point[0]) && is_number(point[1]) && is_number(point[2]) && fabsf(point[0]) < Coordinate_Most &&
           fabsf(point[1]) < Coordinate_Most && fabsf(point[2]) < Coordinate_Most;
}

static float square_root(float value) {
    return sqrtf(value);
}

int aae_arc_bend(float (*points)[3], const float anchor[3], int n) {
    if (n < 8)
        return 0;
    int k = (n + 3) / 4;
    if (k < 4)
        k = 4;
    if (k > 16)
        k = 16;
    if (k + 1 >= n)
        return 0;
    float x0 = anchor[0], y0 = anchor[1], z0 = anchor[2];
    float pkx = points[k][0], pky = points[k][1], pkz = points[k][2];
    float dx = pkx - x0, dy = pky - y0, dz = pkz - z0;
    float length = square_root(dy * dy + dx * dx + dz * dz);
    if (!is_number(length) || !(length > Bend_Least_Length))
        return 0;
    float length_squared = length * length;
    if (!is_number(length_squared))
        return 0;
    // the arc's way at point k, and the second control point a third of k steps back along it
    const float way[3] = { points[k + 1][0] - pkx, points[k + 1][1] - pky, points[k + 1][2] - pkz };
    if (!aae_arc_usable(way))
        return 0;
    float steps = (float)k;
    float third = steps / 3.0f;
    const float back[3] = { pkx - way[0] * third, pky - way[1] * third, pkz - way[2] * third };
    if (!aae_arc_usable(back))
        return 0;
    float rx = back[0] - x0, ry = back[1] - y0, rz = back[2] - z0;
    float along = (ry * dy + rx * dx + rz * dz) / length_squared;
    if (!is_number(along))
        return 0;
    float at_least = along > Bend_Least_Along ? along : Bend_Least_Along;
    float px = rx - dx * along, py = ry - dy * along, pz = rz - dz * along;
    float aside = square_root(py * py + px * px + pz * pz);
    if (!is_number(aside))
        return 0;
    float at_most = at_least < Bend_Most_Along ? at_least : Bend_Most_Along;
    float limit = length * Bend_Most_Aside;
    float scale = aside > limit ? limit / aside : 1.0f;
    float c1x = dx * Bend_Leave + x0, c1y = dy * Bend_Leave + y0, c1z = dz * Bend_Leave + z0;
    const float c2[3] = { at_most * dx + x0 + px * scale, at_most * dy + y0 + py * scale,
                          at_most * dz + z0 + pz * scale };
    if (!aae_arc_usable(c2))
        return 0;
    float curve[Points_Most + 1][3];
    memset(curve, 0, sizeof(curve));
    for (int i = 1; i < k; i++) {
        float u = (float)i / steps;
        float v = 1.0f - u;
        float last = u * u * u;
        float v3 = v * 3.0f;
        float first = v * v * v;
        float second = v3 * v * u;
        float third_weight = v3 * u * u;
        float x = first * x0 + second * c1x + third_weight * c2[0] + last * pkx;
        if (!is_number(x))
            return 0;
        float y = first * y0 + second * c1y + third_weight * c2[1] + last * pky;
        if (!is_number(y))
            return 0;
        float z = first * z0 + second * c1z + third_weight * c2[2] + last * pkz;
        if (!is_number(z))
            return 0;
        if (!(fabsf(x) < Coordinate_Most) || !(fabsf(y) < Coordinate_Most) || !(fabsf(z) < Coordinate_Most))
            return 0;
        curve[i][0] = x, curve[i][1] = y, curve[i][2] = z;
    }
    memcpy(points + 1, curve + 1, (size_t)(k - 1) * sizeof(curve[0]));
    return k;
}

bool aae_arc_anchored_points(const float anchor[3], const float (*points)[3], int n, const float end[3],
                             float (*out)[3]) {
    if (!aae_arc_usable(anchor) || !aae_arc_usable(end) || points == NULL || (unsigned)(n - 1) >= Points_Most)
        return false;
    for (int i = 0; i <= n; i++) {
        const float *point = i == n ? end : points[i];
        if (!aae_arc_usable(point))
            return false;
        memcpy(out[i], point, sizeof(out[i]));
    }
    aae_arc_bend(out, anchor, n);
    memcpy(out[0], anchor, sizeof(out[0]));
    memcpy(out[n], end, sizeof(out[n]));
    return true;
}

#define Tag_Size 0x40
#define Fnv_Basis 0xcbf29ce484222325ull
#define Fnv_Prime 0x100000001b3ull

// a character of a bone's name, as the plugin lets it through
static bool tag_character(unsigned char c) {
    return (unsigned char)(c - 'a') < 26 || (unsigned char)(c - '0') <= 9 || (unsigned char)(c - 'A') < 26 || c == '_';
}

uint64_t aae_arc_tag(const char *text, char tag[Tag_Size]) {
    tag[0] = '\0';
    size_t length = text != NULL ? strnlen(text, Tag_Size) : Tag_Size;
    bool good = length < Tag_Size;
    for (size_t i = 0; good && i < length; i++)
        good = tag_character((unsigned char)text[i]);
    if (good)
        memcpy(tag, text, length + 1);
    uint64_t key = Fnv_Basis;
    for (size_t i = 0; tag[i] != '\0'; i++)
        key = (key ^ (unsigned char)tag[i]) * Fnv_Prime;
    return key;
}

#define Object_Computed 0xc8     // the DObj: 12 dwords, a bit for each bone the skeleton has been worked out for
#define Skeleton_Bone_Size 0x20  // its skeleton: per bone a quaternion, then the translation
#define Skeleton_Translation 0x10
#define Index_Unknown 0xfffe     // what the plugin hands DObjGetBoneIndex as the index (0xffff makes it say no)

bool aae_arc_bone_position(const uint8_t *object, const void *tree, int16_t bone_count, const uint8_t *skeleton,
                           uint32_t name, aae_arc_bone_cache_t *cache,
                           int (*bone_index)(const uint8_t *object, uint32_t name, uint16_t *index),
                           float position[3]) {
    if (name == 0 || bone_index == NULL)
        return false;
    int16_t index;
    if (cache->object == object && cache->tree == tree && cache->name == name && cache->index < bone_count) {
        index = cache->index;
    } else {
        uint16_t found = Index_Unknown;
        int answer = bone_index(object, name, &found);
        index = answer == 0 || (int16_t)found < 0 || (int16_t)found >= bone_count ? -1 : (int16_t)found;
        cache->object = object, cache->tree = tree, cache->name = name, cache->index = index;
    }
    if (index < 0 || index >= bone_count)
        return false;
    uint32_t computed;
    memcpy(&computed, object + Object_Computed + (size_t)(index >> 5) * 4, sizeof(computed));
    if ((computed & (0x80000000u >> (index & 31))) == 0)
        return false;
    float translation[3];
    memcpy(translation, skeleton + (size_t)index * Skeleton_Bone_Size + Skeleton_Translation, sizeof(translation));
    if (!aae_arc_usable(translation))
        return false;
    memcpy(position, translation, sizeof(translation));
    return true;
}

// ---- the plugin's part (c): guards

#define Segments_Most 5
#define Local_Clients 4

bool aae_arc_may_add_segment(int local_client, int segments) {
    return (unsigned)local_client < Local_Clients && (unsigned)segments < Segments_Most;
}

bool aae_arc_may_draw_marker(uint8_t marker, int segments) {
    return marker == 0 || (unsigned)(segments - 1) < Segments_Most - 1;
}

// ---- the game's side

#define Cg_Size 0x342a80
#define Cg_Player_State 0x11a8d0
#define Arc_Block_Size 0xeec        // the arc of a local client: 5 segments of 60 points, their counts ...
#define Arc_Segments 0xe60          // int: how many segments there are
#define Arc_Marker 0xee9            // a byte: the end marker is to be drawn
#define View_Size 0x3a0             // the view weapon states: per local client, the view model's DObj at +0
#define Object_Tree 0x18
#define Object_Skeleton 0x58
#define Object_Bone_Count 0x120     // uint16_t
#define Bones_Most 0x180
#define Draw_Local_Client (-0xac)   // the arc's draw function, from its frame pointer: the local client (int)
#define Draw_Command (-0xa8)        // and the command it hands the game for each segment
#define Command_Start 0x04          // 3 floats
#define Command_End 0x10            // 3 floats
#define Command_Last 0x40           // int: the segment's points are 0 to this, the end comes after them
#define Command_Points 0x68         // float (*)[3]
#define Material_Techniques 0x278   // what R_AddCodeMeshDrawSurf reads of a material first
#define Dvar_Type 0x1c
#define Dvar_Type_String 8
#define Ring_Size 1024              // buffers for the points of anchored segments, as many as the plugin has
#define String_User 0               // what the plugin hands SL_GetStringOfSize
#define String_Type 3
#define Any_Model 0xffff            // DObjGetBoneIndex's last argument: any of the object's models

static uint64_t base_address;
static int debug;
static void *(*Dvar_FindByHash)(uint32_t hash);
static const char *(*Dvar_GetString)(void *dvar);
static int (*Dvar_GetInt)(void *dvar);
static const uint8_t *(*BG_GetWeaponDef)(uint64_t weapon);
static int (*BG_WeaponAmmo)(const uint8_t *player_state, uint64_t weapon);
static uint32_t (*BG_FindWeaponIndexByName)(const char *name, void *hashes, uint8_t *sorted);
static uint32_t (*SL_GetStringOfSize)(const char *text, uint32_t user, uint32_t length, int type);
static int (*DObjGetBoneIndex)(const uint8_t *object, uint32_t name, uint16_t *index, int model);
static const void *skeleton_sites[2]; // where the view model's two DObjCalcSkel calls return to

static bool active(void) {
    return aae_plugin_loaded_lately();
}

// a dvar's text when it has text, NULL for none and for any other kind
static const char *dvar_text(const char *name) {
    const uint8_t *dvar = Dvar_FindByHash(gsc_hash(name));
    return dvar != NULL && as_int(dvar + Dvar_Type) == Dvar_Type_String ? Dvar_GetString((void *)dvar) : NULL;
}

// One of T7Overcharged's two bool dvars. On the Mac the mod's script makes them with setdvar, as dvars with text; a
// dvar of another kind counts by the game's own reading of it as a number.
static int switch_dvar(const char *name) {
    void *dvar = Dvar_FindByHash(gsc_hash(name));
    if (dvar == NULL)
        return Aae_Arc_No_Dvar;
    if (as_int((const uint8_t *)dvar + Dvar_Type) == Dvar_Type_String)
        return aae_arc_dvar_switch(Dvar_GetString(dvar));
    return Dvar_GetInt(dvar) != 0;
}

static uint8_t *arc_block(int local_client) {
    return (uint8_t *)(base_address + ADDR_CG_ArcBlocks) + (size_t)local_client * Arc_Block_Size;
}

// the game's cg_t of a local client, NULL as the game's own test has it
static const uint8_t *local_client_cg(int local_client) {
    const uint8_t *first = *(const uint8_t *const *)(base_address + ADDR_cgArray);
    int count = *(const int *)(base_address + ADDR_LOCAL_CLIENT_COUNT);
    if (first == NULL || local_client < 0 || local_client >= count)
        return NULL;
    return first + (size_t)local_client * Cg_Size;
}

// ---- T7Overcharged: the test and the job

// the game's test, (int local client, Weapon): whether the job predicts an arc this frame
install_hook_name(CG_ShouldPredictArc, bool, int local_client, uint64_t weapon) {
    if (!active())
        return orig_CG_ShouldPredictArc(local_client, weapon);
    const uint8_t *cg = local_client_cg(local_client);
    int force = switch_dvar("force_arc"), requires_tmode = switch_dvar("arc_requires_tmode");
    bool predicts = aae_arc_predicts(force, requires_tmode, cg != NULL ? cg + Cg_Player_State : NULL, weapon,
                                     BG_GetWeaponDef, BG_WeaponAmmo);
    if (debug) {
        // what the mod's script has set, when it changes (-1: no such dvar yet)
        static _Atomic int said_switches = -1;
        int switches = (force + 1) * 4 + requires_tmode + 1;
        if (atomic_exchange(&said_switches, switches) != switches)
            printf("arc: T7Overcharged's arc test answers for the game: force_arc %d, arc_requires_tmode %d\n", force,
                   requires_tmode);
    }
    if (debug && predicts) {
        static atomic_bool asked;
        if (!atomic_exchange(&asked, true)) {
            const uint8_t *const *materials = (const uint8_t *const *)(base_address + ADDR_CG_ArcMaterials);
            printf("arc: the arc's materials:");
            for (int i = 0; i < 4; i++)
                printf(" %s", materials[i] == NULL ? "none"
                                  : *(void *const *)(materials[i] + Material_Techniques) == NULL ? "without techniques"
                                                                                                : "there");
            printf("\n");
        }
    }
    if (debug && (unsigned)local_client < Local_Clients) {
        static _Atomic int said[Local_Clients] = { -1, -1, -1, -1 };
        if (atomic_exchange(&said[local_client], predicts) != (int)predicts)
            printf("arc: local client %d %s (weapon %#llx)\n", local_client,
                   predicts ? "gets an arc" : "no longer gets an arc", (unsigned long long)weapon);
    }
    return predicts;
}

// "movss xmm0, [rbp - 0x2c]": the job's branch for Tactical Mode being off. T7Overcharged takes the test out, so the
// job always goes on in the branch for it being on. Every register a call may change is unused here (the test was
// just called), as detour_choice needs.
detour_choice(Arc_No_Tactical_Mode, "mov %rbp, %rdi\n mov %rbp, %rsi\n") {
    (void)first, (void)second;
    return active();
}

// R_AddCodeMeshDrawSurf (material, ... 9 arguments): T7Overcharged's guard, for a material that isn't there
install_hook_name(R_AddCodeMeshDrawSurf, void, const uint8_t *material, uint64_t second, uint64_t third,
                  uint64_t fourth, uint64_t fifth, uint64_t sixth, uint64_t seventh, uint64_t eighth, uint64_t ninth) {
    if ((material == NULL || *(void *const *)(material + Material_Techniques) == NULL) && active()) {
        static atomic_bool said;
        if (debug && !atomic_exchange(&said, true))
            printf("arc: a code mesh without a material is not drawn (the first time)\n");
        return;
    }
    orig_R_AddCodeMeshDrawSurf(material, second, third, fourth, fifth, sixth, seventh, eighth, ninth);
}

// ---- the plugin (a): the weapon the arc is worked out with

static const uint8_t *definition_named(const char *name) {
    uint32_t index = BG_FindWeaponIndexByName(name, (void *)(base_address + ADDR_bg_weaponNameHashes),
                                              (uint8_t *)(base_address + ADDR_bg_weaponNameHashesSorted));
    if (index - 1 > 0x1fe)
        return NULL;
    const uint8_t *variant = ((const uint8_t *const *)(base_address + ADDR_bg_weaponVariantDefs))[index];
    return variant != NULL ? *(const uint8_t *const *)(variant + WeaponVariantDef_WeaponDef) : NULL;
}

// "mov [rbp - 0xa0], rax" right behind the job's BG_GetWeaponDef: the definition in rax, the weapon in rbx
detour_with_registers(Arc_Weapon_Definition) {
    if (!active())
        return;
    const uint8_t *original = (const uint8_t *)registers->rax;
    const char *text = dvar_text("aae_arc_weapondef");
    const uint8_t *used = aae_arc_prediction_definition(original, text, definition_named);
    registers->rax = (uint64_t)used;
    if (debug) {
        static _Atomic(const uint8_t *) said;
        if (atomic_exchange(&said, used) == used)
            return;
        char name[Name_Size];
        const uint8_t *named = aae_arc_weapon_name(text, name) ? definition_named(name) : NULL;
        const char *problem = named != NULL && named != original ? aae_arc_definition_problem(original, named) : NULL;
        printf("arc: worked out with %s (aae_arc_weapondef \"%.40s\"%s%s)\n",
               used != original ? "that weapon's definition" : "the weapon's own definition", text != NULL ? text : "",
               problem != NULL ? ": " : "", problem != NULL ? problem : "");
    }
}

// ---- the plugin (b): the view model's bone

// what is kept of each local client's view model, its bone and the anchor: as the plugin keeps it
typedef struct {
    _Atomic(const uint8_t *) object; // the view model's DObj as last noted, and its tree
    _Atomic(const void *) tree;
    aae_arc_bone_cache_t custom, right;
    pthread_mutex_t lock; // for the two caches and writing the anchor
    // the anchor: written under the lock, read without it, by the sequence (odd while it's written)
    _Atomic uint32_t sequence;
    atomic_bool valid;
    _Atomic uint64_t key;
    _Atomic uint32_t position[3];
} view_t;
static view_t views[Local_Clients] = {
    { .lock = PTHREAD_MUTEX_INITIALIZER, .custom.index = -2, .right.index = -2 },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .custom.index = -2, .right.index = -2 },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .custom.index = -2, .right.index = -2 },
    { .lock = PTHREAD_MUTEX_INITIALIZER, .custom.index = -2, .right.index = -2 },
};
// the names of the two bones (0 for none yet) and the key of the tag text they are for
static _Atomic uint32_t right_name, custom_name;
static _Atomic uint64_t tag_key = Fnv_Basis; // the plugin starts with the key of no text
static pthread_mutex_t tag_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool anchor_said[Local_Clients];

static void forget_anchor(view_t *view) {
    atomic_store(&view->valid, false);
}

// The plugin notes each local client's view model every frame where the game updates it, and forgets what it found
// out about a model when the model or its tree changes. Here the game's own pointer is read where it's needed.
static void note_view_models(void) {
    const uint8_t *states = *(const uint8_t *const *)(base_address + ADDR_cgViewWeaponStates);
    int count = *(const int *)(base_address + ADDR_LOCAL_CLIENT_COUNT);
    for (int which = 0; which < Local_Clients; which++) {
        const uint8_t *object = NULL;
        if (states != NULL && which < count)
            object = *(const uint8_t *const *)(states + (size_t)which * View_Size);
        const void *tree = object != NULL ? *(const void *const *)(object + Object_Tree) : NULL;
        view_t *view = &views[which];
        if (atomic_load(&view->object) == object && atomic_load(&view->tree) == tree)
            continue;
        atomic_store(&view->object, NULL);
        pthread_mutex_lock(&view->lock);
        view->custom = (aae_arc_bone_cache_t){ .index = -2 };
        view->right = (aae_arc_bone_cache_t){ .index = -2 };
        forget_anchor(view);
        pthread_mutex_unlock(&view->lock);
        atomic_store(&view->tree, tree);
        atomic_store(&view->object, object);
    }
}

static int bone_index(const uint8_t *object, uint32_t name, uint16_t *index) {
    return DObjGetBoneIndex(object, name, index, Any_Model);
}

static uint32_t string_named(const char *text) {
    return SL_GetStringOfSize(text, String_User, (uint32_t)strlen(text) + 1, String_Type);
}

// The plugin's work right after the game has worked out a view model's skeleton: where the bone is now.
static void find_anchor(const uint8_t *object) {
    note_view_models();
    bool mine[Local_Clients] = { false }, usable[Local_Clients] = { false }, any = false;
    for (int which = 0; which < Local_Clients; which++)
        any |= mine[which] = atomic_load(&views[which].object) == object;
    if (!any)
        return;
    const void *tree = *(const void *const *)(object + Object_Tree);
    uint16_t count;
    memcpy(&count, object + Object_Bone_Count, sizeof(count));
    const uint8_t *skeleton = *(const uint8_t *const *)(object + Object_Skeleton);
    any = false;
    for (int which = 0; which < Local_Clients; which++) {
        if (!mine[which])
            continue;
        if (tree == NULL || (uint16_t)(count - 1) > Bones_Most - 1 || skeleton == NULL ||
            atomic_load(&views[which].tree) != tree)
            forget_anchor(&views[which]);
        else
            any = usable[which] = true;
    }
    if (!any)
        return;
    uint32_t right = atomic_load(&right_name);
    if (right == 0) {
        right = string_named("tag_weapon_right");
        if (right != 0)
            atomic_store(&right_name, right);
    }
    char tag[Tag_Size];
    uint64_t key = aae_arc_tag(dvar_text("aae_arc_tag"), tag);
    if (key != atomic_load(&tag_key)) {
        pthread_mutex_lock(&tag_lock);
        if (key != atomic_load(&tag_key)) {
            atomic_store(&custom_name, tag[0] != '\0' ? string_named(tag) : 0);
            for (int which = 0; which < Local_Clients; which++)
                forget_anchor(&views[which]);
            atomic_store(&tag_key, key);
        }
        pthread_mutex_unlock(&tag_lock);
    }
    uint32_t custom = atomic_load(&custom_name);
    for (int which = 0; which < Local_Clients; which++) {
        if (!usable[which])
            continue;
        view_t *view = &views[which];
        if (right == 0 && custom == 0) {
            forget_anchor(view);
            continue;
        }
        pthread_mutex_lock(&view->lock);
        float position[3] = { 0.0f, 0.0f, 0.0f };
        bool found = false;
        if (custom != 0 && custom != right)
            found = aae_arc_bone_position(object, tree, (int16_t)count, skeleton, custom, &view->custom, bone_index,
                                          position);
        if (!found)
            found = aae_arc_bone_position(object, tree, (int16_t)count, skeleton, right, &view->right, bone_index,
                                          position);
        if (atomic_load(&view->object) == object && atomic_load(&view->tree) == tree) {
            if (found) {
                uint32_t sequence = atomic_load(&view->sequence);
                atomic_store(&view->sequence, sequence + 1);
                atomic_store(&view->valid, false);
                for (int i = 0; i < 3; i++) {
                    uint32_t bits;
                    memcpy(&bits, &position[i], sizeof(bits));
                    atomic_store(&view->position[i], bits);
                }
                atomic_store(&view->key, key);
                atomic_store(&view->valid, true);
                atomic_store(&view->sequence, sequence + 2);
                if (atomic_load(&view->object) != object || atomic_load(&view->tree) != tree)
                    forget_anchor(view);
            } else {
                forget_anchor(view);
            }
        }
        pthread_mutex_unlock(&view->lock);
        if (debug && atomic_exchange(&anchor_said[which], found) != found)
            printf("arc: local client %d's line %s (%s, at %.1f %.1f %.1f)\n", which,
                   found ? "starts at the view model's bone" : "has no bone to start at",
                   custom != 0 && custom != right ? tag : "tag_weapon_right", position[0], position[1], position[2]);
    }
}

// DObjCalcSkel (DObj, part bits, origin): the game works out a model's skeleton. The plugin does its work after the
// view model's two calls (from where the view model is drawn and updated); here those two are told by where they
// return to.
static uint64_t (*orig_DObjCalcSkel)(const uint8_t *object, uint64_t second, uint64_t third, uint64_t fourth,
                                     uint64_t fifth, uint64_t sixth);
static uint64_t calc_skeleton(const uint8_t *object, uint64_t second, uint64_t third, uint64_t fourth, uint64_t fifth,
                              uint64_t sixth, const void *from) {
    uint64_t result = orig_DObjCalcSkel(object, second, third, fourth, fifth, sixth);
    if ((from == skeleton_sites[0] || from == skeleton_sites[1]) && object != NULL && active())
        find_anchor(object);
    return result;
}
static uint64_t fake_DObjCalcSkel(const uint8_t *object, uint64_t second, uint64_t third, uint64_t fourth,
                                  uint64_t fifth, uint64_t sixth) {
    return calc_skeleton(object, second, third, fourth, fifth, sixth, __builtin_return_address(0));
}

// the anchor of a local client, as the plugin reads it: a whole one, for the key of the tag text now
static bool anchor_now(int which, uint64_t key, float anchor[3]) {
    view_t *view = &views[which];
    uint32_t sequence = atomic_load(&view->sequence);
    if ((sequence & 1) != 0 || !atomic_load(&view->valid) || atomic_load(&view->object) == NULL)
        return false;
    for (int i = 0; i < 3; i++) {
        uint32_t bits = atomic_load(&view->position[i]);
        memcpy(&anchor[i], &bits, sizeof(bits));
    }
    if (atomic_load(&view->sequence) != sequence || !atomic_load(&view->valid))
        return false;
    return atomic_load(&view->key) == key && atomic_load(&tag_key) == key;
}

static float ring[Ring_Size][Points_Most + 1][3];
static _Atomic uint32_t ring_next;
static _Atomic int drawn_said[Local_Clients] = { -1, -1, -1, -1 };

// BO3MACFIX_ARCDEBUG: whether a local client's line is drawn from the anchor, when that changes
static void say_drawn(int which, bool anchored, int last, const float anchor[3]) {
    if (!debug || atomic_exchange(&drawn_said[which], anchored) == (int)anchored)
        return;
    if (anchored)
        printf("arc: local client %d's line is drawn from the hand (%d points, from %.1f %.1f %.1f)\n", which, last + 1,
               anchor[0], anchor[1], anchor[2]);
    else
        printf("arc: local client %d's line is drawn from where the game starts it\n", which);
}

// Before the arc's draw function hands the game a segment of its first or its second loop over the segments. The
// plugin changes the first call of each loop, the first segment (whose points begin the local client's arc): it
// starts at the anchor. The draw function sets the start and the points of the command again for every segment, so
// they are changed in place (the plugin hands the game a changed copy).
static void draw_segment(detour_registers_t *registers) {
    if (!active())
        return;
    const uint8_t *frame = (const uint8_t *)registers->rbp;
    int which = as_int(frame + Draw_Local_Client);
    uint8_t *command = (uint8_t *)frame + Draw_Command;
    const float(*points)[3];
    memcpy(&points, command + Command_Points, sizeof(points));
    if ((unsigned)which >= Local_Clients || (const uint8_t *)points != arc_block(which))
        return;
    note_view_models();
    char tag[Tag_Size];
    float anchor[3], end[3];
    int last = as_int(command + Command_Last);
    if (!anchor_now(which, aae_arc_tag(dvar_text("aae_arc_tag"), tag), anchor)) {
        say_drawn(which, false, last, anchor);
        return;
    }
    memcpy(end, command + Command_End, sizeof(end));
    float(*buffer)[3] = ring[atomic_fetch_add(&ring_next, 1) % Ring_Size];
    if (!aae_arc_anchored_points(anchor, points, last, end, buffer)) {
        say_drawn(which, false, last, anchor);
        return;
    }
    memcpy(command + Command_Start, anchor, sizeof(anchor));
    memcpy(command + Command_Points, &buffer, sizeof(buffer));
    say_drawn(which, true, last, anchor);
}

// "lea rdi, [rbp - 0xa8]" right before the draw function's call that hands the game a segment: in its first loop
// over the segments, and in its second
detour_with_registers(Arc_First_Line) {
    draw_segment(registers);
}
detour_with_registers(Arc_Second_Line) {
    draw_segment(registers);
}

// ---- the plugin (c): the guards

// "cmp byte [r15 + rsi + 0xee9], 0": the draw function asks whether the end marker is to be drawn (rsi the arcs, r15
// the local client's). It writes the marker behind the last segment, which is outside the arc behind a fifth one:
// the plugin goes on as if there were none, which here is the marker's byte set to 0 before the game's test
detour_with_registers(Arc_Marker) {
    if (!active())
        return;
    uint8_t *block = (uint8_t *)(registers->rsi + registers->r15);
    int segments = as_int(block + Arc_Segments);
    if (aae_arc_may_draw_marker(block[Arc_Marker], segments))
        return;
    block[Arc_Marker] = 0;
    static atomic_bool said;
    if (debug && !atomic_exchange(&said, true))
        printf("arc: no end marker behind %d segments (the first time)\n", segments);
}

// the job adds a segment to the arc: (int local client, the trace ...)
install_hook_name(CG_ArcPrediction_AddSegment, void, int local_client, uint64_t second, uint64_t third,
                  uint64_t fourth, uint64_t fifth, uint64_t sixth) {
    if (active()) {
        int segments = (unsigned)local_client < Local_Clients ? as_int(arc_block(local_client) + Arc_Segments) : 0;
        if (!aae_arc_may_add_segment(local_client, segments)) {
            static atomic_bool said;
            if (debug && !atomic_exchange(&said, true))
                printf("arc: no segment %d for local client %d (the first time)\n", segments + 1, local_client);
            return;
        }
    }
    orig_CG_ArcPrediction_AddSegment(local_client, second, third, fourth, fifth, sixth);
}

void aae_arc_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    const char *env = getenv("BO3MACFIX_ARCDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    aae_plugin_init(game_base_address);
    Dvar_FindByHash = (void *)(base_address + ADDR_Dvar_FindByHash);
    Dvar_GetString = (void *)(base_address + ADDR_Dvar_GetString);
    Dvar_GetInt = (void *)(base_address + ADDR_Dvar_GetInt);
    BG_GetWeaponDef = (void *)(base_address + ADDR_BG_GetWeaponDef);
    BG_WeaponAmmo = (void *)(base_address + ADDR_BG_WeaponAmmo);
    BG_FindWeaponIndexByName = (void *)(base_address + ADDR_BG_FindWeaponIndexByName);
    SL_GetStringOfSize = (void *)(base_address + ADDR_SL_GetStringOfSize);
    DObjGetBoneIndex = (void *)(base_address + ADDR_DObjGetBoneIndex);
    skeleton_sites[0] = (const void *)(base_address + ADDR_DObjCalcSkel_ViewModelReturn);
    skeleton_sites[1] = (const void *)(base_address + ADDR_DObjCalcSkel_ViewModelReturn2);
    const uint32_t places[] = { ADDR_CG_ArcPrediction_NoTacticalMode, ADDR_CG_ArcPrediction_WeaponDef,
                                ADDR_CG_DrawArc_FirstLine, ADDR_CG_DrawArc_SecondLine, ADDR_CG_DrawArc_MarkerTest };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        if (!prepatch_has_hook(places[i])) {
            printf("arc: not running from the pre-patched executable: no grenade line\n");
            return;
        }
    }
    install_hook_CG_ShouldPredictArc((void *)(base_address + ADDR_CG_ShouldPredictArc));
    install_detour_Arc_No_Tactical_Mode((void *)(base_address + ADDR_CG_ArcPrediction_NoTacticalMode),
                                        (void *)(base_address + ADDR_CG_ArcPrediction_TacticalMode));
    install_hook_R_AddCodeMeshDrawSurf((void *)(base_address + ADDR_R_AddCodeMeshDrawSurf));
    install_detour_Arc_Weapon_Definition((void *)(base_address + ADDR_CG_ArcPrediction_WeaponDef));
    DobbyHook((void *)(base_address + ADDR_DObjCalcSkel), (void *)fake_DObjCalcSkel, (void **)&orig_DObjCalcSkel);
    install_detour_Arc_First_Line((void *)(base_address + ADDR_CG_DrawArc_FirstLine));
    install_detour_Arc_Second_Line((void *)(base_address + ADDR_CG_DrawArc_SecondLine));
    install_detour_Arc_Marker((void *)(base_address + ADDR_CG_DrawArc_MarkerTest));
    install_hook_CG_ArcPrediction_AddSegment((void *)(base_address + ADDR_CG_ArcPrediction_AddSegment));
}
