// All-around Enhancement 4's free aim: the plugin's free_aim component.
//
// The game has no crosshair in third person: its test for drawing one says no while the view is a third person one
// (unless the weapon wants one anyway). The mod has a third person view of its own, and its plugin gives it a
// crosshair and makes shots go to what is under it. This file is that, piece by piece.
//
// So far: the crosshair test says yes while free aim is on. Free aim is on, by the plugin's rule, for a local client
// whose crosshair code was at work within the last 250 ms (the plugin notes that at a place of its own right before
// the test; here the test's hook does it), whose view has been third person for 250 ms, who isn't a spectator, in a
// map whose name begins with "zm_", unless the dvar aae_freeaim_enabled is 0 (the plugin makes it with 1; without the
// plugin there is none until somebody sets it, and no dvar counts as 1). The second local client has
// aae_freeaim_enabled_1 as a switch of its own.
//
// The plugin wraps the one call of the test in the crosshair function. Here the test itself is hooked and only
// answers differently when that call is the one asking.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aae_freeaim.h"
#include "aae_plugin.h"
#include "dobby.h"
#include "gsc_obj.h"
#include "offsets.h"
#include "prepatch.h"

#define Seen_For_Ms 250        // how long a client counts as seen after the crosshair code was at work for it
#define Third_Person_For_Ms 250 // how long the view has to have been third person
#define Max_Client_Number 0x3f
#define Player_Kind_Spectator 4 // the player state's kind of movement
#define Local_Clients 2

void aae_freeaim_seen(aae_freeaim_client_t *client, const void *cg, uint64_t now) {
    if (client->cg != cg)
        client->third_person = 0;
    client->cg = cg;
    client->seen_at = now;
}

bool aae_freeaim_on(aae_freeaim_client_t *client, uint64_t now, uint32_t client_number, bool third_person,
                    int player_kind, const char *map, float enabled, float enabled_for_client) {
    bool seen = client->seen_at != 0 && now - client->seen_at <= Seen_For_Ms && client->cg != NULL &&
                client_number <= Max_Client_Number;
    bool is_third = seen && third_person;
    uint64_t state = is_third ? 2 : 1, kept = client->third_person;
    if ((kept & 3) != state) {
        client->third_person = now * 4 | state;
        return false;
    }
    if (!is_third || kept >> 2 > now || now - (kept >> 2) < Third_Person_For_Ms)
        return false;
    if (player_kind == Player_Kind_Spectator)
        return false;
    if (map == NULL || strncmp(map, "zm_", 3) != 0)
        return false;
    return enabled != 0.0f && enabled_for_client != 0.0f;
}

// ---- the game's side
#define Cg_Size 0x342a80
#define Cg_ThirdPerson 0x11a8c9  // a byte: the view is a third person one
#define Cg_PlayerState 0x11a8d0
#define Ps_Kind 0x4

static uint64_t base_address;
static int debug;
static void *(*Dvar_FindByHash)(uint32_t hash);
static float (*Dvar_GetFloat)(void *dvar);
static const char *(*Dvar_GetString)(void *dvar);
static uint8_t *const *cg_clients;
static const void *crosshair_asks_from; // where the crosshair code's own call of the test returns to
static aae_freeaim_client_t clients[Local_Clients];

static uint64_t uptime_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}
static uint64_t (*now_ms)(void) = uptime_ms;

// a dvar's value as a number, or the fallback when there is no such dvar
static float number_dvar(const char *name, float fallback) {
    void *dvar = Dvar_FindByHash(gsc_hash(name));
    return dvar != NULL ? Dvar_GetFloat(dvar) : fallback;
}

static bool on_for(int which, const uint8_t *cg) {
    uint32_t number;
    int kind;
    memcpy(&number, cg, sizeof(number));
    memcpy(&kind, cg + Cg_PlayerState + Ps_Kind, sizeof(kind));
    void *map = Dvar_FindByHash(gsc_hash("mapname"));
    return aae_freeaim_on(&clients[which], now_ms(), number, cg[Cg_ThirdPerson] != 0, kind,
                          map != NULL ? Dvar_GetString(map) : NULL, number_dvar("aae_freeaim_enabled", 1.0f),
                          number_dvar(which == 0 ? "aae_freeaim_enabled" : "aae_freeaim_enabled_1", 1.0f));
}

// the game's test for drawing the crosshair at all, (cg_t *, playerState_s *), which is hooked below
static bool (*orig_CG_DrawCrosshairTest)(uint8_t *cg, uint8_t *ps);

// What the test is to answer. from_the_crosshair: the crosshair function is the one asking, the only one that gets
// another answer than the game's.
static bool crosshair_test(uint8_t *cg, uint8_t *ps, bool from_the_crosshair) {
    bool draw = orig_CG_DrawCrosshairTest(cg, ps);
    if (!from_the_crosshair || cg == NULL || !aae_plugin_loaded_lately())
        return draw;
    const uint8_t *first = *cg_clients;
    // which local client's it is (counted like this, something from before them is far behind them)
    uintptr_t offset = (uintptr_t)cg - (uintptr_t)first;
    if (first == NULL || offset % Cg_Size != 0 || offset / Cg_Size >= Local_Clients)
        return draw;
    int which = (int)(offset / Cg_Size);
    aae_freeaim_seen(&clients[which], cg, now_ms());
    bool on = on_for(which, cg);
    if (debug) {
        static _Atomic int said = -1;
        if (atomic_exchange(&said, on) != (int)on)
            printf("free aim: %s for local client %d (the game's own answer for a crosshair: %s)\n", on ? "on" : "off",
                   which, draw ? "yes" : "no");
    }
    return draw || on;
}

static bool fake_CG_DrawCrosshairTest(uint8_t *cg, uint8_t *ps) {
    return crosshair_test(cg, ps, __builtin_return_address(0) == crosshair_asks_from);
}

void aae_freeaim_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    base_address = game_base_address;
    const char *env = getenv("BO3MACFIX_FREEAIMDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    aae_plugin_init(game_base_address);
    Dvar_FindByHash = (void *)(base_address + ADDR_Dvar_FindByHash);
    Dvar_GetFloat = (void *)(base_address + ADDR_Dvar_GetFloat);
    Dvar_GetString = (void *)(base_address + ADDR_Dvar_GetString);
    cg_clients = (uint8_t *const *)(base_address + ADDR_cgArray);
    crosshair_asks_from = (const void *)(base_address + ADDR_CG_DrawCrosshair_TestReturn);
    DobbyHook((void *)(base_address + ADDR_CG_DrawCrosshairTest), (void *)fake_CG_DrawCrosshairTest,
              (void **)&orig_CG_DrawCrosshairTest);
}
