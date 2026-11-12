// MoreMapMemory: a second 2 GB of address space for the files the game loads, used when the first is full, as
// All-around Enhancement 4's Windows plugin provides (its pool_fix component, "AAE_native_spill").
//
// The game's memory for what it loads (PMem) is a set of stacks, 'db' for the fastfiles of the map, the mod and the
// common files. A stack takes its memory from address spaces reserved at start, one per kind of memory: 'main' for
// ordinary data, and three for the GPU's. The 'db' stack's 'main' is 2 GB. A fastfile's blocks are stacked there one
// after the other, and when a block doesn't fit the game stops with
//     Need 8883299 more bytes of 'main' address space in stack 'db' for alloc to succeed
//     Out of memory: filename '', line 0
// With All-around Enhancement loaded that happens on big custom maps: Mob of the Dead Remastered needs 9 MB more, Call
// of the Dead 13 MB, Luigi's Mansion 157 MB.
//
// A stack has room for five address spaces and 'db' uses four ('gpu render targets' is free). So when an allocation in
// 'db' doesn't fit 'main' any more, a new 2 GB address space is reserved through the game's own function, put in the
// free place, and the allocation is made from there, like every later one that doesn't fit 'main'. The stack treats
// all five alike: it remembers each one's fill with every named allocation, and frees back to that.
//
// The memory itself comes out of the game's budget of 5 GB of pages for everything it loads, which stays as it is.
#include <stdio.h>
#include <string.h>
#include "dobby.h"
#include "offsets.h"
#include "pmem_spill.h"

#define Stack_Db 0         // the fastfiles' stack
#define Stack_Size 0xf68
#define Stack_Fill 0x10    // size_t[5]: how far each of the stack's address spaces is filled
#define Stack_Pools 0xf40  // pmem_pool_t *[5]: those address spaces, NULL for none
#define Pool_Main 0
#define Pool_Spill 3       // 'gpu render targets', which the 'db' stack doesn't have
#define Pool_Count 5
#define Pool_Paged 2       // a pool whose pages are mapped as the stack grows
#define Space_Flags 0x1e   // what the game makes 'main' with
#define Spill_Size 0x80000000ULL
#define CritSec_Stacks 0x18 // + the stack's number

// an address space, as the game's own function for it sets it up
typedef struct {
    const char *name;
    uint8_t *base;
    size_t size;
    uint8_t pages[0x1018]; // which pages are mapped, and its place in the game's list of them
} address_space_t;
typedef struct {
    int32_t kind;
    int32_t pad;
    address_space_t space;
} pmem_pool_t;
_Static_assert(sizeof(pmem_pool_t) == 0x1038 && offsetof(pmem_pool_t, space) == 8, "a PMem pool");

static uint64_t base_address;
static void (*Sys_Lock)(void *lock, int critical_section, int mode);
static void (*Sys_Unlock)(void *lock);
static bool (*AddressSpace_Create)(const char *name, uint32_t flags, size_t size, address_space_t *space);
static pmem_pool_t spill; // all of this under the 'db' stack's lock
static int spill_state;   // 0 not needed yet, 1 there, -1 it couldn't be made
static uint32_t spilled_allocations;

static uint8_t *stack(int number) {
    return (uint8_t *)(base_address + ADDR_PMem_Stacks) + (size_t)number * Stack_Size;
}
static size_t *fill(int number) {
    return (size_t *)(stack(number) + Stack_Fill);
}
static pmem_pool_t **pools(int number) {
    return (pmem_pool_t **)(stack(number) + Stack_Pools);
}

// whether the stack's address space of this type has room, by the game's own test (which counts the bytes skipped
// for the alignment twice, on the safe side)
static bool fits(int number, uint32_t type, size_t size, size_t alignment) {
    const pmem_pool_t *pool = pools(number)[type];
    if (pool == NULL || alignment == 0)
        return false;
    int64_t top = (int64_t)fill(number)[type];
    int64_t start = -(int64_t)alignment & (top + (int64_t)alignment - 1);
    int64_t taken = start + (int64_t)size - top, room = (int64_t)pool->space.size - start;
    return taken <= room;
}

// the second address space, made the first time it's needed
static bool spill_ready(size_t size) {
    if (spill_state != 0)
        return spill_state > 0;
    spill_state = -1;
    const pmem_pool_t *main_pool = pools(Stack_Db)[Pool_Main];
    if (pools(Stack_Db)[Pool_Spill] != NULL || main_pool == NULL || main_pool->kind != Pool_Paged) {
        printf("map memory: the game's stack for loaded files isn't laid out as MoreMapMemory knows it\n");
        return false;
    }
    spill.kind = Pool_Paged;
    if (!AddressSpace_Create("main 2 (BO3MacFix)", Space_Flags, Spill_Size, &spill.space) || spill.space.base == NULL) {
        printf("map memory: couldn't reserve 2 GB more of address space\n");
        return false;
    }
    pools(Stack_Db)[Pool_Spill] = &spill;
    spill_state = 1;
    printf("map memory: the game's 2 GB for loaded files are full (%zu MB in use, %zu MB wanted), what doesn't fit goes "
           "into 2 GB more from here on\n", fill(Stack_Db)[Pool_Main] >> 20, size >> 20);
    return true;
}

// PMem_Alloc(size, alignment, type of address space, stack, tag): the next bytes of that stack's address space
install_hook_name(PMem_Alloc, void *, size_t size, size_t alignment, uint32_t type, int number, uint32_t tag) {
    if (type != Pool_Main || number != Stack_Db)
        return orig_PMem_Alloc(size, alignment, type, number, tag);
    // the game takes this lock inside too (it counts)
    uint64_t lock[2] = { 0 };
    Sys_Lock(lock, CritSec_Stacks + number, 0);
    if (!fits(number, Pool_Main, size, alignment) && spill_ready(size) && fits(number, Pool_Spill, size, alignment)) {
        type = Pool_Spill;
        spilled_allocations++;
    }
    void *memory = orig_PMem_Alloc(size, alignment, type, number, tag);
    Sys_Unlock(lock);
    return memory;
}

// read from another thread, for the log: the numbers can be a moment old
size_t pmem_spill_used(size_t *spilled) {
    *spilled = 0;
    if (base_address == 0)
        return 0;
    if (spill_state > 0)
        *spilled = *(volatile size_t *)&fill(Stack_Db)[Pool_Spill];
    return *(volatile size_t *)&fill(Stack_Db)[Pool_Main];
}

void pmem_spill_init(uint64_t game_base_address, int enabled) {
    base_address = game_base_address;
    if (!enabled)
        return;
    Sys_Lock = (void *)(base_address + ADDR_CritSec_ScopedLock);
    Sys_Unlock = (void *)(base_address + ADDR_CritSec_ScopedUnlock);
    AddressSpace_Create = (void *)(base_address + ADDR_AddressSpace_Create);
    install_hook_PMem_Alloc((void *)(base_address + ADDR_PMem_Alloc));
}
