// All-around Enhancement 4's pause for everybody in a co-op game: the plugin's coop_pause component.
//
// The game pauses only for a player alone on their machine: in a game with more than one client its own test lets
// nobody pause (it puts cl_paused back to 0 at once). With the plugin, the host of a co-op game (the machine whose
// server runs the game) can pause it for everybody, when the mod's HUD allows it (aae_coop_pause_remote 1: in every
// game but a public zombies match). The host's menu key pauses its own view at once; its machine tells every other
// machine "paused" in packets of its own; each of those, if it has the plugin too, pauses (cl_paused 1, and
// aae_coop_paused 1, which the mod's menus show) and acknowledges; when all have, the host's server stops as well. A
// client that hasn't acknowledged within 3.5 s cancels it. Two or more players on one machine (split screen) can pause
// as well: then nothing goes over the network, the game's own refusal is skipped and the server's and the client's
// time are held while cl_paused is 1.
//
// The packets are the plugin's own, 48 bytes that begin with "\xe1\0AAECP1", sent with the game's NET_SendPacket
// right after the game's own traffic for local client 0, and taken out of what the game's NET_GetPacket gives before
// the game sees them. They go over the game's connection like its own packets. Everything here is the plugin's
// behaviour, byte for byte and with its timings (in milliseconds; its clock, GetTickCount64, moves in steps of about
// 15.6 ms, this one in steps of 1 ms), so a Mac player can play with Windows players who have the plugin, as the
// host or as a client.
//
// The plugin's places in the Windows game, and here:
// - the server's pause test (SV_CheckPaused): hooked, as on Windows;
// - the server's frame (SV_Frame) where the game's frame calls it: on the Mac its two calls there (one with the
//   frame's time, one with -1) are told apart from the third, which the plugin leaves alone, by where they return to;
// - SV_Frame's call of the function that runs the server in 50 ms steps: not hooked here. On the Mac it gets
//   exactly SV_Frame's msec, which is 0 already whenever the plugin would make it 0 there: between the two only the
//   pause test runs, and in this build that only ever sets the "server held" flag when the host's rules had set it
//   just before, and the game's own test only ever lowers cl_paused;
// - the client's frame: hooked, as on Windows;
// - the client frame's call of the function that adds the frame's msec to the client's real time: not hooked. The
//   hooked client frame hands that function its own msec, which is 0 whenever the plugin's rule there says so,
//   with one exception: when cl_paused turns 1 during the client frame itself, before that call, the plugin holds
//   the real time already in that frame and this one from the next (one frame of the client's real time, which
//   nothing sends anywhere). The function can't be hooked at its start (its second instruction refers to its own
//   address); a detour on its "add" would do it;
// - Engine.CanPauseGame's function: its second instruction refers to its own address too, so it is a detour on
//   its first test (that "some menu is open" global is nonzero), which says yes by pointing the test at a 1 of ours;
// - the client's server commands: the tokenizer's hook acts only where the client takes apart a server command;
// - NET_GetPacket where the network thread asks it (by where it returns to), and the network thread's sending for a
//   local client: hooked.
// The plugin also still understands the server command "q @ <0|1> [<4 hex digits>]" its older version sent for this
// (and the game's own "q cl_paused <0|1>"), which this does as well.
//
// The plugin keeps its state in globals and guards what the network thread shares with a mutex; so does this. What
// the frame hooks keep is only touched from the game's own frame (as in the plugin, without a lock).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "aae_coop_pause.h"
#include "aae_plugin.h"
#include "detour.h"
#include "dobby.h"
#include "gsc_obj.h"
#include "offsets.h"
#include "prepatch.h"

#define Magic "\xe1\0AAECP1"
#define Magic_Size 8
#define Version 1
enum { Kind_Hello = 1, Kind_State = 2, Kind_Ack = 3 };
#define Packet_Version 0x8 // the packet's fields, little endian
#define Packet_Kind 0xc
#define Packet_A 0x10      // hello: 0; state: the host's nonce; acknowledgement: the host's nonce as last heard
#define Packet_B 0x18      // the client's nonce, never 0
#define Packet_C 0x20      // hello: 0; state: the host's sequence number; acknowledgement: the one acknowledged
#define Packet_D 0x28      // 0 or 1: paused
#define Packet_Tail 0x2c   // 0

#define Adr_Ip 0x0 // in a netadr_t
#define Adr_Port 0x4
#define Adr_Kind 0x8
#define Adr_Net_Id 0xc

#define Host_Sock 4         // the socket the host's packets go with (the server's)
#define Max_Sock 4          // NET_SendPacket refuses a socket or a local net id above this
#define Client_Connected 3  // a server client's state: connected from this on,
#define Client_Active 5     // in the game at this one
#define Connection_Has_Server 4 // local client 0's connection state from which it has the server's address
#define Map_Longest 64

#define Frame_Fresh_Ms 5000 // the network side acts only while local client 0's frame ran within this
#define Resend_Ms 70        // the host's state again to a client that hasn't acknowledged it yet
#define Repeat_Ms 800       // and to one that has; a client's hello and acknowledgement
#define Answer_Ms 3500      // the clients have this long to acknowledge a change of the state or of the clients
#define Heard_Ms 4000       // an acknowledgement counts while its client was heard within this
#define Silent_Ms 10000     // a client paused by a host it hasn't heard from for this unpauses
#define Command_Ms 2000     // a "q @" command counts within this of the last client frame in a map
#define Counts_Ms 16        // how long the plugin keeps its counts of the server's clients
#define Swallowed_Most 32   // packets of these taken out in a row by one NET_GetPacket

// ---- small things

static uint32_t get32(const uint8_t *at) {
    uint32_t value;
    memcpy(&value, at, sizeof(value)); // both the Mac's and the tests' CPU keep numbers little endian, as the packets
    return value;
}
static uint64_t get64(const uint8_t *at) {
    uint64_t value;
    memcpy(&value, at, sizeof(value));
    return value;
}
static void put32(uint8_t *at, uint32_t value) {
    memcpy(at, &value, sizeof(value));
}
static void put64(uint8_t *at, uint64_t value) {
    memcpy(at, &value, sizeof(value));
}
static uint32_t adr_kind(const aae_coop_adr_t *adr) {
    return get32(adr->bytes + Adr_Kind);
}
static uint32_t adr_net_id(const aae_coop_adr_t *adr) {
    return get32(adr->bytes + Adr_Net_Id);
}
// the plugin's comparison of two addresses: all but the two unused bytes behind the port
static bool same_adr(const aae_coop_adr_t *a, const aae_coop_adr_t *b) {
    return get32(a->bytes + Adr_Ip) == get32(b->bytes + Adr_Ip) &&
           memcmp(a->bytes + Adr_Port, b->bytes + Adr_Port, 2) == 0 && adr_kind(a) == adr_kind(b) &&
           adr_net_id(a) == adr_net_id(b);
}
// another machine's address (3 a plain IP, 4 the kind the game's Steam connection has)
static bool remote_kind(const aae_coop_adr_t *adr) {
    return adr_kind(adr) - 3 <= 1;
}

