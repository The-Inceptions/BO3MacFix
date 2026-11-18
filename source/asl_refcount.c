// AtomicRelease: an object of Aspyr's Direct3D layer that two threads release at the same moment is destroyed once,
// not twice.
//
// Every object of Aspyr's Direct3D 11 layer on Metal (ASL::DX11Metal: textures, views, buffers, states, the device and
// its context) has the virtual base IUnknown_Mac: its vtable, an external reference count at +8 (AddRef/Release) and an
// internal one at +0xc (the layer's own references, such as a view's on its texture). The game's Release (0x1001061d2)
// does `lock dec dword [this+8]` and then decides from a second, plain read of the two counts: destroy when both read
// 0. ReleaseInternal (0x100106216) does the same for +0xc. When two threads drop the last two references at nearly the
// same moment (one 2 -> 1, the other 1 -> 0, then both read 0), or one the last external and the other the last
// internal one, both call the deleting destructor and the object's memory is freed twice. Run under Rosetta with the
// game's own bytes, two threads releasing together destroyed an object twice 49 times in 3,000,000 tries (Release with
// Release) and 9 times (Release with ReleaseInternal) (wip/night/crash-render.md, wip/agents/crashrender/race/). The
// render thread's crash of 2026-11-13 ended in exactly such a second free, inside this Release.
//
// With this on, both functions are replaced (through the pre-patched executable's hooks; with it off their slots stay
// empty and the game's own code runs): one atomic compare-and-exchange of the 8-byte word holding both counts lowers
// the one count, so "both counts are 0 now" is a single fact that exactly one caller sees, and only that one destroys.
// AddRef and AddRefInternal stay as they are (32-bit `lock xadd` on one half); an exchange of the whole word fails and
// is tried again whenever either half changed in between (tried under Rosetta: counts exact after 6 million mixed
// pairs). Each half is computed as the game computes it, wrapping at 32 bits, so only the decision changes. A release
// of a count that is 0 already is a fault elsewhere (the object was released once too often); it is logged with the
// object's vtable, which names its class (wip/agents/crashrender/tools/macho.py reads the class names), and it destroys
// nothing, as in the game. This does not cure a reference taken on an object another thread is already destroying
// (TextureSwapGuard keeps the game from handing out such views).
//
// Cost, measured under Rosetta: the hook's stub adds nothing measurable; the 8-byte atomic makes an AddRef and
// Release pair take about 6.7 ns instead of 3.7.
#include <stdio.h>
#include <stdlib.h>
#include "asl_refcount.h"
#include "dobby.h"
#include "offsets.h"

#define Counts_At 8          // the two counts after IUnknown_Mac's vtable pointer
#define Destroy_Entry 4      // vtable entry (+0x20): the deleting destructor
#define Internal_Shift 32    // the internal count is the upper half of the word
#define Log_Lines 8

static uint64_t base_address;
static uint32_t releases_too_many;

uint32_t asl_refcount_release(void *object, refcount_kind_t kind, bool *was_zero) {
    uint64_t *counts = (uint64_t *)((uint8_t *)object + Counts_At);
    unsigned shift = kind == Refcount_Internal ? Internal_Shift : 0;
    uint64_t old = __atomic_load_n(counts, __ATOMIC_RELAXED), now;
    uint32_t count;
    do {
        count = (uint32_t)(old >> shift);
        uint32_t lowered = count - 1;
        now = (old & ~((uint64_t)UINT32_MAX << shift)) | ((uint64_t)lowered << shift);
    } while (!__atomic_compare_exchange_n(counts, &old, now, true, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
    *was_zero = count == 0;
    if (now == 0) {
        void (*destroy)(void *) = (*(void (***)(void *))object)[Destroy_Entry];
        destroy(object);
        return 0;
    }
    return (uint32_t)(now >> shift);
}

bool asl_refcount_logs(uint32_t times) {
    return times != 0 && (times <= Log_Lines || (times & (times - 1)) == 0);
}

static void release_too_many(void *object, refcount_kind_t kind) {
    uint32_t times = __atomic_add_fetch(&releases_too_many, 1, __ATOMIC_RELAXED);
    if (!asl_refcount_logs(times))
        return;
    uint64_t vtable = *(uint64_t *)object;
    printf("refcount: a %s of an object whose count was 0 already (object %p, vtable game+0x%llx, %u times): released "
           "once too often somewhere\n", kind == Refcount_Internal ? "ReleaseInternal" : "Release", object,
           (unsigned long long)(vtable - base_address), times);
}

// Release(IUnknown_Mac *this)
install_hook_name(ASL_Release, uint32_t, void *object) {
    bool was_zero;
    uint32_t left = asl_refcount_release(object, Refcount_External, &was_zero);
    if (was_zero)
        release_too_many(object, Refcount_External);
    return left;
}

// ReleaseInternal(IUnknown_Mac *this)
install_hook_name(ASL_ReleaseInternal, uint32_t, void *object) {
    bool was_zero;
    uint32_t left = asl_refcount_release(object, Refcount_Internal, &was_zero);
    if (was_zero)
        release_too_many(object, Refcount_Internal);
    return left;
}

void asl_refcount_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    install_hook_ASL_Release((void *)(base_address + ADDR_ASL_Release));
    install_hook_ASL_ReleaseInternal((void *)(base_address + ADDR_ASL_ReleaseInternal));
    const char *env = getenv("BO3MACFIX_ATOMICRELEASEDEBUG");
    if (env != NULL && atoi(env) != 0)
        printf("refcount: Release and ReleaseInternal of the Direct3D layer decide with one atomic exchange\n");
}
