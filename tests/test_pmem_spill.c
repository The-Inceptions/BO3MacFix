// pmem_spill.c against a made-up PMem: the stacks' table, address spaces, and an allocation function that does what
// the game's does (the test includes the source itself).
#include <stdio.h>
#include <stdlib.h>
#include "../source/pmem_spill.c"

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

#define MB 0x100000ULL
#define Main_Base ((uint8_t *)0x100000000000)  // nothing is read or written there
#define Spill_Base ((uint8_t *)0x200000000000)
#define Stack_Game 2

// ---- the made-up game
static uint8_t stacks[11 * Stack_Size];
static pmem_pool_t main_pool, gpu_pool, game_pool;
static int lock_depth, out_of_memory, spaces_made, refuse_spaces;
static uint32_t last_type;

static void game_lock(void *lock, int critical_section, int mode) {
    (void)lock, (void)mode;
    CHECK(critical_section == CritSec_Stacks + Stack_Db);
    lock_depth++;
}
static void game_unlock(void *lock) {
    (void)lock;
    CHECK(lock_depth > 0);
    lock_depth--;
}
static bool game_space_create(const char *name, uint32_t flags, size_t size, address_space_t *space) {
    CHECK(lock_depth > 0 && flags == Space_Flags && size == Spill_Size && name != NULL);
    if (refuse_spaces)
        return false;
    spaces_made++;
    memset(space->pages, 0, sizeof(space->pages));
    space->name = name;
    space->base = Spill_Base;
    space->size = size;
    return true;
}
// the game's PMem_Alloc: "Need N more bytes" and a stop when it doesn't fit (here: NULL and a count)
static void *game_alloc(size_t size, size_t alignment, uint32_t type, int number, uint32_t tag) {
    (void)tag;
    last_type = type;
    const pmem_pool_t *pool = pools(number)[type];
    if (pool == NULL) {
        out_of_memory++;
        return NULL;
    }
    size_t top = fill(number)[type];
    size_t start = -alignment & (top + alignment - 1);
    if (start + size - top > pool->space.size - start) {
        out_of_memory++;
        return NULL;
    }
    fill(number)[type] = start + size;
    return pool->space.base + start;
}

static void new_game(void) {
    memset(stacks, 0, sizeof(stacks));
    memset(&spill, 0, sizeof(spill));
    spill_state = 0;
    spilled_allocations = 0;
    lock_depth = out_of_memory = spaces_made = refuse_spaces = 0;
    main_pool = (pmem_pool_t){ .kind = Pool_Paged, .space = { .name = "main", .base = Main_Base, .size = 2048 * MB } };
    gpu_pool = (pmem_pool_t){ .kind = Pool_Paged, .space = { .name = "gpu private", .base = Main_Base + 0x10000000000, .size = 4096 * MB } };
    game_pool = (pmem_pool_t){ .kind = Pool_Paged, .space = { .name = "main", .base = Main_Base + 0x20000000000, .size = 64 * MB } };
    pools(Stack_Db)[Pool_Main] = &main_pool;
    pools(Stack_Db)[1] = &gpu_pool;
    pools(Stack_Game)[Pool_Main] = &game_pool;
}

static void test_room_in_main(void) {
    new_game();
    // while 'main' has room nothing changes: no second address space, the same pointers
    void *a = fake_PMem_Alloc(1000 * MB, 0x1000, Pool_Main, Stack_Db, 0);
    void *b = fake_PMem_Alloc(1048 * MB, 0x10000, Pool_Main, Stack_Db, 0);
    CHECK(a == Main_Base && b == Main_Base + 1000 * MB && spaces_made == 0 && lock_depth == 0);
    CHECK(fill(Stack_Db)[Pool_Main] == 2048 * MB && pools(Stack_Db)[Pool_Spill] == NULL);
    size_t more = 1;
    CHECK(pmem_spill_used(&more) == 2048 * MB && more == 0);
}