void aae_coop_reset(aae_coop_t *coop) {
    memset(coop, 0, sizeof(*coop));
    coop->role = -1;
    coop->sock = -1;
    coop->sequence = 1;
    coop->coop_paused_written = -1;
    coop->last_cl_paused = -999;
    coop->total = coop->remote = coop->remote_active = -1;
}

// what the plugin does when the machine becomes host or client or neither (its FUN_180012ef0)
static void forget_network(aae_coop_t *coop) {
    memset(coop->slots, 0, sizeof(coop->slots));
    memset(&coop->server, 0, sizeof(coop->server));
    coop->server_id = 0;
    coop->heard = 0;
    coop->server_valid = false;
    coop->role = -1;
    coop->sock = -1;
    coop->known_sequence = 0;
    coop->known_nonce = 0;
    coop->own_nonce = 0;
    coop->host_nonce = 0;
    coop->sequence = 1;
    coop->confirmed = 0;
    coop->mask = 0;
    coop->known_state = 0;
    coop->state = 0;
    coop->ack_at = 0;
    coop->hello_at = 0;
    coop->changed_at = 0;
    coop->acked_news = 0;
    coop->news = 0;
    coop->timed_out = false;
}

// ---- what the game says, read the plugin's way

bool aae_coop_in_map(const char *map) {
    if (map == NULL)
        return false;
    size_t length = strnlen(map, Map_Longest + 1);
    if (length == 0 || length > Map_Longest)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)map[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return strcmp(map, "core_frontend") != 0;
}

// a dvar the mod sets to switch something on: a number above 0, or the text "1" and nothing else
bool aae_coop_switch_on(const aae_coop_value_t *value) {
    if (value->kind == AAE_Coop_Number)
        return value->number > 0;
    return value->kind == AAE_Coop_Text && value->text != NULL && strcmp(value->text, "1") == 0;
}

static bool in_map(const aae_coop_game_t *game) {
    aae_coop_value_t map;
    game->value(game, AAE_Coop_Map, &map);
    return map.kind == AAE_Coop_Text && aae_coop_in_map(map.text);
}
// a whole number dvar: -1 for none, -2 for one of another kind
static int number(const aae_coop_game_t *game, int dvar) {
    aae_coop_value_t value;
    game->value(game, dvar, &value);
    if (value.kind == AAE_Coop_None)
        return -1;
    return value.kind == AAE_Coop_Number ? (int)value.number : -2;
}
// sv_running: 1 while this machine's server runs the game, -1 when it can't be read
static int sv_running(const aae_coop_game_t *game) {
    aae_coop_value_t value;
    game->value(game, AAE_Coop_Sv_Running, &value);
    if (value.kind != AAE_Coop_Number || value.number < 0 || value.number > 1)
        return -1;
    return (int)value.number;
}
static bool pause_remote(const aae_coop_game_t *game) {
    aae_coop_value_t value;
    game->value(game, AAE_Coop_Pause_Remote, &value);
    return aae_coop_switch_on(&value);
}
// a client of another machine (not a bot)
static bool is_remote(const aae_coop_client_t *client) {
    return remote_kind(&client->adr) && client->bot == 0;
}

// The server's clients counted: those connected, of other machines, of those the active ones; whether there is an
// active one of this machine, and a human one. Kept for 16 ms. The total, -1 when the count dvar isn't 1..18 or
// there is no table.
static int counts(aae_coop_t *coop, const aae_coop_game_t *game) {
    uint64_t now = game->now(game);
    if (coop->counted_at != 0 && now - coop->counted_at < Counts_Ms)
        return coop->total;
    int most = number(game, AAE_Coop_Max_Clients);
    coop->total = -1;
    coop->local_active = coop->local_human = false;
    coop->remote_active = coop->remote = -1;
    coop->counted_at = now;
    if ((unsigned)(most - 1) >= AAE_Coop_Slots)
        return -1;
    int total = 0, remote = 0, remote_active = 0;
    for (int i = 0; i < most; i++) {
        aae_coop_client_t client;
        if (!game->client(game, i, &client))
            return -1;
        if (client.state < Client_Connected)
            continue;
        total++;
        if (is_remote(&client)) {
            remote++;
            remote_active += client.state == Client_Active;
        } else if (client.state == Client_Active) {
            coop->local_active = true;
            if (client.bot == 0 && !remote_kind(&client.adr))
                coop->local_human = true;
        }
    }
    coop->remote = remote;
    coop->remote_active = remote_active;
    coop->total = total;
    return total;
}

// Only players of this machine: in a map, with clients and none of another machine, this machine's server running
// (or it ran at the last server frame in a map and one of them is a human).
static bool alone(aae_coop_t *coop, const aae_coop_game_t *game) {
    if (!in_map(game))
        return false;
    int running = sv_running(game);
    if (running == 0 || counts(coop, game) <= 0)
        return false;
    return coop->remote == 0 && (running == 1 || (coop->sv_in_map && coop->local_human));
}

// The host may pause for everybody: the mod allows it, this machine's server runs a map and a client of another
// machine is in the game.
static bool requested_host(aae_coop_t *coop, const aae_coop_game_t *game) {
    if (!pause_remote(game) || !in_map(game) || sv_running(game) != 1)
        return false;
    return (counts(coop, game) < 0 ? -1 : coop->remote_active) > 0;
}

// A client the host paused, while in a map. Out of a map what came from the host is forgotten.
static bool paused_by_host(aae_coop_t *coop, const aae_coop_game_t *game) {
    if (!coop->remote_known || coop->remote_state == 0)
        return false;
    if (in_map(game))
        return true;
    coop->remote_state = 0;
    coop->remote_known = false;
    coop->remote_at = 0;
    coop->paused_by_host = false;
    return false;
}

// ---- the host's rules (the plugin's FUN_180013080), every server frame and client frame of the host

static void forget_sending(aae_coop_t *coop) {
    for (int i = 0; i < AAE_Coop_Slots; i++) {
        coop->slots[i].sends = 0;
        coop->slots[i].sent = 0;
    }
}

