// ModUnloadFix: the lobby's Lua runs from copies of the Lua files it loads, so a mod unloaded in the game can't take
// their code away from under it.
//
// What went wrong (wip/night/crash-unload.md has the addresses and how each was found): the game has two Lua VMs, the
// UI's and the lobby's. Both are made for HavokScript's in-place bytecode: a function loaded from a Lua file keeps no
// copy of its instructions, it runs them where the file's bytes are, in the memory of the zone the file came from.
// The game makes the UI's VM anew whenever zones go, but the lobby's is made once, at the start, and lives as long as
// the game; the Windows game does the same. That holds as long as the lobby's files come from zones that stay.
// All-around Enhancement ships its own lua/shared/luareadonlytables.lua, which the lobby's Lua requires too, and which
// replaces Engine.PrintInfo and five more functions with its own. When the mod is on the command line (StartWithMod,
// or +set fs_game) the lobby's VM is made after the mod's zone is loaded and takes the mod's file. Unloading the mod
// in the game (the stock MODS menu's Unload, or loading another mod) frees that zone, and the next Engine.PrintInfo
// the lobby makes, a tenth of a second after the stock menus are back, runs instructions that aren't there any more:
// a bus error. A start that loads the mod from the menus gives the lobby's VM the stock file, so it never had this.
//
// The fix: every Lua file the game loads reaches the parser through one reader, which hands over the file's bytes in
// one piece. When the VM that loads is the lobby's, the reader's answer is replaced by a copy of the same bytes, made
// here; the parser then refers to the copy in place exactly as it referred to the zone. Where the copy lies doesn't
// matter: the parser aligns the instructions by its count of the bytes read, which starts at 0 for each file. The
// copies are freed when the game closes the VM they were made for (nothing of a closed VM runs any more), so a lobby
// VM made anew (the lobby_reload command) gets copies of its own. The lobby's VM goes on exactly as it did while the
// mod was loaded, now from memory that stays. The cost is the lobby's files, at most 42 of them and 941 KB, once.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dobby.h"
#include "havok.h"
#include "mod_unload_fix.h"
#include "offsets.h"

#define LuaFile_Name 0x0  // a Lua file asset: its name, its length (an int) at +8, its bytes at +0x10
#define First_Capacity 64 // records for the lobby's files (it loads about 40)
#define Debug_Lines 1     // BO3MACFIX_MODUNLOADDEBUG=1: a line for each file kept and for each VM's copies freed
#define Debug_Where 2     // =2: the same lines, but the lobby reads the game's own bytes (for a run asking where)

static lua_State *const *lobby_state; // the game's lobby VM, NULL while it has none
static int debug;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static mod_unload_kept_t kept = { .allocate = malloc, .release = free };
static bool said_kept, said_no_memory;

bool mod_unload_is_lobby(const void *vm, const void *lobby_vm) {
    return lobby_vm != NULL && vm == lobby_vm;
}

const char *mod_unload_keep(mod_unload_kept_t *kept, const void *vm, const char *source, size_t size, bool *made) {
    *made = false;
    for (size_t i = 0; i < kept->count; i++) {
        const mod_unload_file_t *file = &kept->files[i];
        if (file->vm == vm && file->source == source && file->size == size && memcmp(file->copy, source, size) == 0)
            return file->copy;
    }
    if (kept->count == kept->capacity) {
        size_t capacity = kept->capacity != 0 ? kept->capacity * 2 : First_Capacity;
        mod_unload_file_t *files = kept->allocate(capacity * sizeof(*files));
        if (files == NULL)
            return NULL;
        if (kept->files != NULL) {
            memcpy(files, kept->files, kept->count * sizeof(*files));
            kept->release(kept->files);
        }
        kept->files = files;
        kept->capacity = capacity;
    }
    char *copy = kept->allocate(size);
    if (copy == NULL)
        return NULL;
    memcpy(copy, source, size);
    kept->files[kept->count++] = (mod_unload_file_t){ .vm = vm, .source = source, .size = size, .copy = copy };
    kept->bytes += size;
    *made = true;
    return copy;
}

