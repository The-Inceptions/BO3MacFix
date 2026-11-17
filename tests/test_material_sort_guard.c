// material_sort_guard.c: which draw surfaces are left out of a frame (their material index names no slot the last
// sort filled), and the hook against a made-up merge, frame and renderer table (the test includes the source itself).
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../source/material_sort_guard.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Base 0x100000000ULL
#define Rgp_Size 0x2d300           // what R_Init clears of the renderer's permanent globals
#define Data_Size (Data_Surface_Count + 4)

// --- the made-up game --------------------------------------------------------------------------------------------
static void *hooked_at, *hooked_with;
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)out_origin_func;
    hooked_at = address, hooked_with = fake_func, hooks++;
    return 0;
}

// a key as the sort writes it (the bits it sets around the index, 0xfffe0007f7f80000) with this material index
static uint64_t sorted_key(uint32_t index) {
    return 0xfffe000007f80000ULL | ((uint64_t)index << 27);
}

// materials (only their addresses matter) and the renderer's globals: the table first, the rest at their offsets
static uint8_t materials[8][0x2a0];
static uint8_t *game_rgp;
static void set_table(uint32_t material_count, uint8_t need_sort, bool world) {
    memset(game_rgp, 0, Rgp_Size);
    void **table = (void **)game_rgp;
    for (uint32_t i = 0; i < material_count && i < Material_Sort_Table_Size; i++)
        table[i] = materials[i % 8];
    memcpy(game_rgp + Rgp_Material_Count, &material_count, sizeof(material_count));
    game_rgp[Rgp_Need_Sort] = need_sort;
    void *w = world ? (void *)materials : NULL;
    memcpy(game_rgp + Rgp_World, &w, sizeof(w));
}

// the frame's data: its array of surface pointers and their count
static uint8_t *game_data;
static int32_t data_count(void) {
    int32_t n;
    memcpy(&n, game_data + Data_Surface_Count, sizeof(n));
    return n;
}
static void set_data_count(int32_t n) {
    memcpy(game_data + Data_Surface_Count, &n, sizeof(n));
}
static const uint64_t **data_surfaces(void) {
    return (const uint64_t **)(game_data + Data_Surfaces);
}

// the game's merge: appends what the test prepared for it (as the game's, up to the array's end), remembers how it
// was called
static const uint64_t *to_append[64];
static uint32_t to_append_count;
static int merges;
static uint64_t merged_stage, merged_stage_count;
static uint8_t *merged_data;
#define Merge_Result 0x5eed5eed5eedULL
static uint64_t game_merge(uint64_t stage, uint64_t stage_count, uint8_t *data) {
    merges++;
    merged_stage = stage, merged_stage_count = stage_count, merged_data = data;
    int32_t n;
    memcpy(&n, data + Data_Surface_Count, sizeof(n));
    if (n + (int32_t)to_append_count > Data_Surface_Most)
        return Merge_Result;
    for (uint32_t i = 0; i < to_append_count && n >= 0; i++)
        ((const uint64_t **)(data + Data_Surfaces))[n + i] = to_append[i];
    n += (int32_t)to_append_count;
    memcpy(data + Data_Surface_Count, &n, sizeof(n));
    return Merge_Result;
}

static void set_game(void) {
    if (game_rgp == NULL) {
        game_rgp = calloc(1, Rgp_Size);
        game_data = calloc(1, Data_Size);
    }
    rgp = game_rgp;
    orig_R_MergeDrawSurfLists = game_merge;
    debug = false;
    test_at = 0;
    atomic_store(&test_merges, 0);
    atomic_store(&drops, 0);
    atomic_store(&surfaces_left, 0);
    seen_count = UINT32_MAX, seen_need_sort = UINT8_MAX, seen_world = UINT8_MAX;
    set_data_count(0);
    to_append_count = 0;
    merges = 0;
}
static void append_next(const uint64_t *const *surfaces, uint32_t count) {
    memcpy(to_append, surfaces, count * sizeof(*surfaces));
    to_append_count = count;
}

