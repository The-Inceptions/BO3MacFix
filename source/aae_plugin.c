// See aae_plugin.h.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "aae_plugin.h"
#include "offsets.h"
#include "t7overcharged.h"

#define Plugin_Path "T7Overcharged/plugins/AAEUnified.dll" // in the mod's folder: T7Overcharged loads it on Windows
#define Mod_Name_Size 0x20 // the game's array for the loaded mod's name
#define Plugin_Look_Ms 1000

static const char *loaded_mod; // the game's: the folder name of the mod that's loaded, "" without one

static uint64_t plugin_uptime_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}
static uint64_t (*plugin_now_ms)(void) = plugin_uptime_ms;

void aae_plugin_init(uint64_t game_base_address) {
    loaded_mod = (const char *)(game_base_address + ADDR_Mods_LoadedName);
}

bool aae_plugin_loaded(void) {
    if (loaded_mod == NULL)
        return false;
    char path[4096 + sizeof(Plugin_Path) + 1], loaded[Mod_Name_Size + 1];
    const char *folder = t7o_mod_folder(), *name = strrchr(folder, '/');
    name = name != NULL ? name + 1 : folder;
    memcpy(loaded, loaded_mod, Mod_Name_Size);
    loaded[Mod_Name_Size] = '\0';
    // the folder T7Overcharged was last asked for from has to be the loaded mod's, not one from earlier on
    if (loaded[0] == '\0' || strcmp(name, loaded) != 0)
        return false;
    snprintf(path, sizeof(path), "%s/%s", folder, Plugin_Path);
    return access(path, F_OK) == 0;
}

bool aae_plugin_loaded_lately(void) {
    // when it was last looked up, 0 for never (a time is kept one higher than it is)
    static _Atomic uint64_t looked_at;
    static _Atomic bool answer;
    uint64_t now = plugin_now_ms() + 1, last = atomic_load_explicit(&looked_at, memory_order_relaxed);
    if (last != 0 && now - last < Plugin_Look_Ms)
        return atomic_load_explicit(&answer, memory_order_relaxed);
    bool loaded = aae_plugin_loaded();
    atomic_store_explicit(&answer, loaded, memory_order_relaxed);
    atomic_store_explicit(&looked_at, now, memory_order_relaxed);
    return loaded;
}