static void host_rules(aae_coop_t *coop, const aae_coop_game_t *game) {
    game->lock(game);
    if (coop->role != 1 || !in_map(game)) {
        coop->host_timed_out = coop->server_paused = coop->host_view_frozen = false;
        coop->waiting = coop->all_confirmed = false;
        game->unlock(game);
        return;
    }
    // the clients that take part: of another machine, active, with an id and an address the game can send to
    int count = number(game, AAE_Coop_Max_Clients), most = (unsigned)(count - 2) <= AAE_Coop_Slots - 2 ? count : 0;
    uint32_t mask = 0;
    bool not_ready = false; // one of another machine is connecting, or can't take part
    for (int i = 0; i < AAE_Coop_Slots; i++) {
        aae_coop_slot_t seen = { 0 };
        aae_coop_client_t client;
        if (i < most && game->client(game, i, &client) && client.state >= Client_Connected && is_remote(&client)) {
            if (client.state == Client_Active) {
                seen.adr = client.adr;
                seen.id = client.id;
                seen.ok = client.id != 0 && remote_kind(&client.adr) && adr_net_id(&client.adr) <= Max_Sock;
            }
            not_ready = not_ready || !seen.ok || client.state != Client_Active;
        }
        // a slot whose client is another one now starts anew
        aae_coop_slot_t *slot = &coop->slots[i];
        if (slot->ok != seen.ok || slot->id != seen.id || !same_adr(&slot->adr, &seen.adr))
            *slot = seen;
        if (slot->ok)
            mask |= 1u << i;
    }
    uint64_t now = game->now(game);
    if (mask != coop->mask) {
        coop->mask = mask;
        coop->sequence++;
        coop->changed_at = now;
        coop->timed_out = false;
        forget_sending(coop);
    }
    bool requested = pause_remote(game) && number(game, AAE_Coop_Cl_Paused) == 1;
    uint32_t wanted = requested && !coop->timed_out;
    if (!requested)
        coop->timed_out = false;
    if (coop->state != wanted) {
        coop->state = wanted;
        coop->sequence++;
        coop->changed_at = now;
        coop->timed_out = false;
        forget_sending(coop);
    }
    // every client that takes part acknowledged this state and was heard lately
    bool all = mask != 0 && !not_ready && coop->host_nonce != 0 && (unsigned)coop->sock <= Max_Sock;
    for (int i = 0; i < AAE_Coop_Slots; i++) {
        const aae_coop_slot_t *slot = &coop->slots[i];
        if (slot->ok && (slot->acked != coop->sequence || slot->heard == 0 || now - slot->heard >= Heard_Ms))
            all = false;
    }
    if (all)
        coop->confirmed = coop->sequence;
    bool paused = false;
    coop->host_view_frozen = false;
    if (requested) {
        if (mask != 0 || not_ready) {
            // too late: the pause is given up until it is asked for anew
            if (coop->confirmed != coop->sequence && !coop->timed_out && coop->changed_at != 0 &&
                now - coop->changed_at >= Answer_Ms) {
                if (coop->state != 0) {
                    coop->state = 0;
                    coop->sequence++;
                    coop->changed_at = now;
                    forget_sending(coop);
                }
                coop->timed_out = true;
            }
            paused = mask == 0 ? !not_ready
                               : !not_ready && coop->confirmed == coop->sequence && !coop->timed_out;
        } else {
            paused = true;
        }
        coop->server_paused = paused;
        coop->host_view_frozen = mask != 0 && !coop->timed_out;
    } else {
        coop->server_paused = false;
    }
    coop->host_timed_out = coop->timed_out;
    coop->waiting = requested && mask != 0 && !coop->timed_out;
    coop->all_confirmed = coop->confirmed == coop->sequence && !coop->timed_out;
    coop->paused_at = paused ? now : 0;
    game->unlock(game);
}

// the rules above, but not while they or the server's pause test are running already (the plugin's FUN_180012420)
static void host_frame(aae_coop_t *coop, const aae_coop_game_t *game) {
    if (coop->busy)
        return;
    coop->busy = true;
    host_rules(coop, game);
    coop->busy = false;
}

// ---- the packets

void aae_coop_packet(uint8_t out[AAE_Coop_Packet_Size], uint32_t kind, uint64_t a, uint64_t b, uint64_t c,
                     uint32_t d) {
    memset(out, 0, AAE_Coop_Packet_Size);
    memcpy(out, Magic, Magic_Size);
    put32(out + Packet_Version, Version);
    put32(out + Packet_Kind, kind);
    put64(out + Packet_A, a);
    put64(out + Packet_B, b);
    put64(out + Packet_C, c);
    put32(out + Packet_D, d);
}

bool aae_coop_ours(const uint8_t *data, uint32_t size) {
    return size >= Magic_Size && data != NULL && memcmp(data, Magic, Magic_Size) == 0;
}

void aae_coop_receive(aae_coop_t *coop, const aae_coop_game_t *game, const aae_coop_adr_t *from, int msg_sock,
                      const uint8_t *data, uint32_t size) {
    if (size != AAE_Coop_Packet_Size || get32(data + Packet_Version) != Version || get32(data + Packet_Tail) != 0)
        return;
    uint32_t kind = get32(data + Packet_Kind), d = get32(data + Packet_D);
    uint64_t a = get64(data + Packet_A), b = get64(data + Packet_B), c = get64(data + Packet_C);
    if (d > 1 || kind - 1 > 2 || b == 0)
        return;
    game->lock(game);
    uint64_t now = game->now(game);
    if (coop->sock < 0 || msg_sock != coop->sock || coop->frame_at == 0 || now - coop->frame_at > Frame_Fresh_Ms) {
        game->unlock(game);
        return;
    }
    if (coop->role == 1) {
        for (int i = 0; i < AAE_Coop_Slots; i++) {
            aae_coop_slot_t *slot = &coop->slots[i];
            if (!slot->ok || !same_adr(&slot->adr, from))
                continue;
            if (kind == Kind_Hello) {
                if (a == 0 && (c | d) == 0) {
                    if (slot->nonce != b) {
                        // a client new to this slot, or the same one started anew
                        slot->nonce = b;
                        slot->sends = 0;
                        slot->sent = 0;
                        slot->acked = 0;
                    }
                    slot->heard = now;
                }
            } else if (kind == Kind_Ack && b == slot->nonce && a == coop->host_nonce && c == coop->sequence &&
                       d == coop->state) {
                slot->acked = c;
                slot->heard = now;
            }
            break;
        }
    } else if (coop->role == 0 && coop->server_valid && same_adr(&coop->server, from) && kind == Kind_State &&
               b == coop->own_nonce && a != 0 && c != 0) {
        if (coop->known_nonce != 0 && coop->known_nonce != a) {
            // another host than the one heard so far: start anew with it
            coop->own_nonce = game->nonce(game);
            coop->known_sequence = 0;
            coop->known_nonce = 0;
            coop->hello_at = 0;
        } else if (c >= coop->known_sequence && (c != coop->known_sequence || d == coop->known_state)) {
            // the newest state, or the same one again (another state under a known number is not taken)
            coop->known_nonce = a;
            coop->heard = now;
            if (coop->known_sequence != c) {
                coop->news++;
                coop->known_sequence = c;
                coop->acked_news = 0;
                coop->known_state = d;
            }
            coop->ack_at = 0;
        }
    }
    game->unlock(game);
}

