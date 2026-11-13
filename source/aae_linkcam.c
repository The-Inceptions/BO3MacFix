// All-around Enhancement 4's left-hand gestures: the plugin's link_to_camera component (AAEGestures).
//
// The mod plays a gesture with the left hand when the player picks something up, opens a door, turns the power on,
// revives somebody, drinks a perk, pings or prints (its client script aae_left_ges.csc). The hand is an entity the
// script spawns, links to the camera with the game's script method linktocamera(mode), animates and deletes again.
// The game's half of that method only notes the entity in a list of four in the local client's cg_t (with its mode
// and an offset), and the game's update of client script entities leaves every entity in that list alone. Nothing
// else in the game reads the list: nothing puts those entities at the camera, nothing takes them off the list (not
// even delete), and a link while four are noted is a script error ("Max camera linked entities reached"), which ends
// the gesture script. Without the plugin the hand isn't shown where it belongs, and as one gesture is played while
// the map starts, the fourth gesture of a game is the last one.
//
// The plugin finishes it, and so does this, by the plugin's rules:
// - delete and forcedelete take the entity off the list first: its other entries move up, in their order, and the
//   ones freed at the end are cleared;
// - each frame the game draws the client's entities before it works out the view of the frame. A linked entity isn't
//   drawn then but held back (up to four, with copies of what the drawing function was given), and right after the
//   view's axis is made (an AnglesToAxis call in CG_CalcViewValues) each one held back in the same frame is put at
//   the view and drawn: at the view's origin plus its offset along the axis, with angles by its mode (its own with the
//   view's pitch (0), yaw (1) or roll (2) and an axis made from them; its own (5) or the view's (3, 4, any other)
//   with the view's axis). Where that changes its place or angles, the game is told its pose changed.
// - Which entities are linked the plugin takes from a copy of the list's numbers, made anew after each link, delete
//   and view. It keeps one cg_t for all of it: the one the last link was made for, or the last view held-back
//   entities were drawn in (with two local clients that is the plugin's way too).
//
// Not here: the plugin also keeps the list across the game's own save and restore of a local client, which only
// AAE's zombie checkpoint on Windows uses (through the plugin's checkpoint_fix); BO3MacFix's checkpoint starts a new
// game instead, whose list begins empty. And it lets linktocamera be called without an argument (the table's
// minimum of one becomes none); the mod always gives one.
//
// Everything here runs on the client's thread (the client's frame, and its scripts' methods), as on Windows: the
// plugin keeps all of it without locks, and so does this. Only from the pre-patched executable, because of the
// detour behind AnglesToAxis.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "aae_linkcam.h"
#include "aae_plugin.h"
#include "detour.h"
#include "dobby.h"
#include "offsets.h"
#include "prepatch.h"

// the sums below are the plugin's, in its order: nothing is to be folded into one operation
#pragma STDC FP_CONTRACT OFF

#define Cg_Size 0x342a80
#define Cg_Camera_List 0x3272f4 // the list (aae_linkcam_list_t), 84 bytes (Windows +0x326f94)
#define Cg_Time 0x11a8ac        // int: the time the frame's client entities are worked out for (Windows +0x11a88c)
#define Cg_View_Origin 0x131dc8 // the view's origin, three floats (Windows +0x131d78)
#define Cg_View_Axis 0x131de4   // and its axis, three directions of three floats (Windows +0x131d94)
#define Cg_View_Angles 0x2d8930 // and its angles: pitch, yaw and roll (Windows +0x2d88e0)
#define Cg_Third_Person 0x11a8c9 // a byte: the view is a third person one (only for the debug lines)
#define Cent_Local_Client 0x2   // a client entity (centity_t): a byte, the local client it belongs to
#define Cent_Origin 0x40        // where it is
#define Cent_Angles 0x4c        // and its angles
#define Cent_Entnum 0x3f8       // its number
#define Max_Local_Client 3      // the plugin hands a client entity's local client on only up to this, otherwise 0
#define Lighting_Flags 0x0      // what the drawing function's last argument points to: a word of flags,
#define Lighting_Has_Extra 0x8  // with this one,
#define Lighting_Extra 0x18     // a pointer to more

_Static_assert(sizeof(aae_linkcam_list_t) == 84, "the list is 84 bytes in the cg_t");

// ---- the plugin's rules

static bool in_range(int32_t count) {
    return (uint32_t)(count - 1) < Aae_Linkcam_Max;
}