// log.txt, as the guard prints to it
static char printed[16384];
static int saved_stdout;
static FILE *capture;
static void capture_start(void) {
    fflush(stdout);
    capture = tmpfile();
    saved_stdout = dup(STDOUT_FILENO);
    dup2(fileno(capture), STDOUT_FILENO);
}
static void capture_end(void) {
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    rewind(capture);
    size_t n = fread(printed, 1, sizeof(printed) - 1, capture);
    printed[n] = '\0';
    fclose(capture);
}
static int lines_in(const char *text) {
    int n = 0;
    for (; *text != '\0'; text++)
        n += *text == '\n';
    return n;
}

// --- the rules ---------------------------------------------------------------------------------------------------
static void test_index(void) {
    // bits 27..42: the game's (key >> 0x18) & 0x7fff8 as a byte offset, divided by 8
    CHECK(material_sort_guard_index(sorted_key(0x1234)) == 0x1234);
    CHECK(material_sort_guard_index(sorted_key(0xffff)) == 0xffff);
    CHECK(material_sort_guard_index((0x1234ULL << 27) | 0x7ffffffULL) == 0x1234); // bits 0..26 aren't in it
    CHECK(material_sort_guard_index((0x1234ULL << 27) | (1ULL << 43)) == 0x1234);  // nor bit 43
    CHECK(material_sort_guard_index(1ULL << 27) == 1);
    CHECK(material_sort_guard_index(1ULL << 42) == 0x8000);
    for (uint64_t key = 0x0123456789abcdefULL, i = 0; i < 1000; i++, key = key * 6364136223846793005ULL + 1)
        CHECK(material_sort_guard_index(key) * 8 == ((key >> 0x18) & 0x7fff8)); // the readers' own computation
}

static void test_check(void) {
    // a table of exactly 0x5800 slots on the heap: a read past it is the address sanitizer's error
    const void **table = calloc(Material_Sort_Table_Size, sizeof(*table));
    for (int i = 0; i < 100; i++)
        table[i] = materials[i % 8];
    table[5] = NULL;
    table[150] = materials[1];                         // left from an older sort that filled more
    table[Material_Sort_Table_Size - 1] = materials[2];
    CHECK(material_sort_guard_check(sorted_key(4), table, 100) == Material_Sort_Kept);
    CHECK(material_sort_guard_check(sorted_key(0), table, 100) == Material_Sort_Kept);
    CHECK(material_sort_guard_check(sorted_key(99), table, 100) == Material_Sort_Kept); // the last filled slot
    CHECK(material_sort_guard_check(sorted_key(5), table, 100) == Material_Sort_Empty_Slot);
    CHECK(material_sort_guard_check(sorted_key(100), table, 100) == Material_Sort_Past_Count); // the first after
    CHECK(material_sort_guard_check(sorted_key(150), table, 100) == Material_Sort_Past_Count); // filled, but stale
    CHECK(material_sort_guard_check(sorted_key(Material_Sort_Table_Size), table, 100) == Material_Sort_Past_Count);
    CHECK(material_sort_guard_check(sorted_key(0xffff), table, 100) == Material_Sort_Past_Count);
    // right after R_Init: nothing filled
    CHECK(material_sort_guard_check(sorted_key(0), table, 0) == Material_Sort_Past_Count);
    // a count past the table (a raised material pool): never a slot past the table
    CHECK(material_sort_guard_check(sorted_key(Material_Sort_Table_Size - 1), table, 0x6000) == Material_Sort_Kept);
    CHECK(material_sort_guard_check(sorted_key(Material_Sort_Table_Size), table, 0x6000) == Material_Sort_Past_Count);
    CHECK(material_sort_guard_check(sorted_key(0xffff), table, 0xffffffff) == Material_Sort_Past_Count);
    CHECK(material_sort_guard_check(sorted_key(0x57ff), table, Material_Sort_Table_Size) == Material_Sort_Kept);
    free(table);
}