static void test_spill(void) {
    new_game();
    fake_PMem_Alloc(2040 * MB, 0x1000, Pool_Main, Stack_Db, 0);
    // 9 MB into the 8 that are left: the second address space is made, once, and the block comes from it
    uint8_t *block = fake_PMem_Alloc(9 * MB, 0x10000, Pool_Main, Stack_Db, 0);
    CHECK(block == Spill_Base && spaces_made == 1 && out_of_memory == 0 && last_type == Pool_Spill);
    CHECK(pools(Stack_Db)[Pool_Spill] == &spill && spill.kind == Pool_Paged && spill.space.size == Spill_Size);
    CHECK(fill(Stack_Db)[Pool_Main] == 2040 * MB && fill(Stack_Db)[Pool_Spill] == 9 * MB);
    // what still fits 'main' goes there, the rest on into the second
    CHECK(fake_PMem_Alloc(8 * MB, 0x1000, Pool_Main, Stack_Db, 0) == Main_Base + 2040 * MB && last_type == Pool_Main);
    block = fake_PMem_Alloc(100 * MB, 0x100000, Pool_Main, Stack_Db, 0);
    CHECK(block == Spill_Base + 9 * MB && spaces_made == 1 && spilled_allocations == 2);
    size_t more;
    CHECK(pmem_spill_used(&more) == 2048 * MB && more == 109 * MB);
    // the game's own free: both address spaces back to where a named allocation began. The second stays.
    fill(Stack_Db)[Pool_Main] = 1500 * MB;
    fill(Stack_Db)[Pool_Spill] = 0;
    CHECK(fake_PMem_Alloc(500 * MB, 0x1000, Pool_Main, Stack_Db, 0) == Main_Base + 1500 * MB && last_type == Pool_Main);
    CHECK(fake_PMem_Alloc(500 * MB, 0x1000, Pool_Main, Stack_Db, 0) == Spill_Base && spaces_made == 1);
    // both full: the game's own stop, asked of 'main' as before
    fill(Stack_Db)[Pool_Spill] = 2048 * MB - 0x1000;
    CHECK(fake_PMem_Alloc(500 * MB, 0x1000, Pool_Main, Stack_Db, 0) == NULL && out_of_memory == 1 && last_type == Pool_Main);
    CHECK(lock_depth == 0);
}

static void test_others_untouched(void) {
    new_game();
    // other stacks and the 'db' stack's other address spaces stop as the game stops them
    CHECK(fake_PMem_Alloc(65 * MB, 0x1000, Pool_Main, Stack_Game, 0) == NULL && out_of_memory == 1 && spaces_made == 0);
    CHECK(fake_PMem_Alloc(4097 * MB, 0x1000, 1, Stack_Db, 0) == NULL && out_of_memory == 2 && spaces_made == 0);
    CHECK(fake_PMem_Alloc(16 * MB, 0x1000, 1, Stack_Db, 0) == gpu_pool.space.base && lock_depth == 0);
}

static void test_no_spill(void) {
    // the free place isn't free in this game: nothing is touched, the game stops as before
    new_game();
    pools(Stack_Db)[Pool_Spill] = &gpu_pool;
    fake_PMem_Alloc(2040 * MB, 0x1000, Pool_Main, Stack_Db, 0);
    CHECK(fake_PMem_Alloc(9 * MB, 0x1000, Pool_Main, Stack_Db, 0) == NULL && out_of_memory == 1 && spaces_made == 0);
    CHECK(pools(Stack_Db)[Pool_Spill] == &gpu_pool && last_type == Pool_Main);
    CHECK(fake_PMem_Alloc(9 * MB, 0x1000, Pool_Main, Stack_Db, 0) == NULL && out_of_memory == 2);
    // the address space can't be reserved: the same, and it isn't tried again
    new_game();
    refuse_spaces = 1;
    fake_PMem_Alloc(2040 * MB, 0x1000, Pool_Main, Stack_Db, 0);
    CHECK(fake_PMem_Alloc(9 * MB, 0x1000, Pool_Main, Stack_Db, 0) == NULL && out_of_memory == 1);
    refuse_spaces = 0;
    CHECK(fake_PMem_Alloc(9 * MB, 0x1000, Pool_Main, Stack_Db, 0) == NULL && spaces_made == 0 &&
          pools(Stack_Db)[Pool_Spill] == NULL && lock_depth == 0);
}

static void test_fit_rule(void) {
    // the game's test counts the bytes skipped for the alignment against the room: one that the game would refuse
    // must go to the second address space, never to the game's stop
    new_game();
    fill(Stack_Db)[Pool_Main] = 2048 * MB - 0x18000;
    // from 0x...e8000, aligned to 0x10000: starts at 0x...f0000, 0x10000 long: exactly the end. The game takes
    // 0x18000 (with the gap) against 0x10000 of room, and refuses.
    CHECK(!fits(Stack_Db, Pool_Main, 0x10000, 0x10000));
    CHECK(fake_PMem_Alloc(0x10000, 0x10000, Pool_Main, Stack_Db, 0) == Spill_Base && out_of_memory == 0);
    CHECK(fits(Stack_Db, Pool_Main, 0x8000, 0x1000) && !fits(Stack_Db, Pool_Main, 0x18001, 1) &&
          fits(Stack_Db, Pool_Main, 0x18000, 1));
    CHECK(!fits(Stack_Db, 4, 1, 1) && !fits(Stack_Db, Pool_Main, 1, 0));
}

int main(void) {
    orig_PMem_Alloc = game_alloc;
    pmem_spill_init((uint64_t)stacks - ADDR_PMem_Stacks, 1);
    Sys_Lock = game_lock, Sys_Unlock = game_unlock, AddressSpace_Create = game_space_create;
    test_room_in_main();
    test_spill();
    test_others_untouched();
    test_no_spill();
    test_fit_rule();
    if (failures != 0) {
        printf("test_pmem_spill: %d failures\n", failures);
        return 1;
    }
    printf("test_pmem_spill: all passed\n");
    return 0;
}