int aae_linkcam_find(const aae_linkcam_list_t *list, int32_t entnum) {
    if (!in_range(list->count))
        return -1;
    for (int i = 0; i < list->count; i++)
        if (list->entnum[i] == entnum)
            return i;
    return -1;
}

int aae_linkcam_remove(aae_linkcam_list_t *list, int32_t entnum) {
    int count = list->count, kept = 0, removed = 0;
    if (!in_range(count))
        return 0;
    for (int i = 0; i < count; i++) {
        if (list->entnum[i] == entnum) {
            removed++;
            continue;
        }
        if (kept != i) {
            list->entnum[kept] = list->entnum[i];
            list->mode[kept] = list->mode[i];
            memcpy(list->offset[kept], list->offset[i], sizeof(list->offset[i]));
        }
        kept++;
    }
    if (removed == 0)
        return 0;
    for (int i = kept; i < count; i++) {
        list->entnum[i] = 0;
        list->mode[i] = 0;
        memset(list->offset[i], 0, sizeof(list->offset[i]));
    }
    list->count = kept;
    return removed;
}

void aae_linkcam_note(aae_linkcam_ids_t *ids, const aae_linkcam_list_t *list) {
    ids->count = 0;
    if (list == NULL || !in_range(list->count))
        return;
    memcpy(ids->entnum, list->entnum, (size_t)list->count * sizeof(ids->entnum[0]));
    ids->count = list->count;
}

bool aae_linkcam_noted(const aae_linkcam_ids_t *ids, int32_t entnum) {
    for (int i = 0; i < ids->count && i < Aae_Linkcam_Max; i++)
        if (ids->entnum[i] == entnum)
            return true;
    return false;
}

bool aae_linkcam_angles(uint32_t mode, const float own[3], const float view[3], float angles[3]) {
    memcpy(angles, own, 3 * sizeof(float));
    if (mode <= 2) {
        angles[mode] = view[mode];
        return true;
    }
    if (mode != 5)
        memcpy(angles, view, 3 * sizeof(float));
    return false;
}

void aae_linkcam_origin(const float offset[3], const float view_origin[3], const float axis[9], float origin[3]) {
    for (int k = 0; k < 3; k++)
        origin[k] = offset[0] * axis[k] + view_origin[k] + offset[1] * axis[3 + k] + offset[2] * axis[6 + k];
}

static bool is_number(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000) != 0x7f800000;
}

bool aae_linkcam_moves(const float origin[3], const float angles[3], const float now_origin[3],
                       const float now_angles[3]) {
    for (int k = 0; k < 3; k++)
        if (!is_number(origin[k]) || !is_number(angles[k]))
            return false;
    return memcmp(origin, now_origin, 3 * sizeof(float)) != 0 || memcmp(angles, now_angles, 3 * sizeof(float)) != 0;
}

void aae_linkcam_hold(aae_linkcam_held_t *held, void *object, const uint8_t *cg, uint8_t *cent, int32_t entnum,
                      uint32_t flags, const uint8_t *lighting, int32_t time) {
    uint32_t at = held->count;
    for (uint32_t i = 0; i < held->count; i++) {
        if (held->slot[i].cent == cent) {
            at = i;
            break;
        }
    }
    if (at >= Aae_Linkcam_Max)
        return;
    const uint8_t *extra = NULL;
    if (lighting != NULL && (lighting[Lighting_Flags] & Lighting_Has_Extra) != 0)
        memcpy(&extra, lighting + Lighting_Extra, sizeof(extra));
    aae_linkcam_slot_t *slot = &held->slot[at];
    slot->object = object;
    slot->cg = cg;
    slot->cent = cent;
    slot->entnum = entnum;
    slot->flags = flags;
    slot->time = time;
    slot->had_lighting = lighting != NULL;
    slot->had_extra = extra != NULL;
    // a slot used again keeps the copies it had where there are none now, as the plugin's does: unused then
    if (lighting != NULL)
        memcpy(slot->lighting, lighting, sizeof(slot->lighting));
    if (extra != NULL)
        memcpy(slot->extra, extra, sizeof(slot->extra));
    if (at == held->count)
        held->count = at + 1;
}

// ---- the game's side

static int debug;
static uint8_t *const *cg_clients;
static const int32_t *local_client_count;
static void (*AnglesToAxis)(const float *angles, float *axis); // reads 16 bytes of angles
static void (*CG_EntityPoseChanged)(int local_client, int32_t entnum);

