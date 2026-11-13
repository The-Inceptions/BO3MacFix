// All-around Enhancement 4's pause for everybody in a co-op game, as its Windows plugin does it (coop_pause). See
// aae_coop_pause.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void aae_coop_pause_init(uint64_t game_base_address, int enabled);

#define AAE_Coop_Slots 18        // the server's clients the plugin keeps a place for
#define AAE_Coop_Packet_Size 0x30

// A netadr_t as the game keeps it: the IP (4 bytes), the port (2), 2 unused, the kind (4: 3 and 4 are other machines),
// the local net id (4: which local client of that machine, or the server).
typedef struct {
    uint8_t bytes[16];
} aae_coop_adr_t;

// One of the server's clients (client_t), what the plugin reads of it.
typedef struct {
    uint32_t state;      // 5 is a client in the game
    aae_coop_adr_t adr;  // where it is
    uint64_t id;         // its 8-byte id, 0 for none
    int32_t bot;         // not 0 for a bot
} aae_coop_client_t;

// A dvar's value as the plugin reads it.
enum { AAE_Coop_None, AAE_Coop_Number, AAE_Coop_Text, AAE_Coop_Other };
typedef struct {
    int kind;            // AAE_Coop_Number for a bool, an int or an enum
    int64_t number;
    const char *text;    // for AAE_Coop_Text
} aae_coop_value_t;

// the dvars it reads
enum { AAE_Coop_Map, AAE_Coop_Sv_Running, AAE_Coop_Cl_Paused, AAE_Coop_Sv_Paused, AAE_Coop_Max_Clients,
       AAE_Coop_Pause_Remote };

// What the plugin asks the game and does to it, as calls, so that the rules below run against a made-up game in the
// tests. Each is called where the plugin asks.
typedef struct aae_coop_game aae_coop_game_t;
struct aae_coop_game {
    void *context;
    uint64_t (*now)(const aae_coop_game_t *game); // milliseconds, never 0 (the plugin's GetTickCount64)
    void (*value)(const aae_coop_game_t *game, int dvar, aae_coop_value_t *value);
    void (*set_dvar)(const aae_coop_game_t *game, const char *name, const char *value); // made if it isn't there
    bool (*client)(const aae_coop_game_t *game, int index, aae_coop_client_t *client); // false: no table
    int (*connection)(const aae_coop_game_t *game); // local client 0's connection state, -1 outside 0..11
    bool (*server)(const aae_coop_game_t *game, aae_coop_adr_t *adr, uint64_t *id); // false: no connection data
    int (*local_sock)(const aae_coop_game_t *game); // the socket the game sends local client 0's packets with
    uint64_t (*nonce)(const aae_coop_game_t *game); // 64 random bits, 0 if there are none
    void (*lock)(const aae_coop_game_t *game);      // what the network thread shares
    void (*unlock)(const aae_coop_game_t *game);
};

// One server client's place on the host.
typedef struct {
    aae_coop_adr_t adr;
    uint64_t id;
    uint64_t nonce;  // the client's own, from its hello; 0 until one came
    uint64_t acked;  // the host's sequence number it acknowledged last
    uint64_t heard;  // when its last good hello or acknowledgement came, 0 for never
    uint64_t sent;   // when the host's state went to it last, 0 for not since a change
    uint32_t sends;  // how often (for the log only)
    bool ok;         // an active client of another machine with an id: it takes part
} aae_coop_slot_t;