int aae_coop_send(aae_coop_t *coop, const aae_coop_game_t *game, aae_coop_outgoing_t out[AAE_Coop_Slots + 2]) {
    int count = 0;
    game->lock(game);
    uint64_t now = game->now(game);
    if (coop->frame_at == 0 || now - coop->frame_at > Frame_Fresh_Ms || (unsigned)coop->sock > Max_Sock) {
        game->unlock(game);
        return 0;
    }
    if (coop->role == 1) {
        // the state to every client that said hello: quickly until it acknowledged it, then now and then
        for (int i = 0; i < AAE_Coop_Slots && coop->host_nonce != 0; i++) {
            aae_coop_slot_t *slot = &coop->slots[i];
            if (!slot->ok || slot->nonce == 0)
                continue;
            uint64_t every = slot->acked == coop->sequence ? Repeat_Ms : Resend_Ms;
            if (slot->sent != 0 && now - slot->sent < every)
                continue;
            slot->sent = now;
            slot->sends++;
            out[count].sock = coop->sock;
            out[count].to = slot->adr;
            aae_coop_packet(out[count++].data, Kind_State, coop->host_nonce, slot->nonce, coop->sequence, coop->state);
        }
    } else if (coop->role == 0 && coop->server_valid && coop->own_nonce != 0) {
        if (coop->hello_at == 0 || now - coop->hello_at >= Repeat_Ms) {
            coop->hello_at = now;
            out[count].sock = coop->sock;
            out[count].to = coop->server;
            aae_coop_packet(out[count++].data, Kind_Hello, 0, coop->own_nonce, 0, 0);
        }
        // the acknowledgement of the newest state, once this client's frame put it into effect
        if (coop->acked_news != 0 && coop->acked_news == coop->news && coop->known_nonce != 0 &&
            coop->known_sequence != 0 && (coop->ack_at == 0 || now - coop->ack_at >= Repeat_Ms)) {
            coop->ack_at = now;
            out[count].sock = coop->sock;
            out[count].to = coop->server;
            aae_coop_packet(out[count++].data, Kind_Ack, coop->known_nonce, coop->own_nonce, coop->known_sequence,
                            coop->known_state);
        }
    }
    game->unlock(game);
    return count;
}

// ---- the hooks' rules

int aae_coop_server_frame(aae_coop_t *coop, const aae_coop_game_t *game, int msec) {
    coop->sv_in_map = in_map(game);
    host_frame(coop, game);
    if (requested_host(coop, game)) {
        if (coop->server_paused) {
            if (number(game, AAE_Coop_Cl_Paused) == 1)
                return 0;
            coop->server_paused = false;
        }
    } else {
        coop->server_paused = false;
    }
    return alone(coop, game) && number(game, AAE_Coop_Cl_Paused) == 1 ? 0 : msec;
}

bool aae_coop_check_paused(aae_coop_t *coop, const aae_coop_game_t *game) {
    if (!in_map(game))
        return true;
    int running = sv_running(game);
    if (running == 0)
        return !paused_by_host(coop, game);
    if (alone(coop, game)) {
        // more than one player of this machine: the game's "more than one client" refusal is skipped
        int total = counts(coop, game);
        return total <= 1 || number(game, AAE_Coop_Cl_Paused) != 1;
    }
    if (!requested_host(coop, game))
        return true;
    if (coop->busy)
        return false;
    coop->busy = true;
    int paused = number(game, AAE_Coop_Cl_Paused);
    if (coop->last_cl_paused != paused) {
        if (paused == 1)
            coop->counted_at = 0; // the clients are counted anew
        coop->last_cl_paused = paused;
    }
    if (paused != 1) {
        coop->server_paused = false;
        coop->busy = false;
        return true;
    }
    int total = counts(coop, game);
    if (total >= 1 && sv_running(game) == 1 && (counts(coop, game) < 0 ? -1 : coop->remote) == 0) {
        // Nobody of another machine is there. The plugin then resets what its older version keeps and holds the
        // server; it can't come here while the host's pause is asked for, which needs such a client.
        coop->host_view_frozen = coop->waiting = coop->all_confirmed = coop->host_timed_out = false;
        coop->paused_at = 0;
        coop->server_paused = true;
        coop->busy = false;
        return false;
    }
    if (total < 2) {
        if (total != 1 && sv_running(game) == 0) {
            coop->busy = false;
            return false;
        }
        coop->server_paused = false;
        coop->busy = false;
        return true;
    }
    // the host's own way of pausing instead of the game's: the server is held once the host's rules said so
    running = sv_running(game);
    int server_paused = number(game, AAE_Coop_Sv_Paused);
    if (running == 1 && server_paused >= 0 && coop->waiting && coop->all_confirmed && !coop->host_timed_out &&
        coop->paused_at != 0 && game->now(game) >= coop->paused_at)
        coop->server_paused = true;
    coop->busy = false;
    return false;
}

void aae_coop_client_frame(aae_coop_t *coop, const aae_coop_game_t *game, int local_client, int *msec, float *frac,
                           aae_coop_frame_t *frame) {
    bool map = in_map(game);
    int running = map ? sv_running(game) : -1;
    frame->news = 0;
    if (local_client == 0) {
        bool pause = false, unpause = false; // what goes into cl_paused
        game->lock(game);
        coop->frame_at = game->now(game);
        int role = map ? running : -1;
        if (role != coop->role) {
            unpause = coop->applied;
            coop->applied = false;
            forget_network(coop);
            coop->role = role;
            if (role == 1)
                coop->host_nonce = game->nonce(game);
            if (role == 0)
                coop->own_nonce = game->nonce(game);
        }
        if (coop->role == 1) {
            coop->sock = Host_Sock;
        } else if (coop->role == 0) {
            coop->sock = game->local_sock(game);
            // where the host is, valid when it is another machine the game can send to
            aae_coop_adr_t adr = { 0 };
            uint64_t id = 0;
            bool valid = false;
            if (game->connection(game) >= Connection_Has_Server && game->server(game, &adr, &id))
                valid = id != 0 && remote_kind(&adr) && adr_net_id(&adr) <= Max_Sock;
            bool silent = false;
            if (coop->server_valid != valid || coop->server_id != id || !same_adr(&coop->server, &adr)) {
                coop->server = adr;
                coop->server_id = id;
                coop->server_valid = valid;
                silent = true;
            } else if (coop->heard != 0 && coop->frame_at - coop->heard >= Silent_Ms && coop->known_state != 0) {
                silent = true;
            }
            if (silent) {
                // another server, or a paused one not heard from for 10 s: everything from it is forgotten
                coop->own_nonce = game->nonce(game);
                coop->acked_news = coop->news = 0;
                coop->known_sequence = coop->known_nonce = 0;
                coop->known_state = 0;
                coop->ack_at = coop->hello_at = coop->heard = 0;
                unpause = coop->applied;
                coop->applied = false;
                coop->remote_state = 0;
                coop->remote_known = false;
                coop->remote_at = 0;
                coop->paused_by_host = false;
            }
            if (coop->news != 0 && coop->server_valid) {
                coop->remote_state = (int)coop->known_state;
                coop->remote_known = true;
                coop->remote_at = coop->heard;
                if (coop->known_state != (uint32_t)coop->applied) {
                    pause = coop->known_state != 0;
                    unpause = !pause && coop->applied;
                    coop->applied = pause;
                }
                frame->news = coop->news;
            }
        }
        game->unlock(game);
        if (pause || unpause)
            game->set_dvar(game, "cl_paused", pause ? "1" : "0");
    }
    if (running == 1)
        host_frame(coop, game);
    coop->client_in_map = map && running == 0;
    if (!coop->client_in_map) {
        coop->remote_state = 0;
        coop->remote_known = false;
        coop->remote_at = 0;
        coop->paused_by_host = false;
    }
    coop->client_frame_at = coop->client_in_map ? game->now(game) : 0;
    // the host's server is held
    bool host_paused = false;
    if (requested_host(coop, game) && coop->server_paused) {
        if (number(game, AAE_Coop_Cl_Paused) == 1)
            host_paused = true;
        else
            coop->server_paused = false;
    }
    // a client paused by the host, while still connected and hearing from it
    bool by_host = coop->client_in_map && paused_by_host(coop, game);
    if (by_host) {
        int connection = game->connection(game);
        uint64_t now;
        if ((connection >= 0 && connection < Connection_Has_Server) ||
            (coop->remote_at != 0 && (now = game->now(game)) >= coop->remote_at &&
             now - coop->remote_at >= Silent_Ms)) {
            coop->remote_state = 0;
            coop->remote_known = false;
            coop->remote_at = 0;
            by_host = false;
        }
    }
    coop->paused_by_host = frame->by_host = by_host;
    bool frozen = coop->host_view_frozen && in_map(game) && sv_running(game) == 1 &&
                  number(game, AAE_Coop_Cl_Paused) == 1;
    bool hold = host_paused || by_host || (alone(coop, game) && number(game, AAE_Coop_Cl_Paused) == 1) || frozen;
    if (coop->coop_paused_written != (int)by_host) {
        game->set_dvar(game, "aae_coop_paused", by_host ? "1" : "0");
        coop->coop_paused_written = by_host;
    }
    if (hold) {
        *msec = 0;
        *frac = 0.0f;
    }
}