static struct {
    uint8_t *cg;              // the cg_t of the last link, or of the last view entities held back were drawn in
    aae_linkcam_ids_t noted;  // the plugin's copy of that list's numbers
    aae_linkcam_held_t held;  // the linked entities held back in this frame
    bool placing;             // and they are being drawn now
} state;

static aae_linkcam_list_t *list_of(uint8_t *cg) {
    return (aae_linkcam_list_t *)(cg + Cg_Camera_List);
}
// The cg_t kept from the last link or view, while it is still one of the game's. The game frees its cg_t array when
// a map is left and makes one anew for the next map or the menu, not always at the same place: what was kept then
// points at memory that is nobody's (the game hands its pages back) or somebody else's, and a delete from a script in
// the menu or at the next map's start would read a list there and perhaps write it. The plugin asks Windows whether
// the memory can be read before it does that; whether it still is the list, it can't know. Here the kept cg_t counts
// only as long as the game's array holds it, and when it doesn't, all that was kept is of a game that is over.
static uint8_t *kept_cg(void) {
    uintptr_t kept = (uintptr_t)state.cg, first = (uintptr_t)*cg_clients;
    uintptr_t clients = *local_client_count > 0 ? (uintptr_t)*local_client_count : 0;
    if (kept != 0 && (first == 0 || kept < first || kept - first >= clients * Cg_Size || (kept - first) % Cg_Size != 0)) {
        state.cg = NULL;
        state.noted.count = 0;
        state.held.count = 0;
    }
    return state.cg;
}
static int32_t entity_number(const uint8_t *cent) {
    int32_t number;
    memcpy(&number, cent + Cent_Entnum, sizeof(number));
    return number;
}
static int32_t time_of(const uint8_t *cg) {
    int32_t time;
    memcpy(&time, cg + Cg_Time, sizeof(time));
    return time;
}

// What the debug lines tell: each link, counted from the start, each delete that takes an entity off the list, and the
// first time an entity is put at the view after a link, with how many times that happened until the next link or
// delete. Each names its thread, and says so when it isn't the one the frames' views are worked out on.
static struct {
    unsigned links;
    bool placed;
    unsigned frames;
    unsigned frame_thread; // 0 until the first view
} told;
static unsigned thread_number(void) {
    return pthread_mach_thread_np(pthread_self());
}
static void say_thread(void) {
    unsigned thread = thread_number();
    if (told.frame_thread != 0 && thread != told.frame_thread)
        printf(" (thread %u, NOT the one the views are worked out on, %u)\n", thread, told.frame_thread);
    else
        printf(" (thread %u)\n", thread);
}
static void tell_placed(int32_t entnum, uint32_t mode, const float origin[3], const float angles[3], bool moves,
                        bool third_person) {
    told.frames++;
    if (told.placed)
        return;
    told.placed = true;
    printf("gestures: entity %d drawn at the view (mode %u): at %.1f %.1f %.1f, angles %.1f %.1f %.1f%s%s", entnum,
           mode, origin[0], origin[1], origin[2], angles[0], angles[1], angles[2],
           moves ? "" : ", where it was already", third_person ? ", a third person view" : "");
    say_thread();
}

// The game draws a client entity: (its model, the entity, its number, flags, how it is lit (or NULL)), which is
// hooked below. Nothing is given back.
static void (*orig_AddDObjToScene)(void *object, uint8_t *cent, int32_t entnum, uint32_t flags,
                                   const uint8_t *lighting);

// puts a held-back entity at the view, as its mode says, while it is drawn in the view's frame
static void place(uint8_t *cent, int32_t entnum) {
    uint8_t *cg = state.cg;
    if (cg == NULL)
        return;
    const aae_linkcam_list_t *list = list_of(cg);
    int at = aae_linkcam_find(list, entity_number(cent));
    if (at < 0)
        return;
    float own[3], view[3], view_origin[3], angles[4] = { 0 }, axis[9], origin[3], now_origin[3];
    memcpy(own, cent + Cent_Angles, sizeof(own));
    memcpy(view, cg + Cg_View_Angles, sizeof(view));
    memcpy(view_origin, cg + Cg_View_Origin, sizeof(view_origin));
    if (aae_linkcam_angles(list->mode[at], own, view, angles))
        AnglesToAxis(angles, axis);
    else
        memcpy(axis, cg + Cg_View_Axis, sizeof(axis));
    aae_linkcam_origin(list->offset[at], view_origin, axis, origin);
    memcpy(now_origin, cent + Cent_Origin, sizeof(now_origin));
    bool moves = aae_linkcam_moves(origin, angles, now_origin, own);
    if (debug)
        tell_placed(entnum, list->mode[at], origin, angles, moves, cg[Cg_Third_Person] != 0);
    if (!moves)
        return;
    memcpy(cent + Cent_Origin, origin, sizeof(origin));
    memcpy(cent + Cent_Angles, angles, 3 * sizeof(float));
    int local = cent[Cent_Local_Client] <= Max_Local_Client ? cent[Cent_Local_Client] : 0;
    CG_EntityPoseChanged(local, entnum);
}

