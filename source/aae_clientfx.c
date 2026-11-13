// MoreClientEffects: T7Overcharged's bigger table of client effects (its "bgcache" component).
//
// The game keeps, for every kind of asset a map refers to by a number (models, effects, strings, menus ...), a table
// of the ones in use: its "BG cache", one copy for the server and one for the client, each entry an asset, the hash of
// its name, a count of the registrations that hold it and three bytes of flags. Client effects ("client_fx", the
// effects the client's scripts play) have 1024 entries, and the first is never used. When a map with its effects and
// a mod with its own need more, the map stops loading with
//     BG_Cache_GetIndexInternal - Exceeded '1024' items for type 'client_fx'
// With All-around Enhancement on, two of the workshop maps (zm_cum_on_my_face and zm_mcdonaldsfinale) do.
//
// What T7Overcharged does: the mod sets its dvar hook_extend_clientfx to 1 before it loads T7Overcharged (which makes
// the dvar with 1 too, "Extend BGCache client_fx limit", and reads it once, as it starts). When it is 1,
// T7Overcharged copies the server's and the client's 1024 entries into two tables of 2048 of its own and replaces
// the game's five functions for client effects and the cache's reset. Each does what the game's does, with its table
// and 2048 instead of the game's and 1024; the game's own generic functions still do the work (find an entry, add
// one, let go of one). The game's tables stay as they are, unused. That stays so until the game ends.
//
// This does the same, from the first time a mod starts T7Overcharged (aae_clientfx_t7o_started, from the stand-in's
// init in t7overcharged.c), while the setting MoreClientEffects is on. The six functions are hooked from the start;
// until then, and for good when the dvar says no, they hand every call to the game's own.
//
// The places on the Mac (wip/night/impl-clientfx.md has how each one was matched with Windows and how the list was
// made complete): the client effects' table is at +0x43c00 of each copy of the cache, and only the five functions
// and the reset use it, besides two printers for debugging (bgCachePrintValues and the server/client diff), which go
// on printing the game's table, now empty, as on Windows with T7Overcharged. An index of a client effect only comes
// from the lookup (the registration hands none back), and its callers use it at once or keep it in a client script's
// variable; none is sent over the network, and the checksum of its cache the server publishes (config string 0xca9)
// leaves client effects out. So a Mac and a Windows player in one game don't have to agree on this table.
//
// Where this differs from T7Overcharged:
// - T7Overcharged's registration passes no third argument on (its replacement takes two), so the game's generic add
//   gets whatever byte is on T7Overcharged's stack for "add a reference to an entry that is there already". Here
//   the caller's is passed on, as the game's own function does.
// - Two client script functions keep refusing effects past the game's 1023: PlayFXOnDynEnt and SpawnFX test the
//   index themselves ("invalid effect"). T7Overcharged leaves those tests, and so does this.
// - T7Overcharged's component also turns the cache's "Exceeded" error into a line in its console for every kind of
//   asset, guards four other tables' getters, and can make the server's effects 512 (hook_extend_serverfx, which
//   the mod sets to 0). Those aren't part of this.
// - On the Windows build this was matched with (the third of the three builds T7Overcharged knows), T7Overcharged's
//   table of addresses has none for five of the six functions, and it takes them from that table as it is loaded,
//   before its search by pattern runs. So there its first hook fails and client effects stay 1024 (with the error
//   made a console line). The bigger table is what it does on the build it has these functions for.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aae_clientfx.h"
#include "dobby.h"
#include "gsc_obj.h"
#include "offsets.h"

#define Cache_Side_Size 0x45fe00 // each of the cache's two copies: the server's, then the client's
#define Cache_ClientFx 0x43c00   // where in a copy the client effects' table is
#define Type_ClientFx 0x1b       // the cache's number for client effects (their row in its table of kinds)
#define Asset_Fx 0x26            // the asset type of an effect
#define Print_Cache_Error 8      // what the game's registration prints "Unable to register client fx" with
#define Print_Cache_Channel 0x17
#define Dvar_Kind 0x1c
#define Dvar_Value 0x28
#define Dvar_Kind_Bool 1
#define Dvar_Kind_Int 6
#define Dvar_Kind_Text 8