// After the client's frame: once it put the newest state into effect, that is acknowledged.
void aae_coop_client_frame_done(aae_coop_t *coop, const aae_coop_game_t *game, const aae_coop_frame_t *frame) {
    if (frame->news == 0)
        return;
    game->lock(game);
    if (coop->role == 0 && frame->news == coop->news && (uint32_t)frame->by_host == coop->known_state &&
        coop->acked_news != frame->news) {
        coop->acked_news = frame->news;
        coop->ack_at = 0;
    }
    game->unlock(game);
}

bool aae_coop_can_pause(aae_coop_t *coop, const aae_coop_game_t *game) {
    if (!in_map(game))
        return false;
    int running = sv_running(game);
    if (alone(coop, game))
        return true;
    if (running == 1)
        return requested_host(coop, game) && !coop->host_timed_out;
    return running == 0 && paused_by_host(coop, game);
}

static bool hex_digit(char c) {
    return (unsigned char)(c - '0') <= 9 || (unsigned char)(c - 'a') <= 5;
}

const char *aae_coop_server_command(aae_coop_t *coop, const aae_coop_game_t *game, const char *text) {
    const char *given = text;
    int state = -1;
    // "q @ <0|1>", or with a 16-bit counter in lowercase hex behind it, from the plugin's older version
    if (text != NULL && strncmp(text, "q @ ", 4) == 0 && (unsigned char)(text[4] - '0') <= 1 &&
        (text[5] == '\0' || (text[5] == ' ' && hex_digit(text[6]) && hex_digit(text[7]) && hex_digit(text[8]) &&
                             hex_digit(text[9]) && text[10] == '\0'))) {
        state = text[4] - '0';
        if (in_map(game))
            given = state != 0 ? "q cl_paused \"1\"" : "q cl_paused \"0\"";
    }
    if (!in_map(game) || text == NULL)
        return given;
    if (state < 0) {
        // the game's own "set cl_paused" command: "q cl_paused ", each quote around the 0 or 1 left out or not
        if (strncmp(text, "q cl_paused ", 12) != 0)
            return given;
        const char *value = text + 12 + (text[12] == '"');
        if ((unsigned char)(value[0] - '0') > 1)
            return given;
        const char *rest = value + 1 + (value[1] == '"');
        if (*rest != '\0')
            return given;
        state = value[0] - '0';
    }
    uint64_t now = game->now(game);
    bool lately = coop->client_in_map && coop->client_frame_at != 0 && now >= coop->client_frame_at &&
                  now - coop->client_frame_at <= Command_Ms;
    if (lately || (state == 0 && coop->client_in_map)) {
        coop->remote_state = state;
        coop->remote_known = true;
        coop->remote_at = state != 0 ? now : 0;
    }
    return given;
}

// ---- the game's side

#define Dvar_Type 0x1c
#define Dvar_Value 0x28
enum { Dvar_Bool = 1, Dvar_Int = 6, Dvar_Enum = 7, Dvar_String = 8 };
#define Client_Size 0xe5270    // client_t (0x100 bigger than the Windows game's)
#define Client_State 0x0
#define Client_Adr 0x2c
#define Client_Id 0x55c8
#define Client_Bot 0xbb460     // the Windows game's +0xbb360
#define Clc_Server 0x2455c     // the client's connection data: the server's address,
#define Clc_Server_Id 0x24b68  // and its 8-byte id
#define Msg_Data 0x8           // msg_t: the packet's bytes (behind the game's header byte),
#define Msg_Size 0x1c          // how many,
#define Msg_Sock 0x34          // the socket the packet came for (the header byte's upper half)
#define Msg_Kept 0x38          // what NET_GetPacket changes of it

static int debug;
static aae_coop_t coop;
static pthread_mutex_t network_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool installed;

// the game's
static void *(*Dvar_FindByHash)(uint32_t hash);
static const char *(*Dvar_GetString)(void *dvar);
static void (*Dvar_SetFromStringByName)(const char *name, const char *value, bool create);
static bool (*NET_SendPacket)(int sock, int length, const void *data, uint64_t to_low, uint64_t to_high);
static int (*CL_LocalClientSocket)(int local_client);
static uint8_t *const *dvar_cl_paused, *const *dvar_sv_paused, *const *dvar_max_clients, *const *dvar_sv_running;
static uint8_t *const *server_clients;
static uint8_t *const *client_connections;
static const int32_t *connection_state;
static const void *com_frame_runs_server, *com_frame_runs_server_late, *netchan_receives, *client_takes_command;

static uint64_t uptime_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}
static uint64_t (*now_ms)(void) = uptime_ms;

static uint64_t game_now(const aae_coop_game_t *game) {
    (void)game;
    return now_ms();
}