// What becomes of an entity the game draws. Every one comes here, many times a frame: one that isn't linked (by the
// plugin's copy) is drawn at once, which is all most of them see of this.
static void draw(void *object, uint8_t *cent, int32_t entnum, uint32_t flags, const uint8_t *lighting) {
    if ((!state.placing && !aae_linkcam_noted(&state.noted, entnum)) || cent == NULL ||
        !aae_plugin_loaded_lately()) {
        orig_AddDObjToScene(object, cent, entnum, flags, lighting);
        return;
    }
    if (state.placing) {
        place(cent, entnum);
        orig_AddDObjToScene(object, cent, entnum, flags, lighting);
        return;
    }
    // held back until the view is worked out: also when all four places to keep it are taken, then it isn't drawn
    uint8_t *cg = kept_cg();
    if (cg == NULL || entity_number(cent) != entnum || aae_linkcam_find(list_of(cg), entnum) < 0) {
        orig_AddDObjToScene(object, cent, entnum, flags, lighting);
        return;
    }
    aae_linkcam_hold(&state.held, object, cg, cent, entnum, flags, lighting, time_of(cg));
}

install_hook_name(AddDObjToScene, void, void *object, uint8_t *cent, int32_t entnum, uint32_t flags,
                  const uint8_t *lighting) {
    draw(object, cent, entnum, flags, lighting);
}

// Right after the view's axis is made for a local client (this cg_t): the copy of the numbers is taken anew, and
// the entities held back in this frame are drawn, each at the view.
static void view_worked_out(uint8_t *cg) {
    if (!aae_plugin_loaded_lately()) {
        state.noted.count = 0;
        state.held.count = 0;
        return;
    }
    if (debug && told.frame_thread == 0)
        told.frame_thread = thread_number();
    aae_linkcam_note(&state.noted, cg != NULL ? list_of(cg) : NULL);
    if (state.held.count == 0 || cg == NULL || state.placing)
        return;
    state.cg = cg;
    int32_t time = time_of(cg);
    state.placing = true;
    for (uint32_t i = 0; i < state.held.count; i++) {
        aae_linkcam_slot_t *slot = &state.held.slot[i];
        if (slot->cg != cg || slot->time != time || slot->object == NULL || slot->cent == NULL ||
            entity_number(slot->cent) != slot->entnum) {
            if (debug) {
                static bool said;
                if (!said)
                    printf("gestures: entity %d was held back but isn't drawn: it was for another view or frame "
                           "(said once)\n", slot->entnum);
                said = true;
            }
            continue;
        }
        if (slot->had_extra) {
            const uint8_t *extra = slot->extra;
            memcpy(slot->lighting + Lighting_Extra, &extra, sizeof(extra));
        }
        draw(slot->object, slot->cent, slot->entnum, slot->flags, slot->had_lighting ? slot->lighting : NULL);
    }
    state.held.count = 0;
    state.placing = false;
}

// "cmp ebx, 0xb ; je" right behind AnglesToAxis(cg->viewAngles, cg->refdef.viewaxis) in CG_CalcViewValues: the cg_t
// is in r12.
detour_with_registers(View_Axis) {
    view_worked_out((uint8_t *)registers->r12);
}

// ---- the client scripts' methods: (script instance, the entity's number in the low half of the second, the local
// client in the high half of the third)

// the cg_t a method is for, found as the game's methods find it
static uint8_t *cg_of_method(uint64_t third) {
    uint8_t *first = *cg_clients;
    int32_t local = (int32_t)(third >> 32);
    if (first == NULL || !(*local_client_count > local))
        return NULL;
    return first + (int64_t)local * Cg_Size;
}

