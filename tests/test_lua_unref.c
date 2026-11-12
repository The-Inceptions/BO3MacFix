// lua_unref.c: reference 0 never reaches the game's luaL_unref, every other one does, and the log doesn't fill up
// (the test includes the source itself).
#include <stdio.h>
#include "../source/lua_unref.c"

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

static int released, last_index, last_ref;
static void game_unref(lua_State *s, int index, int ref) {
    (void)s;
    released++;
    last_index = index;
    last_ref = ref;
}

int main(void) {
    orig_hksi_luaL_unref = game_unref;
    // the game's own releases: a reference, and the two values it uses for "none" (which luaL_unref ignores itself)
    fake_hksi_luaL_unref(NULL, -10000, 3);
    CHECK(released == 1 && last_index == -10000 && last_ref == 3);
    fake_hksi_luaL_unref(NULL, -10000, -2);
    fake_hksi_luaL_unref(NULL, 2, -1);
    CHECK(released == 3 && last_index == 2 && last_ref == -1 && skipped == 0);
    // reference 0, of any table
    fake_hksi_luaL_unref(NULL, -10000, 0);
    fake_hksi_luaL_unref(NULL, 1, 0);
    CHECK(released == 3 && skipped == 2);
    fake_hksi_luaL_unref(NULL, -10000, 1);
    CHECK(released == 4 && last_ref == 1);

    // the log: the first 16, then 32, 64, 128...
    int lines = 0;
    for (uint32_t n = 1; n <= 100000; n++)
        lines += lua_unref_logged(n);
    CHECK(lines == 16 + 12 && lua_unref_logged(16) && !lua_unref_logged(17) && lua_unref_logged(32) &&
          !lua_unref_logged(48) && lua_unref_logged(65536));
    if (failures != 0) {
        printf("test_lua_unref: %d failures\n", failures);
        return 1;
    }
    printf("test_lua_unref: all passed\n");
    return 0;
}