static void test_filter(void) {
    const void **table = calloc(Material_Sort_Table_Size, sizeof(*table));
    for (int i = 0; i < 10; i++)
        table[i] = materials[i % 8];
    table[3] = NULL;
    uint64_t a = sorted_key(1), b = sorted_key(10), c = sorted_key(2), d = sorted_key(9), e = sorted_key(3);
    uint64_t f = sorted_key(0x4000);

    // nothing to leave out: the array is never written (it is read-only here: a write would fault)
    size_t page = (size_t)getpagesize();
    const uint64_t **ro = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(ro != MAP_FAILED);
    ro[0] = &a, ro[1] = &c, ro[2] = &d;
    mprotect(ro, page, PROT_READ);
    material_sort_drop_t drop = { 0 };
    CHECK(material_sort_guard_filter(ro, 3, table, 10, &drop) == 3);
    CHECK(drop.dropped == 0);
    CHECK(ro[0] == &a && ro[1] == &c && ro[2] == &d);
    munmap(ro, page);

    // past the count, NULL, an empty slot: out; the rest keep their order
    const uint64_t *list[] = { &a, &b, &c, NULL, &d, &e, &a };
    memset(&drop, 0, sizeof(drop));
    CHECK(material_sort_guard_filter(list, 7, table, 10, &drop) == 4);
    CHECK(list[0] == &a && list[1] == &c && list[2] == &d && list[3] == &a);
    CHECK(drop.dropped == 3);
    CHECK(drop.key == b && drop.index == 10 && drop.reason == Material_Sort_Past_Count); // the first left out
    // a second merge adds to the count, the first stays the first
    const uint64_t *list2[] = { &e, &f };
    CHECK(material_sort_guard_filter(list2, 2, table, 10, &drop) == 0);
    CHECK(drop.dropped == 5);
    CHECK(drop.key == b && drop.index == 10 && drop.reason == Material_Sort_Past_Count);
    // the reasons, each as a first one
    const uint64_t *one[] = { NULL };
    memset(&drop, 0, sizeof(drop));
    CHECK(material_sort_guard_filter(one, 1, table, 10, &drop) == 0);
    CHECK(drop.reason == Material_Sort_No_Surface && drop.key == 0 && drop.index == 0 && drop.dropped == 1);
    one[0] = &e;
    memset(&drop, 0, sizeof(drop));
    CHECK(material_sort_guard_filter(one, 1, table, 10, &drop) == 0);
    CHECK(drop.reason == Material_Sort_Empty_Slot && drop.key == e && drop.index == 3);
    // right after R_Init every surface is out
    const uint64_t *all[] = { &a, &c, &d };
    memset(&drop, 0, sizeof(drop));
    CHECK(material_sort_guard_filter(all, 3, table, 0, &drop) == 0);
    CHECK(drop.dropped == 3 && drop.key == a && drop.reason == Material_Sort_Past_Count);
    // none at all
    memset(&drop, 0, sizeof(drop));
    CHECK(material_sort_guard_filter(all, 0, table, 10, &drop) == 0 && drop.dropped == 0);
    // the first entry out, the last kept: it moves to the front
    const uint64_t *tail[] = { &b, &b, &c };
    memset(&drop, 0, sizeof(drop));
    CHECK(material_sort_guard_filter(tail, 3, table, 10, &drop) == 1);
    CHECK(tail[0] == &c && drop.dropped == 2);
    free(table);
}

