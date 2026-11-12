// LuaUnrefFix: releasing Lua reference 0 does nothing, instead of writing outside the table.
//
// luaL_ref keeps a value in a table (the registry, for the game) and returns a number for it, luaL_unref gives that
// number back. HavokScript's luaL_unref writes the table's array part directly, at slot ref - 1 as an unsigned 32-bit
// number, after making sure the array is at least ref long. For ref 0 that is slot 4,294,967,295: 16 bytes written
// 64 GB behind the array, which crashes the game or damages whatever is there. Reference 0 is never handed out (the
// first one of a table is 3), so nothing is lost by not releasing it.
//
// The game does release it: when it frees a UI element it releases the reference its animation state at +0x138 holds,
// and that can be 0. All-around Enhancement 4's Windows plugin (its lui_ref_guard component) replaces that one call
// with a check and counts how often it skips one ("ZERO_REF_SUPPRESSED"). Here luaL_unref itself is hooked, which
// covers that call and every other.
#include <stdio.h>
#include "dobby.h"
#include "havok.h"
#include "lua_unref.h"
#include "offsets.h"

static uint32_t skipped;

bool lua_unref_logged(uint32_t n) {
    return n <= 16 || (n & (n - 1)) == 0;
}

// hksi_luaL_unref(L, index of the table, ref)
install_hook_name(hksi_luaL_unref, void, lua_State *s, int index, int ref) {
    if (ref != 0) {
        orig_hksi_luaL_unref(s, index, ref);
        return;
    }
    uint32_t n = __atomic_add_fetch(&skipped, 1, __ATOMIC_RELAXED);
    if (lua_unref_logged(n))
        printf("lua: the game released reference 0 of table %d, which would have written outside the table (skipped, "
               "%u so far)\n", index, n);
}

void lua_unref_init(uint64_t game_base_address, int enabled) {
    if (enabled)
        install_hook_hksi_luaL_unref((void *)(game_base_address + ADDR_hksi_luaL_unref));
}
