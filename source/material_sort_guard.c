// MaterialSortGuard: a surface whose material the renderer hasn't sorted into its table yet is left out of that
// frame's draw lists instead of crashing the game.
//
// The renderer draws a surface by the material index in the surface's 64-bit key (bits 27..42): its sorted material
// table (rgp.sortedMaterials, 0x5800 slots) gives the material, the material its technique set (+0x278). The index
// comes from the material itself: whoever adds a surface copies the material's own key (+0x10) into the surface's.
// Only the material sort writes those keys and the table, and it runs only when a world is loaded, no zone is
// being loaded and something marked it needed (a new material, the world's load), at the start of a frame, with the
// render thread waited for. R_Init empties the whole table. So between a zone load or an R_Init and the next sort, a
// material's key can name a slot the last sort didn't fill: the key it came with from its file, or an index from
// before the table was emptied. Every frame drawn meanwhile reads that slot as the surface's material, and an empty
// slot (or one past the table's end) is followed at once: the main thread's pass that collects the lists' flags
// (0x1010ae4a5, Windows 0x141ce4020) reads the material's +0x278, and so do the render thread's list drawers
// (0x100fea391 with 0x100f31174, and 0x100fea546), none of them testing for one. The Windows build has the same code
// with the same gap.
//
// The user hit it 37 seconds after a start (2026-11-14, crash 2): the mod loaded from the stock MODS menu, the third
// frontend became active, and 14 ms later the lobby's mode switch started loading the zombies zones (en_zm_patch,
// zm_patch, en_zm_common, zm_common) while the menu's 3D scene went on drawing; the sort waits for those three
// seconds, and the first surface of an effects list named an empty slot (wip/night/crash-main.md).
//
// The guard: every surface gets into the frame's lists through one function, the merge (R_MergeDrawSurfLists below),
// on the main thread, which appends the surfaces of a few stages' buckets, sorted by key, to the frame's array of
// surface pointers; each draw list is a slice of that array (its count has no other writer but the frame's reset).
// After the game's merge, the surfaces it appended whose index is not one of the slots the last sort filled, or
// whose slot is empty, are taken out again (the rest keep their order) and the array's count is lowered. So neither
// the main thread nor the render thread meets them: the render thread only draws lists of a frame whose surfaces
// were checked against the table it draws with (the sort waits for it, and only the sort and R_Init write the
// table). Such a thing is missing for the frames until the sort has taken its material in, usually a few frames.
// A surface whose index names another material's filled slot is drawn with that one for those frames, as the game
// does; only slots that would crash it are guarded. A game whose keys all name filled slots is unchanged, and each
// surface costs a read of its key and of its slot, both just read by the game's own sort of the same lists.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dobby.h"
#include "material_sort_guard.h"
#include "offsets.h"

#define Key_Index_Shift 27
#define Key_Index_Mask 0xffffULL
// the renderer's permanent globals (rgp), at ADDR_rgp: the table first
#define Rgp_Need_Sort 0x2c000      // bool: a sort is needed (a material was added, or the world loaded)
#define Rgp_Material_Count 0x2c004 // int: how many slots the last sort filled
#define Rgp_World 0x2c320          // GfxWorld *: the loaded world, NULL without one (the sort waits for one)
// the frame's data the merge appends to (its third argument)
#define Data_Surfaces 0x14fb0      // GfxDrawSurf *[Data_Surface_Most]
#define Data_Surface_Count 0x8a36c // int
#define Data_Surface_Most 60000

// ---- the rules

uint32_t material_sort_guard_index(uint64_t key) {
    return (uint32_t)((key >> Key_Index_Shift) & Key_Index_Mask);
}

material_sort_reason_t material_sort_guard_check(uint64_t key, const void *const *table, uint32_t material_count) {
    uint32_t filled = material_count < Material_Sort_Table_Size ? material_count : Material_Sort_Table_Size;
    uint32_t index = material_sort_guard_index(key);
    if (index >= filled)
        return Material_Sort_Past_Count;
    return table[index] != NULL ? Material_Sort_Kept : Material_Sort_Empty_Slot;
}

static void note_drop(material_sort_drop_t *drop, uint64_t key, material_sort_reason_t reason) {
    if (drop->dropped++ != 0)
        return;
    drop->key = key;
    drop->index = material_sort_guard_index(key);
    drop->reason = (uint8_t)reason;
}

uint32_t material_sort_guard_filter(const uint64_t **surfaces, uint32_t count, const void *const *table,
                                    uint32_t material_count, material_sort_drop_t *drop) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t *surface = surfaces[i];
        material_sort_reason_t reason = Material_Sort_No_Surface;
        uint64_t key = 0;
        if (surface != NULL) {
            key = *surface;
            reason = material_sort_guard_check(key, table, material_count);
        }
        if (reason != Material_Sort_Kept) {
            note_drop(drop, key, reason);
            continue;
        }
        // nothing is written while nothing has been left out
        if (kept != i)
            surfaces[kept] = surface;
        kept++;
    }
    return kept;
}

uint64_t material_sort_guard_with_index(uint64_t key, uint32_t index) {
    return (key & ~(Key_Index_Mask << Key_Index_Shift)) | (((uint64_t)index & Key_Index_Mask) << Key_Index_Shift);
}

bool material_sort_guard_logged(uint32_t n, bool debug) {
    return n <= (debug ? 20u : 3u) || (n & (n - 1)) == 0;
}

// ---- in the game

