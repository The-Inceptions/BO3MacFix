// All-around Enhancement 4's free aim in third person, as its Windows plugin does it (AAEFreeAim). See aae_freeaim.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void aae_freeaim_init(uint64_t game_base_address, int enabled);

// What the plugin keeps for a local client to say whether free aim is on.
typedef struct {
    const void *cg;       // the client's cg_t as last seen by the crosshair code
    uint64_t seen_at;     // and when, in ms; 0 for never
    uint64_t third_person; // since when the view is (2) or isn't (1) third person: the time * 4 + that; 0 for unknown
} aae_freeaim_client_t;

// the crosshair code is at work for this client
void aae_freeaim_seen(aae_freeaim_client_t *client, const void *cg, uint64_t now);
// Whether free aim is on: the client was seen within the last 250 ms, its view has been third person for 250 ms, the
// player isn't a spectator, the map is a zombies one and neither dvar switches it off.
bool aae_freeaim_on(aae_freeaim_client_t *client, uint64_t now, uint32_t client_number, bool third_person,
                    int player_kind, const char *map, float enabled, float enabled_for_client);
// The crosshair's gap while free aim is on: from the gap the weapon's spread gives to the weapon's least gap as the
// sights come up (sights: 0 down, 1 all the way up).
float aae_freeaim_gap(float gap, float least, float sights);

// ---- where the shots go

// The plugin's way of reading a number: white space, a sign, digits, a point and more digits, an exponent of at most
// 38 either way; worked out in doubles as it goes, and not beyond 16777216. On success *text is behind the number.
bool aae_freeaim_number(const char **text, float *value);
// The point in the world under the crosshair as the mod's script leaves it in a dvar: three such numbers with white
// space between them and nothing else, in fewer than 192 characters.
bool aae_freeaim_target(const char *text, float target[3]);
// A switch dvar's text as a number (fewer than 64 characters, nothing but white space behind the number), or
// otherwise.
float aae_freeaim_switch(const char *text, float otherwise);
// Turns a shot's three directions (forward, right, up, each of length 1 and at right angles to the others, give or
// take 0.05) by the shortest turn that makes forward point from the muzzle at the target. False when the directions
// aren't such ones, the muzzle is beyond 16777216 or the target is at the muzzle.
bool aae_freeaim_turn(const float muzzle[3], const float directions[9], const float target[3], float turned[9]);