static void read_dvar(uint8_t *dvar, aae_coop_value_t *value) {
    memset(value, 0, sizeof(*value));
    if (dvar == NULL)
        return;
    uint32_t type;
    memcpy(&type, dvar + Dvar_Type, sizeof(type));
    value->kind = AAE_Coop_Number;
    if (type == Dvar_Bool) {
        value->number = dvar[Dvar_Value];
    } else if (type == Dvar_Int || type == Dvar_Enum) {
        int32_t whole;
        memcpy(&whole, dvar + Dvar_Value, sizeof(whole));
        value->number = whole;
    } else if (type == Dvar_String) {
        value->kind = AAE_Coop_Text;
        value->text = Dvar_GetString(dvar);
    } else {
        value->kind = AAE_Coop_Other;
    }
}

// a dvar found by its name once it is there
static uint8_t *named_dvar(const char *name, _Atomic(uint8_t *) *found) {
    uint8_t *dvar = atomic_load_explicit(found, memory_order_relaxed);
    if (dvar == NULL) {
        dvar = Dvar_FindByHash(gsc_hash(name));
        atomic_store_explicit(found, dvar, memory_order_relaxed);
    }
    return dvar;
}
static _Atomic(uint8_t *) dvar_mapname, dvar_pause_remote;

static void game_value(const aae_coop_game_t *game, int which, aae_coop_value_t *value) {
    (void)game;
    uint8_t *dvar = NULL;
    switch (which) {
    case AAE_Coop_Map: dvar = named_dvar("mapname", &dvar_mapname); break;
    case AAE_Coop_Sv_Running: dvar = *dvar_sv_running; break;
    case AAE_Coop_Cl_Paused: dvar = *dvar_cl_paused; break;
    case AAE_Coop_Sv_Paused: dvar = *dvar_sv_paused; break;
    case AAE_Coop_Max_Clients: dvar = *dvar_max_clients; break;
    case AAE_Coop_Pause_Remote: dvar = named_dvar("aae_coop_pause_remote", &dvar_pause_remote); break;
    }
    read_dvar(dvar, value);
}

static void game_set_dvar(const aae_coop_game_t *game, const char *name, const char *value) {
    (void)game;
    Dvar_SetFromStringByName(name, value, true);
}

// the server's client table holds as many as the count dvar says (the game's own loops over it go that far)
static bool game_client(const aae_coop_game_t *game, int index, aae_coop_client_t *client) {
    (void)game;
    const uint8_t *table = *server_clients;
    if (table == NULL)
        return false;
    const uint8_t *entry = table + (size_t)index * Client_Size;
    memcpy(&client->state, entry + Client_State, sizeof(client->state));
    memcpy(client->adr.bytes, entry + Client_Adr, sizeof(client->adr.bytes));
    memcpy(&client->id, entry + Client_Id, sizeof(client->id));
    memcpy(&client->bot, entry + Client_Bot, sizeof(client->bot));
    return true;
}

static int game_connection(const aae_coop_game_t *game) {
    (void)game;
    int32_t state = *connection_state;
    return state >= 0 && state < 12 ? state : -1;
}

static bool game_server(const aae_coop_game_t *game, aae_coop_adr_t *adr, uint64_t *id) {
    (void)game;
    const uint8_t *connection = *client_connections; // local client 0's
    if (connection == NULL)
        return false;
    memcpy(adr->bytes, connection + Clc_Server, sizeof(adr->bytes));
    memcpy(id, connection + Clc_Server_Id, sizeof(*id));
    return true;
}

static int game_local_sock(const aae_coop_game_t *game) {
    (void)game;
    return CL_LocalClientSocket(0);
}

static uint64_t game_nonce(const aae_coop_game_t *game) {
    (void)game;
    uint64_t nonce;
    arc4random_buf(&nonce, sizeof(nonce));
    return nonce; // a 0 stays 0, as in the plugin, which then sends nothing until the next one
}

static void game_lock(const aae_coop_game_t *game) {
    (void)game;
    pthread_mutex_lock(&network_lock);
}
static void game_unlock(const aae_coop_game_t *game) {
    (void)game;
    pthread_mutex_unlock(&network_lock);
}

static const aae_coop_game_t game = {
    .now = game_now, .value = game_value, .set_dvar = game_set_dvar, .client = game_client,
    .connection = game_connection, .server = game_server, .local_sock = game_local_sock, .nonce = game_nonce,
    .lock = game_lock, .unlock = game_unlock,
};

// ---- BO3MACFIX_COOPPAUSEDEBUG=1: what the hooks see, a line when it changes

static const char *yes_no(bool value) {
    return value ? "yes" : "no";
}

static void describe_adr(const aae_coop_adr_t *adr, char *out, size_t size) {
    const uint8_t *b = adr->bytes;
    uint16_t port;
    memcpy(&port, b + Adr_Port, sizeof(port));
    snprintf(out, size, "%u.%u.%u.%u:%u kind %u net id %u", b[0], b[1], b[2], b[3], port, adr_kind(adr),
             adr_net_id(adr));
}

// which thread each hook runs on, the first time (the plugin runs all but the network ones on the game's frame)
static uint64_t thread_id(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return id;
}
static _Atomic uint64_t frame_thread;
static void first_call(int which, const char *what) {
    static _Atomic unsigned char said[8];
    if (!debug || atomic_exchange(&said[which], 1))
        return;
    uint64_t id = thread_id(), frame = atomic_load(&frame_thread);
    printf("coop pause: %s, first call: thread %llu%s%s\n", what, (unsigned long long)id,
           pthread_main_np() ? " (the process's main thread)" : "",
           frame == 0 ? "" : frame == id ? ", the client frame's" : ", not the client frame's");
}

// the server's clients, the probe for what the plugin reads of them (only from the game's frame)
static void report_clients(void) {
    static char last[1024];
    char line[1024] = "";
    size_t used = 0;
    int most = number(&game, AAE_Coop_Max_Clients);
    for (int i = 0; i < most && i < AAE_Coop_Slots; i++) {
        aae_coop_client_t client;
        if (!game_client(&game, i, &client))
            break;
        if (client.state == 0)
            continue;
        char where[96];
        describe_adr(&client.adr, where, sizeof(where));
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%d: state %u, %s, bot %d, id %s",
                                 used ? "; " : "", i, client.state, where, client.bot, client.id != 0 ? "set" : "0");
        if (used >= sizeof(line))
            break;
    }
    if (strcmp(line, last) != 0) {
        snprintf(last, sizeof(last), "%s", line);
        printf("coop pause: the server's clients (count dvar %d): %s\n", most, line[0] ? line : "none");
    }
}