static uint64_t base_address;
static const uint8_t *rgp;
static bool debug;
// BO3MACFIX_MATERIALSORTTEST=n: the n-th merge that appends surfaces while the table holds materials gets its first
// surface's key changed to name the slot right after the filled ones, once, as a material the sort hasn't taken in
// yet would; the guard then has to leave it out (a test of the guard in the game; the guard being on is what makes
// it safe, without the guard it isn't done)
static uint32_t test_at;
static atomic_uint test_merges;
static atomic_uint drops;           // merges that left something out
static atomic_ullong surfaces_left; // surfaces left out, all told
// the table as the last merge saw it, for the debug lines about its changes
static uint32_t seen_count = UINT32_MAX;
static uint8_t seen_need_sort = UINT8_MAX, seen_world = UINT8_MAX;

static int32_t read_int(const uint8_t *at) {
    int32_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}

static const char *reason_text(uint8_t reason) {
    switch (reason) {
    case Material_Sort_No_Surface:
        return "no surface at all in its place";
    case Material_Sort_Past_Count:
        return "at or past the slots the last sort filled";
    case Material_Sort_Empty_Slot:
        return "an empty slot";
    default:
        return "?";
    }
}

static void note_table(uint32_t material_count, uint8_t need_sort, uint8_t world) {
    if (material_count == seen_count && need_sort == seen_need_sort && world == seen_world)
        return;
    printf("material sort guard: the renderer's sorted material table holds %u materials (was %d), a sort is %s, "
           "%s world\n",
           material_count, seen_count == UINT32_MAX ? -1 : (int)seen_count, need_sort ? "pending" : "not needed",
           world ? "a" : "no");
    seen_count = material_count, seen_need_sort = need_sort, seen_world = world;
}

static void maybe_test(const uint64_t **surfaces, uint32_t appended, uint32_t material_count) {
    if (test_at == 0 || material_count == 0 || surfaces[0] == NULL)
        return;
    if (atomic_fetch_add_explicit(&test_merges, 1, memory_order_relaxed) + 1 != test_at)
        return;
    uint64_t *surface = (uint64_t *)surfaces[0];
    uint64_t key = *surface;
    *surface = material_sort_guard_with_index(key, material_count);
    printf("material sort guard: test: the first of %u surfaces had its key %016llx changed to %016llx (material "
           "index %u, right after the %u filled slots)\n",
           appended, (unsigned long long)key, (unsigned long long)*surface, material_count, material_count);
}

static void report(uint32_t stage, uint32_t stage_count, uint32_t appended, const material_sort_drop_t *drop,
                   uint32_t material_count, uint8_t need_sort, uint8_t world) {
    uint32_t n = atomic_fetch_add_explicit(&drops, 1, memory_order_relaxed) + 1;
    unsigned long long total =
        atomic_fetch_add_explicit(&surfaces_left, drop->dropped, memory_order_relaxed) + drop->dropped;
    if (!material_sort_guard_logged(n, debug))
        return;
    printf("material sort guard: %u of %u surfaces of stages %u to %u left out of this frame, their material isn't "
           "in the renderer's sorted table (the first: key %016llx, material index %u, %s); the table holds %u "
           "materials, a sort is %s, %s world (%u times, %llu surfaces so far)\n",
           drop->dropped, appended, stage, stage + stage_count - 1, (unsigned long long)drop->key, drop->index,
           reason_text(drop->reason), material_count, need_sort ? "pending" : "not needed", world ? "a" : "no", n,
           total);
}

// R_MergeDrawSurfLists(stage, stage count, the frame's data): appends the surfaces of the buckets [stage, stage +
// stage count) to the frame's array, merged by key (the game's own; it returns nothing anyone reads, passed on)
install_hook_name(R_MergeDrawSurfLists, uint64_t, uint64_t stage, uint64_t stage_count, uint8_t *data) {
    int32_t before = read_int(data + Data_Surface_Count);
    uint64_t result = orig_R_MergeDrawSurfLists(stage, stage_count, data);
    int32_t after = read_int(data + Data_Surface_Count);
    if (before < 0 || after <= before || after > Data_Surface_Most)
        return result;
    const uint64_t **surfaces = (const uint64_t **)(data + Data_Surfaces) + before;
    uint32_t appended = (uint32_t)(after - before);
    uint32_t material_count = (uint32_t)read_int(rgp + Rgp_Material_Count);
    uint8_t need_sort = rgp[Rgp_Need_Sort];
    const void *world;
    memcpy(&world, rgp + Rgp_World, sizeof(world));
    if (debug)
        note_table(material_count, need_sort, world != NULL);
    maybe_test(surfaces, appended, material_count);
    material_sort_drop_t drop = { 0 };
    uint32_t kept = material_sort_guard_filter(surfaces, appended, (const void *const *)rgp, material_count, &drop);
    if (kept == appended)
        return result;
    int32_t count = before + (int32_t)kept;
    memcpy(data + Data_Surface_Count, &count, sizeof(count));
    report((uint32_t)stage, (uint32_t)stage_count, appended, &drop, material_count, need_sort, world != NULL);
    return result;
}

void material_sort_guard_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    rgp = (const uint8_t *)(base_address + ADDR_rgp);
    const char *env = getenv("BO3MACFIX_MATERIALSORTDEBUG");
    debug = env != NULL && atoi(env) != 0;
    env = getenv("BO3MACFIX_MATERIALSORTTEST");
    test_at = env != NULL && atoi(env) > 0 ? (uint32_t)atoi(env) : 0;
    install_hook_R_MergeDrawSurfLists((void *)(base_address + ADDR_R_MergeDrawSurfLists));
    if (debug || test_at != 0)
        printf("material sort guard: on, a surface whose material isn't in the renderer's sorted table is left out "
               "of the frame%s\n",
               test_at != 0 ? " (test: one surface is made such a surface)" : "");
}
