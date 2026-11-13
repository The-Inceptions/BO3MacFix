// All-around Enhancement 4's games on the local network, joined as its Windows plugin makes them work (AAELanJoin).
// See aae_lan.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void aae_lan_init(uint64_t game_base_address, int enabled);

// The plugin's query and reply, on UDP port 27018: 24 bytes each, "AAELAN35", a number the asker picks (u32), the
// kind (u32: 1 a query, 2 a reply) and in a reply the host's Steam ID (u64), all little-endian.
#define AAE_LAN_PORT 27018
#define AAE_LAN_PACKET 24

// A Steam ID of a person: an individual account (type 1) in the public universe (1) with an account number.
bool aae_lan_is_individual(uint64_t id);
// An IPv4 address (its first number in the high byte) of a local network: 10/8, 172.16/12, 192.168/16, 127/8 or
// 169.254/16.
bool aae_lan_is_private(uint32_t address);

// A query as the host takes one: exactly 24 bytes, the magic and the kind 1. Gives the asker's number.
bool aae_lan_read_query(const uint8_t *packet, size_t length, uint32_t *number);
// Whether the host answers a query: it came from a local network, the game is in LAN (1) or online (2) mode and the
// host's Steam ID is a person's.
bool aae_lan_answers(uint32_t from, int network_mode, uint64_t id);
// The host's reply: the asker's number and the host's Steam ID.
void aae_lan_reply(uint8_t packet[AAE_LAN_PACKET], uint32_t number, uint64_t id);

// ---- the joiner's side

// A query as the joiner sends it, with its number: the low half of the time in ms, the connect's count shifted into
// the upper half, and a constant of the plugin's.
void aae_lan_query(uint8_t packet[AAE_LAN_PACKET], uint32_t number);
uint32_t aae_lan_number(uint64_t now, uint32_t attempt);
// A reply as the joiner takes one: exactly 24 bytes from the address and port asked, the magic, the kind 2, the
// query's number and a person's Steam ID. Gives the Steam ID.
bool aae_lan_read_reply(const uint8_t *packet, size_t length, uint32_t from, uint16_t from_port, uint32_t asked,
                        uint16_t asked_port, uint32_t number, uint64_t *id);

// "address@steamid": the Steam ID of the host given with the address, in decimal or (after "0x") in hexadecimal.
// When the part after the first "@" is a person's Steam ID, gives it and the part before (at most 95 characters).
#define AAE_LAN_ADDRESS 96
bool aae_lan_split(const char *text, char address[AAE_LAN_ADDRESS], uint64_t *id);

// What the plugin keeps about the player's connect, and about the hosts it asked.
#define AAE_LAN_CACHED 8
typedef struct {
    uint32_t address;
    uint64_t id;      // 0: the host didn't answer
    uint64_t expires; // in ms
} aae_lan_cached_t;
typedef struct {
    uint32_t attempt;  // connect commands so far
    uint64_t started;  // when the last one began, in ms; 0 for none
    uint32_t target;   // the address it joins: the first one of a local network pinged in its first 10 s, 0 for none
    uint32_t pinged;   // the address last pinged
    bool matched;      // the last ping was at the target, within 90 s of the start
    uint64_t given_id; // a Steam ID given after "@" with the address, 0 for none
    uint32_t requested, requested_attempt; // an address waiting to be asked, and the connect it is asked for
    uint32_t asking;   // the address being asked now
    aae_lan_cached_t cached[AAE_LAN_CACHED];
} aae_lan_join_t;

// a connect begins
void aae_lan_connect(aae_lan_join_t *join, uint64_t now);
// The game is about to ask Steam about the server at this address: whether it is the connect's target, and if so
// whether the host is to be asked for its Steam ID (true: join->requested is set for the asking thread).
bool aae_lan_ping(aae_lan_join_t *join, uint32_t address, uint64_t now);
// The host's Steam ID as the cache has it, while it hasn't expired: 0 when it didn't answer. False for nothing kept.
bool aae_lan_cached(const aae_lan_join_t *join, uint32_t address, uint64_t now, uint64_t *id);
// Keeps what the host answered: for 5 s, or for 0.6 s when it didn't. Replaces this address's entry or an expired
// one, else the one that expires first.
void aae_lan_cache(aae_lan_join_t *join, uint32_t address, uint64_t id, uint64_t now);
// Makes sure the host at this address is asked for this connect: 0 when it can't be (not a local network address),
// 1 when its answer is kept or on its way already, 2 when it is to be asked now (join->requested is set).
int aae_lan_ask(aae_lan_join_t *join, uint32_t address, uint32_t attempt, uint64_t now);

// What becomes of the game's Join to the server Steam found at the address.
typedef struct {
    bool window;     // it came from a ping at the connect's target within 90 s
    bool swap;       // and the game is in LAN or online mode with the target on a local network: the plugin's join
    bool wait;       // and no Steam ID was given, and the server's is a game server's: the host's answer is waited for
    int join_type;   // 4 (a direct join) for the game's 0 when it swaps
    uint64_t id;     // the Steam ID given with the address, when it swaps
    uint32_t attempt, target; // the connect and its target as they were
} aae_lan_plan_t;
aae_lan_plan_t aae_lan_plan(aae_lan_join_t *join, uint64_t now, int network_mode, uint64_t server, int join_type);
// While the host's answer is awaited: true when the wait is over, because another connect began or the answer is
// kept (then in *id; 0 when the host didn't answer).
bool aae_lan_waited(const aae_lan_join_t *join, const aae_lan_plan_t *plan, uint64_t now, uint64_t *id);
// Whether the Join is dropped: it came in the window and another connect began, or the target changed, meanwhile.
bool aae_lan_dropped(const aae_lan_join_t *join, const aae_lan_plan_t *plan);

// ---- lobby messages
// A lobby message's packet as the game sends one to a player (an int 0x28, then '1', the message's type and data):
// to a person's Steam ID the plugin sends those through the player's own Steam networking first.
bool aae_lan_lobby_message(uint64_t to, const uint8_t *packet, uint32_t length);

// ---- the host's join checks
// The plugin's dvar aae_offlinejoin is on when its text is "1" (a dvar of text, as the plugin makes it), or when it is
// a bool, int or enum dvar whose value is 1.
bool aae_lan_switch_on(int type, const char *text, int32_t value);
// In LAN mode the host lets in only joiners in LAN mode; the plugin lets in a joiner in online mode too, when it
// joins directly (join type 4) and aae_offlinejoin is on.
bool aae_lan_offline_join(int joiner_mode, int join_type, int host_mode, bool switched_on);
