#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ClientFieldGuard: a script that asks for a client field nobody registered gets a script error instead of crashing
// the game, at the places T7Overcharged names for that fault (see clientfield_guard.c)
void clientfield_guard_init(uint64_t game_base_address, int enabled);

// what the guard does once the game's lookup found no field, by the place it was called from
typedef enum {
    Clientfield_Pass,         // the game's NULL goes back: the caller checks for it itself, or it isn't a known place
    Clientfield_Script_Error, // a script builtin that would read through it: the game's script error instead
    Clientfield_Zero,         // one of the game's value getters: a stand-in field that reads as 0
} clientfield_action_t;

// a place that calls the lookup, asking for an error if the field is missing, and reads through what it gets back
// without checking it
typedef struct {
    uint32_t return_offset;  // the instruction after the call, from the game's base: where the lookup returns to
    uint8_t kind;            // Clientfield_Script_Error or Clientfield_Zero
    uint8_t script_instance; // for a script error: 0 the server's scripts (GSC), 1 the client's (CSC)
    const char *function;    // the game's function, for the log
    const char *t7o_name;    // T7Overcharged's name for the place, NULL where it has none
} clientfield_site_t;

// the place the lookup returns to at return_offset, NULL for any other caller
const clientfield_site_t *clientfield_guard_site(uint64_t return_offset);
// the guard's rule: only for a missing field asked for with "error if missing", at a known place
clientfield_action_t clientfield_guard_action(const clientfield_site_t *site, bool found, bool error_if_missing);
// the name of client field set number set from the game's table of count names, "unknown" for any other number as
// the game's own message has it
const char *clientfield_guard_set_name(const char *const *names, int count, int set);
// the log's key for a missing field: the place (its index in the table), the set and the field's text, so that the
// same text under another script string id is the same field
uint64_t clientfield_guard_key(size_t place, int set, const char *field);
// whether a guarded lookup with this key gets a line in the log: the first time for each of up to count keys kept in
// seen (zeroed at first), then none; returns the times key was asked about with *times, 0 when there was no room for it
bool clientfield_guard_first_time(uint64_t *seen, uint32_t *counts, size_t count, uint64_t key, uint32_t *times);
// the names of the fields a set holds, as the lookup walks them (set: the game's 0x4018 bytes of it, count at +8,
// fields from +0x10, a field's name at +0x120), written to out as "<count> fields: a, b, ...", cut to its size; for
// the debug line that says what a set has instead of a field that was asked for
void clientfield_guard_set_fields(const uint8_t *set, const char *(*name_of)(uint32_t), char *out, size_t size);
