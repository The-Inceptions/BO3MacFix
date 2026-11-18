#pragma once
#include <stdbool.h>
#include <stdint.h>

// AtomicRelease: an object of Aspyr's Direct3D layer that two threads release at the same moment is destroyed once,
// not twice (see asl_refcount.c)
void asl_refcount_init(uint64_t game_base_address, int enabled);

// which of the two counts a release drops
typedef enum {
    Refcount_External = 0, // Release: the count at +8
    Refcount_Internal = 1, // ReleaseInternal: the count at +0xc
} refcount_kind_t;

// drops one reference of the IUnknown_Mac part at object (vtable at +0, the external count at +8, the internal one at
// +0xc, one aligned 8-byte word) with one atomic exchange of the whole word, so that exactly one caller sees both
// counts reach 0, and that caller calls the deleting destructor (vtable entry 4). Each count is computed as the game
// computes it (32 bits, wrapping). Returns the dropped count's new value, as the game's functions do; *was_zero is set
// when that count was 0 already (a release too many somewhere: the count wraps and nothing is destroyed)
uint32_t asl_refcount_release(void *object, refcount_kind_t kind, bool *was_zero);
// whether the n-th release too many gets a line in the log: the first 8, then each power of two
bool asl_refcount_logs(uint32_t times);
