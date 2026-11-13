// All-around Enhancement 4's free aim for players on other machines, as its Windows plugin does it. See
// aae_freeaim_net.c.
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AAE_FREEAIM_NET_PLAYERS 64
#define AAE_FREEAIM_NET_TEXT 64 // room for a command the client sends (the plugin's buffer is 63 bytes)

// ---- the client's side: what it sends

// What a local client sent last (the plugin keeps this per local client, 0 and 1).
typedef struct {
    uint64_t sent_at;  // when the last command went out, in ms; 0 for never
    int32_t sent[3];   // the numbers of the last "aae X Y Z"
    bool sent_target;  // the last command was "aae X Y Z" (not "aae off", not none)
} aae_freeaim_net_sender_t;

// One coordinate as it is sent: halved, rounded half away from zero (in floats, as the plugin does), cut to an int
// the way the x86 instruction cvttss2si does (INT32_MIN for what doesn't fit and for NaN).
int32_t aae_freeaim_net_half(float coordinate);

// One frame of a local client (the plugin's hook on CL_FinishMove, behind the game's own function). no_server: this
// machine runs no server (sv.state is 0). aiming: free aim is on for this client and its target could be read (then
// target holds it). have_counters: the client connection could be read, and sequence / acknowledged are its count of
// reliable commands sent and acknowledged by the server. True when a command is to go out: its text is in text and
// the sender's state has been updated.
bool aae_freeaim_net_send(aae_freeaim_net_sender_t *sender, uint64_t now, bool no_server, bool aiming,
                          const float target[3], bool have_counters, uint32_t sequence, uint32_t acknowledged,
                          char text[AAE_FREEAIM_NET_TEXT]);

// ---- the server's side: what it keeps of what clients sent

// One player's entry: a sequence counter that is odd while the entry is being written, then what was received.
typedef struct {
    _Atomic uint32_t sequence;
    _Atomic uintptr_t client;    // the client_t the command came from
    _Atomic uint64_t received_at; // when, in ms
    _Atomic uint32_t valid;
    _Atomic uint32_t target[3];   // the point, as the bits of three floats (twice the numbers sent)
} aae_freeaim_net_entry_t;

typedef struct {
    aae_freeaim_net_entry_t entries[AAE_FREEAIM_NET_PLAYERS];
    _Atomic uint64_t in_use; // a bit for every entry that is valid
    uint64_t swept_at;       // when entries of clients that went away were last looked for (client's thread only)
} aae_freeaim_net_table_t;

// A copy of an entry as read at one moment.
typedef struct {
    uintptr_t client;
    uint64_t received_at;
    uint32_t valid;
    float target[3];
} aae_freeaim_net_copy_t;

enum {
    AAE_FREEAIM_NET_NOT_OURS, // no "aae " in front, or no end within 128 characters: the game's
    AAE_FREEAIM_NET_BAD,      // ours, but nothing the plugin understands: dropped
    AAE_FREEAIM_NET_OFF,      // "aae off"
    AAE_FREEAIM_NET_TARGET,   // "aae X Y Z": target is twice those numbers
};
// What a client command is, by its text alone.
int aae_freeaim_net_read(const char *text, float target[3]);

// Which player a client_t is: -1 for one that isn't in the array that starts at clients, or is beyond the 64th.
int aae_freeaim_net_slot(const uint8_t *client, const uint8_t *clients, size_t client_size);

// The plugin's parser of a client's command on the server. Whether the command was "aae ..." and is not to reach
// the game. map: the server's map name (NULL for none); clients: svs.clients (NULL for none). Reads the client's
// state, an int at client_t + 0.
bool aae_freeaim_net_command(aae_freeaim_net_table_t *table, const char *text, const uint8_t *client,
                             int from_old_server, const char *map, const uint8_t *clients, size_t client_size,
                             uint64_t now);

// Reads an entry as the plugin does: up to four tries to get a copy that wasn't being written meanwhile.
bool aae_freeaim_net_copy(aae_freeaim_net_table_t *table, int slot, aae_freeaim_net_copy_t *copy);

// Every 500 ms, while any entry is valid: clears the entries whose client isn't there any more (not at its place in
// svs.clients, or not in state 5). The plugin does this on the client's thread, in its hook on CL_FinishMove.
void aae_freeaim_net_sweep(aae_freeaim_net_table_t *table, uint64_t now, const uint8_t *clients,
                           size_t client_size);

// Whether there is an entry for this player at all: valid, with a client (the plugin's bullet hook asks this first:
// with such an entry it turns the shot by the table or not at all; without one it looks at the local clients).
bool aae_freeaim_net_has_entry(aae_freeaim_net_table_t *table, uint32_t player);

// Where a player on another machine aims: the plugin's lookup (FUN_180021250).
bool aae_freeaim_net_lookup(aae_freeaim_net_table_t *table, uint32_t player, const char *map, bool active,
                            const uint8_t *clients, size_t client_size, float target[3]);

// ---- the game's side

// What the layer facing the game needs. Addresses of game variables and functions, and what aae_freeaim.c knows.
typedef struct {
    bool (*active)(void);                                     // the mod with its plugin is loaded
    uint64_t (*now_ms)(void);                                 // a clock in ms (the plugin's is GetTickCount64)
    const char *(*map)(void);                                 // the dvar mapname's text, or NULL
    bool (*on)(int local_client, uint64_t now);               // free aim is on for this local client (FUN_1800229f0)
    bool (*local_target)(int local_client, float target[3]); // its target from its dvar (FUN_180021a60, 0 and 1)
    void (*add_reliable_command)(int local_client, const char *text); // CL_AddReliableCommand
    const int32_t *server_state;                             // &sv.state
    uint8_t *const *client_connections;                      // &clc array pointer (0x25780 per local client)
    uint8_t *const *server_clients;                          // &svs.clients
    size_t client_size;                                      // sizeof(client_t)
} aae_freeaim_net_game_t;

void aae_freeaim_net_init(const aae_freeaim_net_game_t *game);

// From the hook on CL_FinishMove, after the game's own function has run.
void aae_freeaim_net_client_frame(int local_client);
// From the hook on SV_ExecuteClientCommand, before the game's own function: true means the command was ours and the
// game's function is not to be called.
bool aae_freeaim_net_server_command(const uint8_t *client, const char *text, int from_old_server);
// For the shot hooks: where the player with this client number aims, if they are on another machine and sent it.
bool aae_freeaim_net_remote_target(uint32_t player, float target[3]);
// For the shot hooks: whether the table has a valid entry for this player (see aae_freeaim_net_has_entry).
bool aae_freeaim_net_remote_entry(uint32_t player);

// for tests: the state kept
aae_freeaim_net_table_t *aae_freeaim_net_table(void);
aae_freeaim_net_sender_t *aae_freeaim_net_sender(int local_client);