_Static_assert(sizeof(aae_clientfx_entry_t) == 16, "an entry of the game's cache");

// ---- the rules

bool aae_clientfx_switch(const aae_clientfx_dvar_t *dvar) {
    if (!dvar->there)
        return true;
    if (dvar->kind == Dvar_Kind_Bool || dvar->kind == Dvar_Kind_Int)
        return dvar->number != 0;
    if (dvar->kind == Dvar_Kind_Text)
        return dvar->text != NULL && strtol(dvar->text, NULL, 10) != 0;
    return true;
}

int aae_clientfx_side(uint32_t side) {
    return side != 0;
}

const char *aae_clientfx_name(const aae_clientfx_entry_t *table, int32_t index) {
    if ((uint32_t)index > Aae_ClientFx_Count - 1 || table[index].asset == NULL)
        return "";
    return *(const char *const *)table[index].asset;
}

void aae_clientfx_take_over(aae_clientfx_entry_t *ours, const aae_clientfx_entry_t *game) {
    memcpy(ours, game, Aae_ClientFx_Game_Count * sizeof(*ours));
    memset(ours + Aae_ClientFx_Game_Count, 0, (Aae_ClientFx_Count - Aae_ClientFx_Game_Count) * sizeof(*ours));
}

void aae_clientfx_clear(aae_clientfx_entry_t *table) {
    memset(table, 0, Aae_ClientFx_Count * sizeof(*table));
}

int aae_clientfx_in_use(const aae_clientfx_entry_t *table, int count) {
    int used = 0;
    for (int i = 0; i < count; i++)
        used += table[i].hash != 0;
    return used;
}

// ---- in the game

static uint64_t base_address;
static int debug;
static int (*say)(const char *format, ...) = printf; // the log
static aae_clientfx_entry_t tables[2][Aae_ClientFx_Count];
static atomic_bool active;  // client effects use the tables above
static atomic_bool decided; // the first start of T7Overcharged has been
// the highest index the registration handed out on each table since its reset, and whether the log has been told
// that it went past the game's table
static atomic_int highest[2];
static atomic_bool past_the_game[2];

// the game's generic functions, which T7Overcharged's replacements call too:
// (side, kind, table, entries in it, name, give a free entry when it isn't there) -> its index, 0 for none; when the
// table is full it prints the kind's entries and stops the map with "Exceeded"
static int (*BG_Cache_GetIndexInternal)(uint32_t side, uint32_t kind, aae_clientfx_entry_t *table, int32_t count,
                                        const char *name, bool free_entry);
// (side, kind, table, entries in it, name, asset, add a reference to an entry that is there already, only the
// lowest byte counts) -> its index, 0 for none
static int (*BG_Cache_RegisterDef)(uint32_t side, uint32_t kind, aae_clientfx_entry_t *table, int32_t count,
                                   const char *name, const void *asset, uint32_t reference);
// (side, kind, table, entries in it, name): one reference less, and the entry free when none is left
static uint64_t (*BG_Cache_Release)(uint32_t side, uint32_t kind, aae_clientfx_entry_t *table, int32_t count,
                                    const char *name);
// (asset type, name, where the asset goes) -> found; prints "Couldn't find asset '%s'" when it isn't
static bool (*BG_Cache_FindAsset)(int32_t type, const char *name, const void **asset);
static void (*Com_Printf)(int32_t kind, int32_t channel, const char *format, ...);
static void *(*Dvar_FindByHash)(uint32_t hash);
static const char *(*Dvar_GetString)(void *dvar);

static bool on(void) {
    return atomic_load_explicit(&active, memory_order_acquire);
}
static aae_clientfx_entry_t *game_table(uint32_t side) {
    return (aae_clientfx_entry_t *)(base_address + ADDR_bgCache + (uint64_t)side * Cache_Side_Size + Cache_ClientFx);
}
static aae_clientfx_entry_t *our_table(uint32_t side) {
    return tables[aae_clientfx_side(side)];
}
static const char *side_name(uint32_t side) {
    return aae_clientfx_side(side) == 0 ? "server's" : "client's";
}

