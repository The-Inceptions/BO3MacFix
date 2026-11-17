#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// TextureSwapGuard: a texture view the game replaces or drops while the render thread may still be reading it dies a
// few frames later instead of at once (see texture_swap_guard.c)
void texture_swap_guard_init(uint64_t game_base_address, int enabled);

// a place that untracks a texture view and releases it right after, while a field the render thread reads may still
// lead to it (or did a moment ago)
typedef struct {
    uint32_t return_offset; // the instruction after its call of the untrack function, from the game's base
    const char *function;   // what the game does there, for the log
} texture_swap_site_t;

// the place the untrack function returns to at return_offset, NULL for any other caller
const texture_swap_site_t *texture_swap_guard_site(uint64_t return_offset);
// the guard's rule: a view, untracked in the category those places use (0), at one of those places
bool texture_swap_guard_delays(const texture_swap_site_t *site, const void *view, uint64_t category);
// whether the n-th hand-over at one place gets a debug line: the first 3, then each power of two
bool texture_swap_guard_logs(uint32_t times);

// what the hook does, with the caller's return address: hands the view to the game's delayed release (an own
// reference first, so that the caller's release that follows leaves it alive) or calls the game's untrack
int texture_swap_guard_untrack(void *view, uint64_t category, uint64_t return_address);