static void test_with_index(void) {
    uint64_t key = sorted_key(0x1234) | 0x7ffff | (0x3fULL << 43);
    uint64_t changed = material_sort_guard_with_index(key, 0x5800);
    CHECK(material_sort_guard_index(changed) == 0x5800);
    CHECK((changed & ~(0xffffULL << 27)) == (key & ~(0xffffULL << 27)));
    CHECK(material_sort_guard_index(material_sort_guard_with_index(key, 0x12345)) == 0x2345); // 16 bits
    uint64_t plain = sorted_key(0x1234); // bits 43..48 clear: an index of more than 16 bits would show there
    CHECK((material_sort_guard_with_index(plain, 0x12345) & ~(0xffffULL << 27)) == (plain & ~(0xffffULL << 27)));
    CHECK((material_sort_guard_with_index(plain, 0x30000) & ~(0xffffULL << 27)) == (plain & ~(0xffffULL << 27)));
    CHECK(material_sort_guard_with_index(sorted_key(7), 7) == sorted_key(7));
}

static void test_logged(void) {
    CHECK(material_sort_guard_logged(1, false) && material_sort_guard_logged(2, false));
    CHECK(material_sort_guard_logged(3, false) && material_sort_guard_logged(4, false));
    CHECK(!material_sort_guard_logged(5, false) && !material_sort_guard_logged(6, false));
    CHECK(!material_sort_guard_logged(7, false) && material_sort_guard_logged(8, false));
    CHECK(!material_sort_guard_logged(9, false) && material_sort_guard_logged(1024, false));
    CHECK(!material_sort_guard_logged(1023, false) && !material_sort_guard_logged(1025, false));
    CHECK(material_sort_guard_logged(20, true) && !material_sort_guard_logged(21, true));
    CHECK(material_sort_guard_logged(32, true) && !material_sort_guard_logged(33, true));
    int lines = 0;
    for (uint32_t n = 1; n <= 100000; n++)
        lines += material_sort_guard_logged(n, false);
    CHECK(lines == 3 + 15); // 1, 2, 3, then 4 .. 65536
}

// --- the hook ----------------------------------------------------------------------------------------------------
static void test_hook_nothing_to_leave_out(void) {
    set_game();
    set_table(20, 0, true);
    uint64_t k1 = sorted_key(1), k2 = sorted_key(19), k3 = sorted_key(0);
    const uint64_t *earlier[] = { &k3 };
    append_next(earlier, 1);
    capture_start();
    CHECK(fake_R_MergeDrawSurfLists(0x2d, 1, game_data) == Merge_Result);
    const uint64_t *mine[] = { &k1, &k2, &k1 };
    append_next(mine, 3);
    CHECK(fake_R_MergeDrawSurfLists(0x28, 3, game_data) == Merge_Result);
    capture_end();
    CHECK(merges == 2 && merged_stage == 0x28 && merged_stage_count == 3 && merged_data == game_data);
    CHECK(data_count() == 4);
    CHECK(data_surfaces()[0] == &k3 && data_surfaces()[1] == &k1 && data_surfaces()[2] == &k2);
    CHECK(data_surfaces()[3] == &k1);
    CHECK(printed[0] == '\0'); // nothing said
    CHECK(atomic_load(&drops) == 0);
}

static void test_hook_leaves_out(void) {
    set_game();
    set_table(20, 1, false);
    // an entry an earlier merge appended that would be out now: not this merge's, left alone
    uint64_t old = sorted_key(30), k1 = sorted_key(1), bad = sorted_key(25), k2 = sorted_key(2), empty = sorted_key(4);
    ((void **)game_rgp)[4] = NULL;
    set_data_count(1);
    data_surfaces()[0] = &old;
    const uint64_t *mine[] = { &bad, &k1, &empty, &k2 };
    append_next(mine, 4);
    capture_start();
    CHECK(fake_R_MergeDrawSurfLists(0x28, 3, game_data) == Merge_Result);
    capture_end();
    CHECK(data_count() == 3);
    CHECK(data_surfaces()[0] == &old && data_surfaces()[1] == &k1 && data_surfaces()[2] == &k2);
    CHECK(atomic_load(&drops) == 1 && atomic_load(&surfaces_left) == 2);
    CHECK(lines_in(printed) == 1);
    CHECK(strstr(printed, "material sort guard: 2 of 4 surfaces of stages 40 to 42 left out") != NULL);
    CHECK(strstr(printed, "key fffe0000cff80000, material index 25, at or past the slots the last sort filled") != NULL);
    CHECK(strstr(printed, "the table holds 20 materials, a sort is pending, no world (1 times, 2 surfaces so far)") != NULL);
}

