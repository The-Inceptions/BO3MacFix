// lua_heap.c against a made-up game: a heap maker that keeps the callbacks it's given the way the game's does, and a
// heap that asks them the way the game's dlmalloc does when it's full (the test includes the source itself).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/lua_heap.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)address, (void)fake_func, (void)out_origin_func;
    return 0;
}

#define MB 0x100000

// ---- the made-up game
static char other_block[64];
static _Alignas(16) uint8_t ui_block[16 + sizeof(heap_t)]; // the UI VM's block, as far as the test's heap needs it
#define Ui_Heap ((heap_t *)(ui_block + 16))
static heap_t other_heap;
// the game's two globals lua_heap.c reads: the UI block's address and whether a heap has it
static uint8_t globals[ADDR_LUI_HeapBlockTaken - ADDR_LUI_HeapBlock + 1];
#define ui_block_taken (*(bool *)(globals + (ADDR_LUI_HeapBlockTaken - ADDR_LUI_HeapBlock)))
static int game_allocs, game_frees, grow_asks;
static size_t game_grow_answer; // what the game's own callback allows, 0 as the UI's always does

// the UI VM's callback: its one block, once
static void *game_ui_block(void *user, void *block, size_t old_size, size_t new_size) {
    (void)user, (void)block, (void)old_size;
    if (new_size == 0) {
        game_frees++;
        ui_block_taken = false;
        return NULL;
    }
    game_allocs++;
    if (ui_block_taken)
        return NULL;
    ui_block_taken = true;
    return ui_block;
}
static void *game_other_block(void *user, void *block, size_t old_size, size_t new_size) {
    (void)user, (void)block, (void)old_size;
    return new_size != 0 ? other_block : NULL;
}
static size_t game_grow_callback(void *heap, size_t wanted) {
    (void)heap, (void)wanted;
    grow_asks++;
    return game_grow_answer;
}

static heap_t *game_heap_create(heap_init_t *init) {
    heap_t *heap = init->alloc == game_ui_block ? Ui_Heap : &other_heap;
    memset(heap, 0, sizeof(*heap));
    heap->first_block = init->alloc(init->user, NULL, 0, init->size);
    heap->alloc = init->alloc;
    heap->user = init->user;
    heap->grow = init->grow;
    heap->used = 0x3d8;
    heap->capacity = init->size;
    return heap;
}

// the game's dlmalloc when nothing in the heap fits: by how much may it grow, then that much memory
static void *heap_wants(heap_t *heap, size_t wanted, size_t *got) {
    *got = 0;
    size_t allowed = heap->grow(heap, wanted);
    if (allowed < wanted)
        return NULL;
    void *memory = heap->alloc(heap->user, NULL, 0, allowed);
    if (memory == NULL)
        return NULL;
    heap->capacity += allowed;
    *got = allowed;
    return memory;
}

static heap_t *new_ui_heap(void) {
    ui_block_taken = false; // the game's shutdown of the last VM
    heap_init_t init = { .size = 60 * MB, .alloc = game_ui_block, .grow = game_grow_callback, .warn_percent = 100 };
    return fake_Lua_HeapCreate(&init);
}

static void test_heaps(void) {
    // another VM's heap keeps the game's callbacks
    heap_init_t other = { .size = 8 * MB, .alloc = game_other_block, .grow = game_grow_callback };
    heap_t *heap = fake_Lua_HeapCreate(&other);
    CHECK(heap == &other_heap && heap->alloc == game_other_block && heap->grow == game_grow_callback);
    size_t capacity = 1;
    CHECK(lua_heap_used(&capacity) == 0 && capacity == 0); // the UI's VM isn't up yet
    size_t got;
    CHECK(heap_wants(heap, 0x10000, &got) == NULL && heap->capacity == 8 * MB);

    // the UI's gets ours, after its first block came from the game's
    heap = new_ui_heap();
    CHECK(heap == Ui_Heap && heap->first_block == ui_block && game_allocs == 1);
    CHECK(heap->alloc == alloc && heap->grow == grow);
    CHECK(lua_heap_used(&capacity) == 0x3d8 && capacity == 60 * MB);
}

