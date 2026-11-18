#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// TextureSwapGuard: a texture view the game replaces while the render thread may still be reading it goes to the
// game's own delayed release instead of dying at once (see texture_swap_guard.c)
void texture_swap_guard_init(uint64_t game_base_address, int enabled);

// a place that untracks a texture view and releases it right after, while a field the render thread reads may still
// lead to it (or did a moment ago)
typedef struct {
    uint32_t return_offset;    // the instruction after its call of the untrack function, from the game's base
    uint32_t unguarded_caller; // a return offset into a caller of the place's function for which the place needs no
                               // guard (nothing can be reading the field then), 0 for none
    const char *function;      // what the game does there, for the log
} texture_swap_site_t;

// the game's delayed-release ring: 8182 entries; the guard leaves this many free for the game's own users
#define Ring_Entries 8182
#define Ring_Entry_Size 16
#define Ring_Room_Kept 1024

// the place the untrack function returns to at return_offset, NULL for any other caller
const texture_swap_site_t *texture_swap_guard_site(uint64_t return_offset);
// how many entries the ring holds, from its head and tail as the game keeps them: head 0 = empty, head == tail = full
uint32_t texture_swap_guard_ring_used(uint64_t head, uint64_t tail);
// the guard's rule: a view, untracked in the category the places use (0), at one of the places, not called by the
// place's unguarded caller (site_caller_offset: where the place's function returns to, from the game's base), and
// room in the ring beyond what is kept for the game's own users
bool texture_swap_guard_delays(const texture_swap_site_t *site, const void *view, uint64_t category,
                               uint64_t site_caller_offset, uint32_t ring_used);
// whether the n-th hand-over (or pass-through) at one place gets a debug line: the first 3, then each power of two
bool texture_swap_guard_logs(uint32_t times);

// what the hook does, with the caller's return address and the frame of the game function the place is in (its rbp:
// [0] its caller's frame, [1] where it returns to): hands the view to the game's delayed release (an own reference
// first, so that the caller's release that follows leaves it alive) or calls the game's untrack
int texture_swap_guard_untrack(void *view, uint64_t category, uint64_t return_address, void *const *site_frame);