// What the rules answer now, worked out on a copy: they keep counts and forget things as they go.
static void report(void) {
    static char last[512];
    char line[512];
    static aae_coop_t copy;
    pthread_mutex_lock(&network_lock);
    copy = coop;
    pthread_mutex_unlock(&network_lock);
    aae_coop_value_t map, remote;
    game_value(&game, AAE_Coop_Map, &map);
    game_value(&game, AAE_Coop_Pause_Remote, &remote);
    bool in = in_map(&game);
    bool is_alone = alone(&copy, &game), host = requested_host(&copy, &game), can = aae_coop_can_pause(&copy, &game);
    snprintf(line, sizeof(line), "map %s (%s), sv_running %d, cl_paused %d, sv_paused %d, aae_coop_pause_remote %s, "
             "clients %d (other machines %d, of them active %d, a human here %s), alone %s, host may pause %s, "
             "CanPauseGame says yes %s", map.kind == AAE_Coop_Text && map.text ? map.text : "none",
             in ? "in a map" : "not in a map", sv_running(&game), number(&game, AAE_Coop_Cl_Paused),
             number(&game, AAE_Coop_Sv_Paused), remote.kind == AAE_Coop_None ? "none" : yes_no(pause_remote(&game)),
             copy.total, copy.remote, copy.remote_active, yes_no(copy.local_human), yes_no(is_alone), yes_no(host),
             yes_no(can));
    if (strcmp(line, last) != 0) {
        snprintf(last, sizeof(last), "%s", line);
        printf("coop pause: %s\n", line);
    }
    if (in && sv_running(&game) == 1) // the table is the running server's (the plugin reads it only then too)
        report_clients();
    static char last_net[512];
    char net[512];
    pthread_mutex_lock(&network_lock);
    if (coop.role == 1) {
        snprintf(net, sizeof(net), "host: state %u, sequence %llu, confirmed %llu, slots taking part 0x%x, timed out "
                 "%s, server held %s, view held %s", coop.state, (unsigned long long)coop.sequence,
                 (unsigned long long)coop.confirmed, coop.mask, yes_no(coop.timed_out), yes_no(coop.server_paused),
                 yes_no(coop.host_view_frozen));
    } else if (coop.role == 0) {
        char where[96];
        describe_adr(&coop.server, where, sizeof(where));
        snprintf(net, sizeof(net), "client (socket %d): server %s, valid %s, host's state %u (sequence %llu, %llu "
                 "new), put into cl_paused %s, paused by the host %s", coop.sock, where, yes_no(coop.server_valid),
                 coop.known_state, (unsigned long long)coop.known_sequence, (unsigned long long)coop.news,
                 yes_no(coop.applied), yes_no(coop.paused_by_host));
    } else {
        snprintf(net, sizeof(net), "neither host nor client");
    }
    pthread_mutex_unlock(&network_lock);
    if (strcmp(net, last_net) != 0) {
        snprintf(last_net, sizeof(last_net), "%s", net);
        printf("coop pause: %s\n", net);
    }
}

// a line when a frame starts or stops being held (only the game's frame comes here)
static void report_held(int which, bool held) {
    static int said[2] = { -1, -1 };
    if (!debug || said[which] == (int)held)
        return;
    said[which] = held;
    printf("coop pause: the %s frame %s\n", which == 0 ? "server's" : "client's", held ? "is held now" : "runs again");
}

// A packet in or out (only the network thread comes here), when it says something else than the last one of its
// kind to or from the same machine did: the repeats every 70 or 800 ms aren't shown.
static void report_packet(const char *way, const aae_coop_adr_t *peer, const uint8_t *data) {
    static struct {
        aae_coop_adr_t peer;
        uint32_t kind;
        char line[160];
    } last[2 * AAE_Coop_Slots];
    static int used, next;
    uint32_t kind = get32(data + Packet_Kind);
    char where[96], line[160];
    describe_adr(peer, where, sizeof(where));
    snprintf(line, sizeof(line), "%s %s, sequence %llu, state %u, %s", kind == Kind_Hello ? "hello"
             : kind == Kind_State ? "state" : kind == Kind_Ack ? "acknowledgement" : "packet of another kind",
             way, (unsigned long long)get64(data + Packet_C), get32(data + Packet_D), where);
    int at = 0;
    while (at < used && !(last[at].kind == kind && memcmp(&last[at].peer, peer, sizeof(*peer)) == 0))
        at++;
    if (at == used) {
        if (used < (int)(sizeof(last) / sizeof(last[0])))
            used++;
        else
            at = next++ % used;
        last[at].peer = *peer;
        last[at].kind = kind;
        last[at].line[0] = '\0';
    }
    if (strcmp(line, last[at].line) != 0) {
        snprintf(last[at].line, sizeof(last[at].line), "%s", line);
        printf("coop pause: %s\n", line);
    }
}

// ---- the hooks

// the mod with its plugin, whose work this is; when it goes, everything is forgotten as when the plugin unloads
static bool active(void) {
    return atomic_load_explicit(&installed, memory_order_relaxed) && aae_plugin_loaded_lately();
}

// on the game's frame: the plugin's start (its worker makes aae_coop_pause_remote with "0" if it isn't there)
static bool started;
static bool frame_active(void) {
    bool now = active();
    if (now && !started) {
        started = true;
        if (Dvar_FindByHash(gsc_hash("aae_coop_pause_remote")) == NULL)
            Dvar_SetFromStringByName("aae_coop_pause_remote", "0", true);
    } else if (!now && started) {
        started = false;
        pthread_mutex_lock(&network_lock);
        aae_coop_reset(&coop);
        pthread_mutex_unlock(&network_lock);
        if (debug)
            printf("coop pause: the mod with its plugin isn't loaded any more, everything is forgotten\n");
    }
    return now;
}

static int server_frame(int msec, bool from_com_frame);
static void client_frame(int local_client, int msec, float frac);
static bool check_paused(void);
static void tokenize(const char *text, bool from_the_client);

// SV_Frame(msec): the game's frame calls it at two places (clang's split of the plugin's one), a third caller passes 0
install_hook_name(SV_Frame, int, int msec) {
    void *from = __builtin_return_address(0);
    return server_frame(msec, from == com_frame_runs_server || from == com_frame_runs_server_late);
}
static int server_frame(int msec, bool from_com_frame) {
    if (!from_com_frame || !frame_active())
        return orig_SV_Frame(msec);
    first_call(0, "SV_Frame");
    int given = aae_coop_server_frame(&coop, &game, msec);
    report_held(0, given != msec);
    return orig_SV_Frame(given);
}

// SV_CheckPaused(): 0 not paused
install_hook_name(SV_CheckPaused, int, void) {
    return check_paused() ? orig_SV_CheckPaused() : 0;
}
static bool check_paused(void) {
    if (!active())
        return true;
    first_call(1, "SV_CheckPaused");
    return aae_coop_check_paused(&coop, &game);
}

// the client's frame (local client, msec, a fraction in xmm0)
install_hook_name(CL_Frame, void, int local_client, int msec, float frac) {
    client_frame(local_client, msec, frac);
}
static void client_frame(int local_client, int msec, float frac) {
    if (!frame_active()) {
        orig_CL_Frame(local_client, msec, frac);
        return;
    }
    aae_coop_frame_t frame;
    if (debug && atomic_load(&frame_thread) == 0)
        atomic_store(&frame_thread, thread_id());
    first_call(2, "the client frame");
    int given = msec;
    aae_coop_client_frame(&coop, &game, local_client, &given, &frac, &frame);
    report_held(1, given != msec);
    orig_CL_Frame(local_client, given, frac);
    aae_coop_client_frame_done(&coop, &game, &frame);
    if (debug)
        report();
}