// Everything the plugin keeps, with its initial values from aae_coop_reset.
typedef struct {
    // shared with the network thread, under game->lock
    int role;                // -1 none, 0 a client of another machine's game, 1 the host (the server runs here)
    int sock;                // the socket packets go with: 4 for the host, -1 unknown
    uint64_t frame_at;       // when local client 0's frame last ran
    uint64_t host_nonce;     // the host's, drawn when the machine becomes host
    uint64_t sequence;       // the host's state number: starts at 1, one up on every change
    uint64_t confirmed;      // the sequence number every client acknowledged last
    uint64_t changed_at;     // when the state or the set of clients last changed
    uint32_t state;          // 1 paused
    uint32_t mask;           // the slots that take part, one bit each
    bool timed_out;          // a client didn't acknowledge in time: no pause until it is asked for anew
    aae_coop_slot_t slots[AAE_Coop_Slots];
    aae_coop_adr_t server;   // the client's: where the host is
    uint64_t server_id;
    bool server_valid;
    uint64_t own_nonce;      // the client's, drawn when it becomes a client or the server changes
    uint64_t known_nonce;    // the host's as last heard, 0 for none
    uint64_t known_sequence; // the newest state heard
    uint32_t known_state;
    uint64_t news;           // how many new states came
    uint64_t acked_news;     // up to which of them an acknowledgement is due
    uint64_t heard;          // when the host was last heard
    uint64_t hello_at;       // when the last hello and acknowledgement went, 0 for "now"
    uint64_t ack_at;
    bool applied;            // the host's pause this client put into cl_paused

    // the game's own threads that run the frames
    bool server_paused;      // the host's server is held
    bool host_view_frozen;   // the host's own view is held while the clients are asked
    bool sv_in_map;          // in a map at the last server frame
    bool busy;               // the host's rules or the server's pause test are running
    bool host_timed_out;     // the host's rules' last answers: timed out,
    bool waiting;            //   asked for with somebody remote and not timed out,
    bool all_confirmed;      //   everybody acknowledged the newest state,
    uint64_t paused_at;      //   held since then (0: not held)
    bool remote_known;       // the client's: a state from the host was taken,
    int remote_state;        //   which one,
    uint64_t remote_at;      //   and when (0 for a 0 from a server command)
    bool paused_by_host;     // the client's answer in its last frame
    bool client_in_map;      // a client in a map at its last frame
    uint64_t client_frame_at;
    int coop_paused_written; // what aae_coop_paused was last set to, -1 for nothing yet
    int last_cl_paused;      // what the server's pause test saw last
    uint64_t counted_at;     // the counts of the server's clients, kept for 16 ms:
    int total, remote, remote_active;
    bool local_active, local_human;
} aae_coop_t;

void aae_coop_reset(aae_coop_t *coop);

// ---- the packets: "\xe1\0AAECP1", version 1, kind 1 hello (client), 2 state (host), 3 acknowledgement (client)
void aae_coop_packet(uint8_t out[AAE_Coop_Packet_Size], uint32_t kind, uint64_t a, uint64_t b, uint64_t c,
                     uint32_t d);
// whether a packet is one of these, which the game never sees
bool aae_coop_ours(const uint8_t *data, uint32_t size);
// one of them came: from where, the socket the game says it came for, its bytes (on the network thread)
void aae_coop_receive(aae_coop_t *coop, const aae_coop_game_t *game, const aae_coop_adr_t *from, int msg_sock,
                      const uint8_t *data, uint32_t size);
// what is to go out now, behind local client 0's own (on the network thread)
typedef struct {
    int sock;
    aae_coop_adr_t to;
    uint8_t data[AAE_Coop_Packet_Size];
} aae_coop_outgoing_t;
int aae_coop_send(aae_coop_t *coop, const aae_coop_game_t *game, aae_coop_outgoing_t out[AAE_Coop_Slots + 2]);

// ---- the hooks' rules
// the server's frame: the msec it is to run with
int aae_coop_server_frame(aae_coop_t *coop, const aae_coop_game_t *game, int msec);
// the server's pause test: true to let the game's own one answer, false to answer 0 (not paused)
bool aae_coop_check_paused(aae_coop_t *coop, const aae_coop_game_t *game);
// before the client's frame (changes msec and frac) and after it, with what the first one gave
typedef struct {
    uint64_t news;   // how many states from the host were taken when this frame began, 0 for none
    bool by_host;    // this frame's answer to "paused by the host"
} aae_coop_frame_t;
void aae_coop_client_frame(aae_coop_t *coop, const aae_coop_game_t *game, int local_client, int *msec, float *frac,
                           aae_coop_frame_t *frame);
void aae_coop_client_frame_done(aae_coop_t *coop, const aae_coop_game_t *game, const aae_coop_frame_t *frame);
// Engine.CanPauseGame: true to say yes, false to let the game answer
bool aae_coop_can_pause(aae_coop_t *coop, const aae_coop_game_t *game);
// a server command the client is about to take apart: the text to take apart instead
const char *aae_coop_server_command(aae_coop_t *coop, const aae_coop_game_t *game, const char *text);

// the rules on their own
bool aae_coop_in_map(const char *map);
bool aae_coop_switch_on(const aae_coop_value_t *value);
