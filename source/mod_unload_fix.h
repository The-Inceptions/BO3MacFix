#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ModUnloadFix: the lobby's Lua runs from copies of the Lua files it loads, so a mod unloaded in the game (the stock
// MODS menu's Unload, or another mod loaded over it) can't take their code away from under it (see mod_unload_fix.c)
void mod_unload_fix_init(uint64_t game_base_address, int enabled);

// one Lua file kept for a Lua VM: the bytes the game handed over (a zone's) and the copy the VM got instead
typedef struct {
    const void *vm;     // the VM's global state, which all its threads share
    const char *source; // where the game's reader had the bytes
    size_t size;
    char *copy;
} mod_unload_file_t;

// the copies kept, and the allocator they are made with (the unit test gives its own)
typedef struct {
    mod_unload_file_t *files;
    size_t count, capacity;
    size_t bytes; // all copies together
    void *(*allocate)(size_t size);
    void (*release)(void *memory);
} mod_unload_kept_t;

// The bytes a load into the VM is to read instead of source: the copy already kept for that VM of the same bytes
// from the same place (the game may hand one file over more than once), else a new one. NULL when there's no memory
// for it: the load then reads the game's own bytes, as without this. *made says whether this call made the copy.
const char *mod_unload_keep(mod_unload_kept_t *kept, const void *vm, const char *source, size_t size, bool *made);
// Frees every copy kept for the VM, once the game has closed it. Returns how many, and their bytes in *bytes.
size_t mod_unload_release(mod_unload_kept_t *kept, const void *vm, size_t *bytes);
// Whether a Lua state whose global state is vm belongs to the lobby's VM, whose global state is lobby_vm (NULL while
// the game has no lobby VM).
bool mod_unload_is_lobby(const void *vm, const void *lobby_vm);