// Engine.CanPauseGame's function at its first test, "mov al, 1 ; cmp dword [rcx + 0x1790], 0": with a yes, rcx is
// pointed at a 1 of ours, so the test finds a menu open and the function returns its al, 1. rcx isn't used after.
#define Can_Pause_Test 0x1790
static const int32_t say_yes = 1;
detour_with_registers(Can_Pause) {
    if (!active())
        return;
    first_call(3, "CanPauseGame");
    if (aae_coop_can_pause(&coop, &game))
        registers->rcx = (uint64_t)(uintptr_t)&say_yes - Can_Pause_Test;
}

// Cmd_TokenizeString(text): only where the client takes apart a server command
install_hook_name(Cmd_TokenizeString, void, const char *text) {
    tokenize(text, __builtin_return_address(0) == client_takes_command);
}
static const char *server_command(const char *text, bool from_the_client) {
    if (!from_the_client || !active())
        return text;
    first_call(4, "a server command");
    const char *given = aae_coop_server_command(&coop, &game, text);
    if (debug && given != text)
        printf("coop pause: the server command \"%s\" is taken as \"%s\"\n", text, given);
    return given;
}
static void tokenize(const char *text, bool from_the_client) {
    orig_Cmd_TokenizeString(server_command(text, from_the_client));
}

static int net_get_packet(aae_coop_adr_t *from, uint8_t *msg, bool from_netchan);
static void transmit(int local_client);

// NET_GetPacket(from, msg), on the network thread
install_hook_name(NET_GetPacket, int, aae_coop_adr_t *from, uint8_t *msg) {
    return net_get_packet(from, msg, __builtin_return_address(0) == netchan_receives);
}
static int net_get_packet(aae_coop_adr_t *from, uint8_t *msg, bool from_netchan) {
    if (!from_netchan || !active())
        return orig_NET_GetPacket(from, msg);
    first_call(5, "NET_GetPacket");
    uint8_t kept[Msg_Kept];
    memcpy(kept, msg, sizeof(kept));
    for (int taken = 0;;) {
        int got = orig_NET_GetPacket(from, msg);
        if (got == 0)
            return 0;
        const uint8_t *data;
        uint32_t size;
        int32_t sock;
        memcpy(&data, msg + Msg_Data, sizeof(data));
        memcpy(&size, msg + Msg_Size, sizeof(size));
        if (!aae_coop_ours(data, size))
            return got;
        memcpy(&sock, msg + Msg_Sock, sizeof(sock));
        if (debug && size == AAE_Coop_Packet_Size)
            report_packet("in", from, data);
        aae_coop_receive(&coop, &game, from, sock, data, size);
        memcpy(msg, kept, sizeof(kept));
        if (++taken >= Swallowed_Most)
            return 0;
    }
}

// the network thread's sending for a local client (it returns whether something went)
install_hook_name(Netchan_SendLocalClient, bool, int local_client) {
    bool sent = orig_Netchan_SendLocalClient(local_client);
    transmit(local_client);
    return sent;
}
static void transmit(int local_client) {
    if (local_client != 0 || !active())
        return;
    first_call(6, "the network thread's sending");
    aae_coop_outgoing_t out[AAE_Coop_Slots + 2];
    int count = aae_coop_send(&coop, &game, out);
    for (int i = 0; i < count; i++) {
        uint64_t low, high;
        memcpy(&low, out[i].to.bytes, sizeof(low));
        memcpy(&high, out[i].to.bytes + 8, sizeof(high));
        NET_SendPacket(out[i].sock, AAE_Coop_Packet_Size, out[i].data, low, high);
        if (debug)
            report_packet("out", &out[i].to, out[i].data);
    }
}

void aae_coop_pause_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    static const uint32_t places[] = { ADDR_SV_Frame, ADDR_SV_CheckPaused, ADDR_CL_Frame, ADDR_CanPauseGame_Test,
                                       ADDR_Cmd_TokenizeString, ADDR_NET_GetPacket, ADDR_Netchan_SendLocalClient };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        if (!prepatch_has_hook(places[i])) {
            printf("coop pause: not running from the pre-patched executable: All-around Enhancement's pause for "
                   "everybody in a co-op game isn't there\n");
            return;
        }
    }
    const char *env = getenv("BO3MACFIX_COOPPAUSEDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    aae_plugin_init(game_base_address);
    aae_coop_reset(&coop);
    Dvar_FindByHash = (void *)(game_base_address + ADDR_Dvar_FindByHash);
    Dvar_GetString = (void *)(game_base_address + ADDR_Dvar_GetString);
    Dvar_SetFromStringByName = (void *)(game_base_address + ADDR_Dvar_SetFromStringByName);
    NET_SendPacket = (void *)(game_base_address + ADDR_NET_SendPacket);
    CL_LocalClientSocket = (void *)(game_base_address + ADDR_Com_LocalClient_GetControllerIndex);
    dvar_cl_paused = (uint8_t *const *)(game_base_address + ADDR_dvar_cl_paused);
    dvar_sv_paused = (uint8_t *const *)(game_base_address + ADDR_dvar_sv_paused);
    dvar_max_clients = (uint8_t *const *)(game_base_address + ADDR_dvar_sv_maxclients);
    dvar_sv_running = (uint8_t *const *)(game_base_address + ADDR_sv_running);
    server_clients = (uint8_t *const *)(game_base_address + ADDR_svs_clients);
    client_connections = (uint8_t *const *)(game_base_address + ADDR_CLIENT_GLOBALS);
    connection_state = (const int32_t *)(game_base_address + ADDR_CLIENT_CONNECTIONS + 8);
    com_frame_runs_server = (const void *)(game_base_address + ADDR_Com_Frame_SV_FrameReturn);
    com_frame_runs_server_late = (const void *)(game_base_address + ADDR_Com_Frame_SV_FrameReturn2);
    netchan_receives = (const void *)(game_base_address + ADDR_Netchan_GetPacketReturn);
    client_takes_command = (const void *)(game_base_address + ADDR_CL_ServerCommand_TokenizeReturn);
    install_hook_SV_Frame((void *)(game_base_address + ADDR_SV_Frame));
    install_hook_SV_CheckPaused((void *)(game_base_address + ADDR_SV_CheckPaused));
    install_hook_CL_Frame((void *)(game_base_address + ADDR_CL_Frame));
    install_detour_Can_Pause((void *)(game_base_address + ADDR_CanPauseGame_Test));
    install_hook_Cmd_TokenizeString((void *)(game_base_address + ADDR_Cmd_TokenizeString));
    install_hook_NET_GetPacket((void *)(game_base_address + ADDR_NET_GetPacket));
    install_hook_Netchan_SendLocalClient((void *)(game_base_address + ADDR_Netchan_SendLocalClient));
    atomic_store(&installed, true);
    if (debug)
        printf("coop pause: hooks set; it acts while All-around Enhancement with its plugin is loaded\n");
}
