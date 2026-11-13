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
// The same for something that flies (a rocket, a grenade, any projectile), which has a place and a velocity and no
// directions: they are made from the velocity, turned as above with the place as the muzzle, and out come the
// velocity to give it, as fast as before and at the target now, and its angles (pitch, yaw and roll, in degrees).
// False when it flies slower than 0.0001 or the turn can't be made.
bool aae_freeaim_projectile(const float origin[3], const float velocity[3], const float target[3],
                            float turned_velocity[3], float angles[3]);

// ---- the client's side of a shot: its tracer from the gun

#define AAE_FREEAIM_SHOTS 32

// A shot the client turned, kept for its tracer (the plugin's record): where the shot starts (the eye), where the gun's
// tag_flash was then, the point the shot was turned at, and when.
typedef struct {
    float start[3];
    float flash[3];
    float target[3]; // (the plugin keeps it twice; the second copy is never read)
    uint64_t made_at; // in ms; 0 for none
    bool used;        // a queued shot's tracer has been moved to this one's flash
} aae_freeaim_shot_t;

// What is kept for a local client: its last 32 turned shots, whether a queued shot may still find one of them, and
// where the gun's tag_flash was last found.
typedef struct {
    aae_freeaim_shot_t shots[AAE_FREEAIM_SHOTS];
    uint32_t count;    // how many were kept so far: the next one goes to count % 32
    bool pending;      // a queued shot may still find one
    float flash[3];    // where the gun's tag_flash was last found
    uint64_t flash_at; // and when, in ms; 0 for never
} aae_freeaim_tracers_t;

// Where the gun's flash was for a shot that is being turned: where it was just found (found; NULL when it couldn't
// be), else where it was last found if that was at most 250 ms ago. It has to be a point within 1e7 and at most 128
// from where the shot starts. One just found that passes becomes the one last found, at now. False for none.
bool aae_freeaim_flash(aae_freeaim_tracers_t *kept, const float start[3], const float *found, uint64_t now,
                       float flash[3]);
// Keeps a turned shot (where it starts, the flash, the point; each a point within 1e7), and notes that a queued shot
// may find it.
bool aae_freeaim_keep(aae_freeaim_tracers_t *kept, const float start[3], const float flash[3], const float target[3],
                      uint64_t now);
// The kept shot a queued one is: the newest one of the last 500 ms not used yet that fits it (its start at most 8
// from the queued shot's origin; its flash within 1e7 and at most 128 from the queued shot's muzzle and origin; its
// point within 1e7 and 1 to 131072 from its flash; the queued shot's muzzle at most 128 from its origin). It is marked
// used, its flash and point are given, and whether a queued shot may still find one is noted anew. False, with nothing
// changed, when the queued shot's origin, start or muzzle isn't a point within 1e7; false when no kept shot fits.
bool aae_freeaim_match(aae_freeaim_tracers_t *kept, const float origin[3], const float start[3],
                       const float muzzle[3], uint64_t now, float flash[3], float target[3]);