size_t mod_unload_release(mod_unload_kept_t *kept, const void *vm, size_t *bytes) {
    size_t freed = 0, left = 0;
    *bytes = 0;
    for (size_t i = 0; i < kept->count; i++) {
        mod_unload_file_t file = kept->files[i];
        if (file.vm != vm) {
            kept->files[left++] = file;
            continue;
        }
        *bytes += file.size;
        kept->release(file.copy);
        freed++;
    }
    kept->count = left;
    kept->bytes -= *bytes;
    return freed;
}

static const char *file_name(const void *file) {
    const char *name = *(const char *const *)((const uint8_t *)file + LuaFile_Name);
    return name != NULL ? name : "?";
}

// the reader the game's Lua file loader (hksL_loadfile) gives lua_load: (L, the file's asset, &size), the asset's
// bytes in one piece, each time it is asked
install_hook_name(LuaFile_Reader, const char *, lua_State *s, const void *file, size_t *size) {
    const char *bytes = orig_LuaFile_Reader(s, file, size);
    if (bytes == NULL || *size == 0 || s == NULL)
        return bytes;
    lua_State *lobby = *lobby_state;
    const void *vm = HKS_GLOBAL(s);
    if (!mod_unload_is_lobby(vm, lobby != NULL ? HKS_GLOBAL(lobby) : NULL))
        return bytes;
    if (debug == Debug_Where) {
        printf("lobby lua: %s (%zu bytes) at %p to %p, read where the game has it (BO3MACFIX_MODUNLOADDEBUG=2)\n",
               file_name(file), *size, (const void *)bytes, (const void *)(bytes + *size));
        return bytes;
    }
    bool made;
    pthread_mutex_lock(&lock);
    const char *copy = mod_unload_keep(&kept, vm, bytes, *size, &made);
    size_t count = kept.count, total = kept.bytes;
    pthread_mutex_unlock(&lock);
    if (copy == NULL) {
        if (!said_no_memory) {
            said_no_memory = true;
            printf("lobby lua: no memory for a copy of a file of the lobby's Lua (%zu bytes): it runs from the "
                   "game's own, as without ModUnloadFix\n", *size);
        }
        return bytes;
    }
    if (made && debug == Debug_Lines)
        printf("lobby lua: kept %s (%zu bytes) from %p at %p (%zu files, %zu KB)\n", file_name(file), *size,
               (const void *)bytes, (const void *)copy, count, total >> 10);
    if (made && !said_kept) {
        said_kept = true;
        printf("lobby lua: the lobby's Lua runs from copies of the files it loads, so unloading a mod in the game "
               "can't take them away (ModUnloadFix)\n");
    }
    return copy;
}

// lua_close: closes a VM and frees all of it; the game calls it only from its two VM closers (one VM, or all)
install_hook_name(hksi_lua_close, void, lua_State *s) {
    const void *vm = s != NULL ? HKS_GLOBAL(s) : NULL;
    orig_hksi_lua_close(s);
    if (vm == NULL)
        return;
    size_t bytes;
    pthread_mutex_lock(&lock);
    size_t freed = mod_unload_release(&kept, vm, &bytes);
    pthread_mutex_unlock(&lock);
    if (freed != 0)
        printf("lobby lua: the game closed the lobby's Lua, the copies of its %zu files (%zu KB) are freed\n", freed,
               bytes >> 10);
}

void mod_unload_fix_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    const char *env = getenv("BO3MACFIX_MODUNLOADDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    lobby_state = (lua_State *const *)(game_base_address + ADDR_LobbyVM_State);
    install_hook_LuaFile_Reader((void *)(game_base_address + ADDR_LuaFile_Reader));
    install_hook_hksi_lua_close((void *)(game_base_address + ADDR_hksi_lua_close));
}