static void test_growth(void) {
    heap_t *heap = new_ui_heap();
    int asks = grow_asks, allocs = game_allocs;
    // a small request: a 4 MB block of memory that can be written, and the game's callback was asked first
    size_t got;
    uint8_t *memory = heap_wants(heap, 0x10000, &got);
    CHECK(memory != NULL && got == 4 * MB && grow_asks == asks + 1 && game_allocs == allocs);
    if (memory != NULL) {
        CHECK(memory[0] == 0 && memory[4 * MB - 1] == 0);
        memory[0] = memory[4 * MB - 1] = 0x77;
    }
    size_t capacity;
    lua_heap_used(&capacity);
    CHECK(capacity == 64 * MB && block_count == 1 && extra == 4 * MB);
    // a bigger one, in 64 KB steps
    uint8_t *second = heap_wants(heap, 5 * MB + 1, &got);
    CHECK(second != NULL && second != memory && got == 5 * MB + 0x10000 && extra == 9 * MB + 0x10000);
    // more than a block may be, and more than is left of the 24 MB
    CHECK(heap_wants(heap, 16 * MB + 1, &got) == NULL && heap_wants(heap, 15 * MB, &got) == NULL && block_count == 2);
    CHECK(heap_wants(heap, 14 * MB, &got) != NULL && got == 14 * MB && extra == 23 * MB + 0x10000);
    CHECK(heap_wants(heap, 0x10000, &got) == NULL && block_count == 3);
    CHECK(memory == NULL || (memory[0] == 0x77 && memory[4 * MB - 1] == 0x77));

    // six blocks at most
    heap = new_ui_heap();
    CHECK(block_count == 0 && extra == 0);
    for (int i = 0; i < Max_Blocks; i++)
        CHECK(heap_wants(heap, 0x2000, &got) != NULL && got == 4 * MB);
    CHECK(heap_wants(heap, 0x2000, &got) == NULL && block_count == Max_Blocks && extra == 24 * MB);
    lua_heap_used(&capacity);
    CHECK(capacity == 84 * MB);

    // the UI's VM starts over: its heap is the 60 MB again, and can grow again
    heap = new_ui_heap();
    lua_heap_used(&capacity);
    CHECK(capacity == 60 * MB && block_count == 0 && extra == 0);
    CHECK(heap_wants(heap, 16 * MB, &got) != NULL && got == 16 * MB);
}

static void test_game_first(void) {
    // when the game's own callback allows growth, the memory is the game's to give
    heap_t *heap = new_ui_heap();
    int allocs = game_allocs;
    size_t got;
    game_grow_answer = 2 * MB;
    CHECK(heap_wants(heap, 0x10000, &got) == NULL && game_allocs == allocs + 1 && block_count == 0);
    game_grow_answer = 0;
    // a request for memory that grow didn't allow just before goes to the game too
    CHECK(heap->grow(heap, 0x10000) == 4 * MB);
    CHECK(heap->alloc(heap->user, NULL, 0, 8 * MB) == NULL && game_allocs == allocs + 2 && block_count == 0);
    CHECK(heap->alloc(heap->user, NULL, 0, 4 * MB) == NULL && game_allocs == allocs + 3 && block_count == 0);
    // nothing is wanted: nothing is given
    CHECK(heap->grow(heap, 0) == 0);
}

static void test_blocks_back(void) {
    heap_t *heap = new_ui_heap();
    size_t got;
    void *a = heap_wants(heap, 0x10000, &got), *b = heap_wants(heap, 6 * MB, &got), *c = heap_wants(heap, 0x10000, &got);
    CHECK(a != NULL && b != NULL && c != NULL && extra == 14 * MB);
    // one of ours back: its memory is free and counts no more; the game's callback isn't told
    int frees = game_frees;
    CHECK(heap->alloc(heap->user, b, 6 * MB, 0) == NULL && game_frees == frees && block_count == 2 && extra == 8 * MB);
    CHECK(heap_wants(heap, 16 * MB, &got) != NULL && extra == 24 * MB);
    // the heap's own first block back is the game's business
    CHECK(heap->alloc(heap->user, ui_block, 60 * MB, 0) == NULL && game_frees == frees + 1 && block_count == 3);
}

int main(void) {
    orig_Lua_HeapCreate = game_heap_create;
    lua_heap_init((uint64_t)globals - ADDR_LUI_HeapBlock, 1);
    game_ui_alloc = game_ui_block;
    uint8_t *block = ui_block;
    memcpy(globals, &block, sizeof(block));
    test_heaps();
    test_growth();
    test_game_first();
    test_blocks_back();
    if (failures != 0) {
        printf("test_lua_heap: %d failures\n", failures);
        return 1;
    }
    printf("test_lua_heap: all passed\n");
    return 0;
}