static void test_hook_after_r_init(void) {
    set_game();
    set_table(0, 0, false); // what R_Init leaves: nothing, no sort marked, no world
    uint64_t k1 = sorted_key(0), k2 = sorted_key(700);
    const uint64_t *mine[] = { &k1, &k2 };
    append_next(mine, 2);
    capture_start();
    fake_R_MergeDrawSurfLists(0xf, 4, game_data);
    capture_end();
    CHECK(data_count() == 0);
    CHECK(strstr(printed, "2 of 2 surfaces of stages 15 to 18 left out") != NULL);
    CHECK(strstr(printed, "the table holds 0 materials, a sort is not needed, no world") != NULL);
}

static void test_hook_nothing_appended(void) {
    set_game();
    set_table(0, 0, false);
    uint64_t old = sorted_key(3);
    set_data_count(1);
    data_surfaces()[0] = &old;
    append_next(NULL, 0);
    fake_R_MergeDrawSurfLists(0x2c, 1, game_data);
    CHECK(data_count() == 1 && data_surfaces()[0] == &old && merges == 1);
    // the array full: the game's merge appends nothing, nothing read past it
    set_data_count(Data_Surface_Most);
    uint64_t k = sorted_key(700);
    const uint64_t *one[] = { &k };
    append_next(one, 1);
    fake_R_MergeDrawSurfLists(0x2c, 1, game_data);
    CHECK(data_count() == Data_Surface_Most && merges == 2);
    // a count the game would never have: left as it is
    set_data_count(-5);
    append_next(one, 1);
    fake_R_MergeDrawSurfLists(0x2c, 1, game_data);
    CHECK(data_count() == -4);
    CHECK(atomic_load(&drops) == 0);
}

static void test_hook_lines(void) {
    set_game();
    set_table(8, 1, true);
    uint64_t bad = sorted_key(9);
    capture_start();
    for (int i = 0; i < 100; i++) {
        const uint64_t *one[] = { &bad };
        append_next(one, 1);
        set_data_count(0);
        fake_R_MergeDrawSurfLists(0x22, 4, game_data);
    }
    capture_end();
    CHECK(atomic_load(&drops) == 100 && atomic_load(&surfaces_left) == 100);
    CHECK(lines_in(printed) == 3 + 5); // 1, 2, 3, 4, 8, 16, 32, 64
    CHECK(strstr(printed, "(64 times, 64 surfaces so far)") != NULL);
    CHECK(strstr(printed, "(5 times") == NULL);
}

static void test_hook_debug(void) {
    set_game();
    debug = true;
    set_table(0, 0, false);
    uint64_t k = sorted_key(2);
    const uint64_t *one[] = { &k };
    capture_start();
    append_next(one, 1);
    fake_R_MergeDrawSurfLists(0x5, 1, game_data); // the first: the table as it is
    set_data_count(0);
    append_next(one, 1);
    fake_R_MergeDrawSurfLists(0x5, 1, game_data); // unchanged: no line about it
    set_table(5000, 0, true);                     // a sort ran
    set_data_count(0);
    append_next(one, 1);
    fake_R_MergeDrawSurfLists(0x5, 1, game_data);
    capture_end();
    CHECK(strstr(printed, "table holds 0 materials (was -1), a sort is not needed, no world") != NULL);
    CHECK(strstr(printed, "table holds 5000 materials (was 0), a sort is not needed, a world") != NULL);
    int table_lines = 0;
    for (const char *p = printed; (p = strstr(p, "sorted material table holds")) != NULL; p++)
        table_lines++;
    CHECK(table_lines == 2);
    CHECK(data_count() == 1); // the last one kept
}

