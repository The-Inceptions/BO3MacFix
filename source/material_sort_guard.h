#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// MaterialSortGuard: a surface whose material the renderer hasn't sorted into its table yet is left out of that
// frame's draw lists instead of crashing the game (see material_sort_guard.c)
void material_sort_guard_init(uint64_t game_base_address, int enabled);

// the renderer's sorted material table: Material *[Material_Sort_Table_Size] at the start of its permanent globals
#define Material_Sort_Table_Size 0x5800

// why a surface was left out
typedef enum {
    Material_Sort_Kept,         // its material's slot is one the last sort filled
    Material_Sort_No_Surface,   // the list held no surface at all (NULL), nothing to read a key from
    Material_Sort_Past_Count,   // its index is at or past the slots the last sort filled (or past the table)
    Material_Sort_Empty_Slot,   // its index is below that count, but the slot is empty
} material_sort_reason_t;

// what one merge left out: how many, and the first of them
typedef struct {
    uint32_t dropped;
    uint64_t key;    // the first left-out surface's key (0 for a NULL surface)
    uint32_t index;  // its material index
    uint8_t reason;  // material_sort_reason_t
} material_sort_drop_t;

// the material index a draw surface's key holds (bits 27..42), as the game's readers take it
// ((key >> 0x18) & 0x7fff8 as a byte offset into the table)
uint32_t material_sort_guard_index(uint64_t key);
// whether a surface with this key can be drawn: its index is below the count the last sort filled (and inside the
// table) and that slot holds a material; the table is only read for an index inside it
material_sort_reason_t material_sort_guard_check(uint64_t key, const void *const *table, uint32_t material_count);
// leaves out of surfaces[0, count) every surface that can't be drawn, keeping the others in their order; returns
// how many are kept (the first that many entries), and adds what was left out to *drop. An entry is a pointer to a
// draw surface, whose first 8 bytes are its key. Entries are only written from the first one left out on.
uint32_t material_sort_guard_filter(const uint64_t **surfaces, uint32_t count, const void *const *table,
                                    uint32_t material_count, material_sort_drop_t *drop);
// a key whose material index is changed to index, the rest as it was (for the test switch)
uint64_t material_sort_guard_with_index(uint64_t key, uint32_t index);
// whether the n-th merge that left something out (counted from 1) gets a line in the log: the first 3, then each
// power of two; with debug the first 20, then each power of two
bool material_sort_guard_logged(uint32_t n, bool debug);
