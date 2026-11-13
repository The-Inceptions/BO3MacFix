// MoreClientEffects: client effects get a table of 2048 instead of the game's 1024 once a mod has started
// T7Overcharged, as T7Overcharged gives them on Windows ("Extend BGCache client_fx limit"). See aae_clientfx.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void aae_clientfx_init(uint64_t game_base_address, int enabled);
// A mod has started T7Overcharged (the stand-in's init, t7overcharged.c). The first time, T7Overcharged's dvar
// hook_extend_clientfx decides whether client effects have the bigger table from then on, as T7Overcharged decides
// it when it starts; later calls change nothing.
void aae_clientfx_t7o_started(void);

// ---- the rules, without the game

// An entry of the game's table of client effects in use, 16 bytes, as the game's cache keeps one for every kind of
// asset it names by a number.
typedef struct {
    const void *asset;   // the effect; its first field is its name
    uint32_t hash;       // of the name in lower case, 0 for a free entry
    uint8_t references;  // how many registrations hold it
    uint8_t flags[3];
} aae_clientfx_entry_t;

#define Aae_ClientFx_Game_Count 1024 // entries in the game's table, the first never used
#define Aae_ClientFx_Count 2048      // in T7Overcharged's

// How hook_extend_clientfx is when T7Overcharged starts, from what the mod set with Engine.SetDvar: a dvar of the
// game's kind 1 (a bool, its byte), 6 (an int) or 8 (text), another kind, or none.
typedef struct {
    bool there;
    int kind;
    int32_t number; // a bool's or an int's value
    const char *text; // a text's
} aae_clientfx_dvar_t;
// Whether the dvar says yes. T7Overcharged makes it a bool with 1 and asks whether it is 1, so: yes without it, a
// bool or int that isn't 0, a text whose number (as the game reads one into a bool: its leading digits) isn't 0,
// and yes for any other kind, which the bool would be made from as from none.
bool aae_clientfx_switch(const aae_clientfx_dvar_t *dvar);

// Which table a call means by its first argument, T7Overcharged's rule: 0 the server's, anything else the client's.
int aae_clientfx_side(uint32_t side);

// The name T7Overcharged gives for an entry: "" for an index outside 0..2047 and for a free entry, else the effect's.
const char *aae_clientfx_name(const aae_clientfx_entry_t *table, int32_t index);

// What T7Overcharged does as it takes over: the game's 1024 entries as they are, the rest free.
void aae_clientfx_take_over(aae_clientfx_entry_t *ours, const aae_clientfx_entry_t *game);

// What the reset of the cache does to a table of 2048: every entry free.
void aae_clientfx_clear(aae_clientfx_entry_t *table);

// How many entries of a table are in use, for the log.
int aae_clientfx_in_use(const aae_clientfx_entry_t *table, int count);