static void test_hook_test_switch(void) {
    set_game();
    test_at = 2;
    set_table(0, 0, false);
    uint64_t k1 = sorted_key(1), k2 = sorted_key(2);
    const uint64_t *two[] = { &k1, &k2 };
    capture_start();
    append_next(two, 2);
    fake_R_MergeDrawSurfLists(0x28, 3, game_data); // an empty table: not counted (and both out)
    set_table(10, 0, true);
    set_data_count(0);
    append_next(two, 2);
    fake_R_MergeDrawSurfLists(0x28, 3, game_data); // the first counted: as it is
    CHECK(data_count() == 2 && k1 == sorted_key(1));
    set_data_count(0);
    append_next(two, 2);
    fake_R_MergeDrawSurfLists(0x28, 3, game_data); // the second: its first surface made to name slot 10, left out
    CHECK(data_count() == 1 && data_surfaces()[0] == &k2);
    CHECK(material_sort_guard_index(k1) == 10);
    CHECK((k1 & ~(0xffffULL << 27)) == (sorted_key(1) & ~(0xffffULL << 27)));
    k1 = sorted_key(1);
    set_data_count(0);
    append_next(two, 2);
    fake_R_MergeDrawSurfLists(0x28, 3, game_data); // once only
    capture_end();
    CHECK(data_count() == 2 && k1 == sorted_key(1));
    CHECK(strstr(printed, "test: the first of 2 surfaces had its key fffe00000ff80000 changed to fffe000057f80000 "
                          "(material index 10, right after the 10 filled slots)") != NULL);
    CHECK(strstr(printed, "1 of 2 surfaces of stages 40 to 42 left out") != NULL);
    CHECK(strstr(printed, "material index 10, at or past") != NULL);
}

static void test_init(void) {
    hooks = 0;
    rgp = NULL;
    unsetenv("BO3MACFIX_MATERIALSORTDEBUG");
    unsetenv("BO3MACFIX_MATERIALSORTTEST");
    material_sort_guard_init(Base, 0);
    CHECK(hooks == 0 && rgp == NULL);
    capture_start();
    material_sort_guard_init(Base, 1);
    capture_end();
    CHECK(hooks == 1);
    CHECK(hooked_at == (void *)(Base + ADDR_R_MergeDrawSurfLists));
    CHECK(hooked_with == (void *)fake_R_MergeDrawSurfLists);
    CHECK(rgp == (const uint8_t *)(Base + ADDR_rgp));
    CHECK(!debug && test_at == 0 && printed[0] == '\0');
    setenv("BO3MACFIX_MATERIALSORTDEBUG", "1", 1);
    setenv("BO3MACFIX_MATERIALSORTTEST", "7", 1);
    capture_start();
    material_sort_guard_init(Base, 1);
    capture_end();
    CHECK(debug && test_at == 7);
    CHECK(strstr(printed, "material sort guard: on") != NULL && strstr(printed, "(test:") != NULL);
    setenv("BO3MACFIX_MATERIALSORTDEBUG", "0", 1);
    setenv("BO3MACFIX_MATERIALSORTTEST", "-3", 1);
    material_sort_guard_init(Base, 1);
    CHECK(!debug && test_at == 0);
    unsetenv("BO3MACFIX_MATERIALSORTDEBUG");
    unsetenv("BO3MACFIX_MATERIALSORTTEST");
    // the places: the merge's start and the table, as offsets.h has them
    CHECK(ADDR_R_MergeDrawSurfLists == 0x100afa1);
    CHECK(ADDR_rgp == 0x1138a580);
}

int main(void) {
    test_index();
    test_check();
    test_filter();
    test_with_index();
    test_logged();
    test_hook_nothing_to_leave_out();
    test_hook_leaves_out();
    test_hook_after_r_init();
    test_hook_nothing_appended();
    test_hook_lines();
    test_hook_debug();
    test_hook_test_switch();
    test_init();
    if (failures == 0)
        printf("test_material_sort_guard: all passed\n");
    return failures != 0;
}