// a registration handed out this index: noted for the log, which hears once a reset when a table goes past the
// game's 1023 (the only time this changes anything a player sees), and with the debug switch each time a table's
// highest index passes another 256
static void note_index(uint32_t side, int index, const char *name) {
    int which = aae_clientfx_side(side), seen = atomic_load_explicit(&highest[which], memory_order_relaxed);
    while (index > seen && !atomic_compare_exchange_weak(&highest[which], &seen, index)) {
    }
    if (debug && index / 256 > seen / 256)
        say("client effects: the %s table has reached entry %d (%s)\n", side_name(side), index, name);
    if (index >= Aae_ClientFx_Game_Count && !atomic_exchange(&past_the_game[which], true))
        say("client effects: the %s table holds more than the game's %d (entry %d is %s)\n", side_name(side),
            Aae_ClientFx_Game_Count - 1, index, name);
}

// T7Overcharged's function 1: (side, name) the registration of an effect let go of
install_hook_name(BG_Cache_ReleaseClientFx, uint64_t, uint32_t side, const char *name) {
    if (!on())
        return orig_BG_Cache_ReleaseClientFx(side, name);
    return BG_Cache_Release(side, Type_ClientFx, our_table(side), Aae_ClientFx_Count, name);
}

// function 2: (side, index) the effect of an entry. No test of the index, like the game's and T7Overcharged's: its
// callers have it from a lookup.
install_hook_name(BG_Cache_GetClientFx, const void *, uint32_t side, int32_t index) {
    if (!on())
        return orig_BG_Cache_GetClientFx(side, index);
    return our_table(side)[index].asset;
}

// function 3: (side, name) the index of an effect, 0 when it isn't there (nothing is added)
install_hook_name(BG_Cache_GetClientFxIndex, int, uint32_t side, const char *name) {
    if (!on())
        return orig_BG_Cache_GetClientFxIndex(side, name);
    return BG_Cache_GetIndexInternal(side, Type_ClientFx, our_table(side), Aae_ClientFx_Count, name, false);
}

// function 4: (side, index) the name of an entry's effect
install_hook_name(BG_Cache_GetClientFxName, const char *, uint32_t side, int32_t index) {
    if (!on())
        return orig_BG_Cache_GetClientFxName(side, index);
    return aae_clientfx_name(our_table(side), index);
}

// function 5: (side, name, add a reference to an entry that is there already) an effect registered: the asset is
// looked up, then added. The kind of map loading that fills the table calls this with 1.
install_hook_name(BG_Cache_RegisterClientFx, void, uint32_t side, const char *name, uint32_t reference) {
    if (!on()) {
        orig_BG_Cache_RegisterClientFx(side, name, reference);
        return;
    }
    const void *asset;
    if (!BG_Cache_FindAsset(Asset_Fx, name, &asset))
        return;
    int index = BG_Cache_RegisterDef(side, Type_ClientFx, our_table(side), Aae_ClientFx_Count, name, asset,
                                     reference & 0xff);
    if (index <= 0)
        Com_Printf(Print_Cache_Error, Print_Cache_Channel, "Unable to register client fx %s\n", name);
    else
        note_index(side, index, name);
}

// the reset of one side's cache, as a map starts: (side). T7Overcharged's table of that side is cleared first.
install_hook_name(BG_Cache_Init, void, uint32_t side) {
    if (on()) {
        int which = aae_clientfx_side(side);
        if (debug)
            say("client effects: the %s table is reset: %d entries were in use, the highest index since the last reset "
                "was %d\n", side_name(side), aae_clientfx_in_use(tables[which], Aae_ClientFx_Count),
                atomic_load(&highest[which]));
        aae_clientfx_clear(tables[which]);
        atomic_store(&highest[which], 0);
        atomic_store(&past_the_game[which], false);
    }
    orig_BG_Cache_Init(side);
}