static void say_list(const char *what, int32_t entnum) {
    const aae_linkcam_list_t *list = list_of(state.cg);
    printf("gestures: entity %d %s (drawn at the view %u times since the last), %d linked now:", entnum, what,
           told.frames, list->count);
    for (int i = 0; i < list->count && i < Aae_Linkcam_Max; i++)
        printf(" %d (mode %u, offset %.1f %.1f %.1f)", list->entnum[i], list->mode[i], list->offset[i][0],
               list->offset[i][1], list->offset[i][2]);
    say_thread();
    told.placed = false;
    told.frames = 0;
}

// linktocamera([mode], [offset]): the game's own adds the entity to the list, or makes a script error when four
// are there already (which ends here too: nothing behind the call runs then).
install_hook_name(CScr_LinkToCamera, void, uint64_t instance, uint64_t entity, uint64_t third, uint64_t fourth,
                  uint64_t fifth, uint64_t sixth) {
    if (!aae_plugin_loaded_lately()) {
        orig_CScr_LinkToCamera(instance, entity, third, fourth, fifth, sixth);
        return;
    }
    // the plugin notes the cg_t where the game's method is about to write the list; here it is found the same way
    // before (a script error in the method's arguments, before that place, would leave the plugin's as it was)
    state.cg = cg_of_method(third);
    if (debug && state.cg != NULL && list_of(state.cg)->count >= Aae_Linkcam_Max)
        printf("gestures: entity %d to be linked, but %d are already: the game makes that a script error\n",
               (int32_t)entity, list_of(state.cg)->count);
    orig_CScr_LinkToCamera(instance, entity, third, fourth, fifth, sixth);
    aae_linkcam_note(&state.noted, state.cg != NULL ? list_of(state.cg) : NULL);
    if (debug && state.cg != NULL) {
        char what[64];
        snprintf(what, sizeof(what), "linked to the camera, link %u since the start", ++told.links);
        say_list(what, (int32_t)entity);
    }
}

// delete and forcedelete: the entity is taken off the list first
static void taken_away(int32_t entnum) {
    uint8_t *cg = kept_cg();
    if (cg == NULL || aae_linkcam_remove(list_of(cg), entnum) == 0)
        return;
    aae_linkcam_note(&state.noted, list_of(cg));
    if (debug)
        say_list("deleted and taken off the list", entnum);
}

install_hook_name(CScr_Delete, void, uint64_t instance, uint64_t entity, uint64_t third, uint64_t fourth,
                  uint64_t fifth, uint64_t sixth) {
    if (aae_plugin_loaded_lately())
        taken_away((int32_t)entity);
    orig_CScr_Delete(instance, entity, third, fourth, fifth, sixth);
}

install_hook_name(CScr_ForceDelete, void, uint64_t instance, uint64_t entity, uint64_t third, uint64_t fourth,
                  uint64_t fifth, uint64_t sixth) {
    if (aae_plugin_loaded_lately())
        taken_away((int32_t)entity);
    orig_CScr_ForceDelete(instance, entity, third, fourth, fifth, sixth);
}

void aae_linkcam_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    if (!prepatch_has_hook(ADDR_CG_CalcViewValues_ViewAxis)) {
        printf("gestures: not running from the pre-patched executable: All-around Enhancement's left-hand gestures "
               "stay as the game has them, the hand not put at the camera\n");
        return;
    }
    aae_plugin_init(game_base_address);
    const char *env = getenv("BO3MACFIX_LINKCAMDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    cg_clients = (uint8_t *const *)(game_base_address + ADDR_cgArray);
    local_client_count = (const int32_t *)(game_base_address + ADDR_LOCAL_CLIENT_COUNT);
    AnglesToAxis = (void *)(game_base_address + ADDR_AnglesToAxis);
    CG_EntityPoseChanged = (void *)(game_base_address + ADDR_CG_EntityPoseChanged);
    install_hook_CScr_LinkToCamera((void *)(game_base_address + ADDR_CScr_LinkToCamera));
    install_hook_CScr_Delete((void *)(game_base_address + ADDR_CScr_Delete));
    install_hook_CScr_ForceDelete((void *)(game_base_address + ADDR_CScr_ForceDelete));
    install_hook_AddDObjToScene((void *)(game_base_address + ADDR_AddDObjToScene));
    install_detour_View_Axis((void *)(game_base_address + ADDR_CG_CalcViewValues_ViewAxis));
}
