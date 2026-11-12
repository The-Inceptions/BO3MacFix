// LuaHeapGrowth: more memory for the UI's Lua VM when its 60 MB are full, as All-around Enhancement 4's Windows plugin
// gives it (its heap_growth_guard component).
//
// Each of the game's Lua VMs gets one block of memory when it starts, 60 MB for the UI's ("LUI_Main"), and keeps a
// dlmalloc heap in it. When the heap can't serve a request it asks its owner through two callbacks: first by how much
// it may grow, then for that memory. The UI's answer to the first is always 0. The allocation then fails, and the game
// stops with "Lua State "UI": Out of memory". With a mod as big as All-around Enhancement on top of a map's own menus,
// 60 MB can run out; on Kino with the mod the VM holds 21 MB.
//
// So the two callbacks of the UI VM's heap are replaced when the heap is made (Lua_HeapCreate, hooked): the game's own
// are asked first, and when they give nothing the heap gets a block of at least 4 MB of new memory, up to 6 blocks and
// 24 MB in all, the plugin's numbers. dlmalloc takes memory from anywhere as another segment of its heap. A VM that
// stays within its 60 MB never gets here, and nothing changes for it.
//
// The heap never gives a segment back while it lives (its trim threshold is off), and nothing tells its owner when it
// ends: the blocks are freed when the UI's VM is made anew, which starts a new heap in the same 60 MB. (The plugin
// keeps its blocks, and its 24 MB are for the whole time the game runs.)
#include <stdbool.h>
#include <stdio.h>
#include <sys/mman.h>
#include "dobby.h"
#include "lua_heap.h"
#include "offsets.h"

#define Block_Step 0x10000    // blocks come in 64 KB steps
#define Block_Min 0x400000    // of 4 MB at least
#define Block_Max 0x1000000   // and 16 MB at most: nothing bigger is asked for by a VM that isn't broken
#define Extra_Max 0x1800000   // 24 MB on top of the VM's own memory
#define Max_Blocks 6

// what Lua_HeapCreate is given
typedef struct {
    size_t size;
    void *(*alloc)(void *user, void *block, size_t old_size, size_t new_size); // new_size 0 gives a block back
    void *user;
    size_t (*grow)(void *heap, size_t wanted); // how much the heap may grow by when it wants this much, 0 for nothing
    uint32_t warn_percent;
    void (*warn)(void *heap, size_t used, size_t capacity);
} heap_init_t;

// the heap: dlmalloc's state, then these
typedef struct {
    uint8_t dlmalloc[0x390];
    void *(*alloc)(void *user, void *block, size_t old_size, size_t new_size);
    void *user;
    void *first_block;
    size_t used, capacity;
    size_t (*grow)(void *heap, size_t wanted);
} heap_t;
_Static_assert(offsetof(heap_t, used) == 0x3a8 && offsetof(heap_t, grow) == 0x3b8, "the Lua heap");

static uint64_t base_address;
static void *game_ui_alloc; // the callback the game gives the UI VM's heap, which tells that heap from the others'
static heap_t *ui_heap;     // all of this only from the thread that holds the UI VM's lock
static void *(*game_alloc)(void *user, void *block, size_t old_size, size_t new_size);
static size_t (*game_grow)(void *heap, size_t wanted);
static struct {
    void *memory;
    size_t size;
} blocks[Max_Blocks];
static int block_count;
static size_t extra;   // what the blocks hold together
static size_t granted; // what grow just allowed, for the alloc that follows it
static bool said_full;

// the heap can't serve a request and wants this much more
static size_t grow(void *heap, size_t wanted) {
    granted = 0;
    size_t allowed = game_grow(heap, wanted);
    if (allowed != 0 || heap != ui_heap || wanted == 0)
        return allowed;
    size_t size = (wanted + Block_Step - 1) & ~(size_t)(Block_Step - 1);
    if (size < Block_Min)
        size = Block_Min;
    if (wanted > Block_Max || size > Block_Max || block_count == Max_Blocks || extra + size > Extra_Max) {
        if (!said_full) {
            said_full = true;
            printf("lua heap: the UI's Lua VM is out of memory and can't have %zu KB more (it got %zu MB in %d blocks "
                   "on top of its %zu MB)\n", wanted >> 10, extra >> 20, block_count, (ui_heap->capacity - extra) >> 20);
        }
        return 0;
    }
    granted = size;
    return size;
}

// the heap asks for a block (the one it starts in, or one grow allowed), or gives one back
static void *alloc(void *user, void *block, size_t old_size, size_t new_size) {
    size_t allowed = granted;
    granted = 0;
    for (int i = 0; block != NULL && new_size == 0 && i < block_count; i++) {
        if (blocks[i].memory == block) {
            // one of ours back (dlmalloc only does that when it trims, which the game has turned off)
            munmap(blocks[i].memory, blocks[i].size);
            extra -= blocks[i].size;
            blocks[i] = blocks[--block_count];
            return NULL;
        }
    }
    if (block != NULL || new_size == 0 || new_size != allowed)
        return game_alloc(user, block, old_size, new_size);
    void *memory = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED)
        return NULL;
    blocks[block_count].memory = memory;
    blocks[block_count].size = new_size;
    block_count++;
    extra += new_size;
    printf("lua heap: the UI's Lua VM was out of memory (%zu of %zu MB), it has %zu MB more now, %zu of %d in all\n",
           ui_heap->used >> 20, ui_heap->capacity >> 20, new_size >> 20, extra >> 20, Extra_Max >> 20);
    return memory;
}

// Lua_HeapCreate(init): takes the heap's first block from init's callback and sets the heap up in it
install_hook_name(Lua_HeapCreate, heap_t *, heap_init_t *init) {
    heap_t *heap = orig_Lua_HeapCreate(init);
    if (heap == NULL || init->alloc != game_ui_alloc || heap->alloc != game_ui_alloc)
        return heap;
    // the UI's VM starts over: what the last one got on top is free
    if (block_count > 0)
        printf("lua heap: the UI's Lua VM starts over, the %zu MB its last heap got on top are freed\n", extra >> 20);
    for (int i = 0; i < block_count; i++)
        munmap(blocks[i].memory, blocks[i].size);
    block_count = 0;
    extra = granted = 0;
    said_full = false;
    ui_heap = heap;
    game_alloc = heap->alloc;
    game_grow = heap->grow;
    heap->alloc = alloc;
    heap->grow = grow;
    return heap;
}

// read from another thread than the VM's, for the log: the numbers can be a moment old
size_t lua_heap_used(size_t *capacity) {
    *capacity = 0;
    if (base_address == 0 || !*(volatile bool *)(base_address + ADDR_LUI_HeapBlockTaken))
        return 0;
    const uint8_t *block = *(uint8_t *volatile *)(base_address + ADDR_LUI_HeapBlock);
    if (block == NULL)
        return 0;
    const heap_t *heap = (const heap_t *)(block + 16);
    *capacity = heap->capacity;
    return heap->used;
}

void lua_heap_init(uint64_t game_base_address, int enabled) {
    base_address = game_base_address;
    if (!enabled)
        return;
    game_ui_alloc = (void *)(game_base_address + ADDR_LUI_HeapAlloc);
    install_hook_Lua_HeapCreate((void *)(game_base_address + ADDR_Lua_HeapCreate));
}