// hook_extend_clientfx as the mod left it, and its description for the log
static aae_clientfx_dvar_t read_switch(char *said, size_t size) {
    aae_clientfx_dvar_t dvar = { 0 };
    const uint8_t *found = Dvar_FindByHash(gsc_hash("hook_extend_clientfx"));
    if (found == NULL) {
        snprintf(said, size, "not set");
        return dvar;
    }
    dvar.there = true;
    memcpy(&dvar.kind, found + Dvar_Kind, sizeof(dvar.kind));
    if (dvar.kind == Dvar_Kind_Bool)
        dvar.number = found[Dvar_Value];
    else if (dvar.kind == Dvar_Kind_Int)
        memcpy(&dvar.number, found + Dvar_Value, sizeof(dvar.number));
    else if (dvar.kind == Dvar_Kind_Text)
        dvar.text = Dvar_GetString((void *)found);
    if (dvar.kind == Dvar_Kind_Text)
        snprintf(said, size, "\"%s\"", dvar.text != NULL ? dvar.text : "");
    else
        snprintf(said, size, "%d (a dvar of kind %d)", dvar.number, dvar.kind);
    return dvar;
}

void aae_clientfx_t7o_started(void) {
    if (base_address == 0 || atomic_exchange(&decided, true))
        return;
    char said[96];
    aae_clientfx_dvar_t dvar = read_switch(said, sizeof(said));
    if (!aae_clientfx_switch(&dvar)) {
        say("client effects: T7Overcharged's hook_extend_clientfx is %s, so the game's table of %d stays "
            "(MoreClientEffects)\n", said, Aae_ClientFx_Game_Count);
        return;
    }
    // what is in the game's tables now goes along, as T7Overcharged copies it
    for (uint32_t side = 0; side < 2; side++)
        aae_clientfx_take_over(tables[side], game_table(side));
    atomic_store_explicit(&active, true, memory_order_release);
    say("client effects: T7Overcharged's hook_extend_clientfx is %s, so client effects have a table of %d from now on "
        "(MoreClientEffects)\n", said, Aae_ClientFx_Count);
    if (debug)
        say("client effects: taken over from the game: %d of the server's entries, %d of the client's\n",
            aae_clientfx_in_use(tables[0], Aae_ClientFx_Count), aae_clientfx_in_use(tables[1], Aae_ClientFx_Count));
}

void aae_clientfx_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    const char *env = getenv("BO3MACFIX_CLIENTFXDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    BG_Cache_GetIndexInternal = (void *)(base_address + ADDR_BG_Cache_GetIndexInternal);
    BG_Cache_RegisterDef = (void *)(base_address + ADDR_BG_Cache_RegisterDef);
    BG_Cache_Release = (void *)(base_address + ADDR_BG_Cache_Release);
    BG_Cache_FindAsset = (void *)(base_address + ADDR_BG_Cache_FindAsset);
    Com_Printf = (void *)(base_address + ADDR_Com_Printf);
    Dvar_FindByHash = (void *)(base_address + ADDR_Dvar_FindByHash);
    Dvar_GetString = (void *)(base_address + ADDR_Dvar_GetString);
    install_hook_BG_Cache_ReleaseClientFx((void *)(base_address + ADDR_BG_Cache_ReleaseClientFx));
    install_hook_BG_Cache_GetClientFx((void *)(base_address + ADDR_BG_Cache_GetClientFx));
    install_hook_BG_Cache_GetClientFxIndex((void *)(base_address + ADDR_BG_Cache_GetClientFxIndex));
    install_hook_BG_Cache_GetClientFxName((void *)(base_address + ADDR_BG_Cache_GetClientFxName));
    install_hook_BG_Cache_RegisterClientFx((void *)(base_address + ADDR_BG_Cache_RegisterClientFx));
    install_hook_BG_Cache_Init((void *)(base_address + ADDR_BG_Cache_Init));
}
