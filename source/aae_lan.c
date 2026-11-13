// All-around Enhancement 4's games on the local network: the plugin's lan_browser component.
//
// The mod has a server list of its own for games on the local network ("LAN"); picking one runs `connect <address>`.
// The game's own connect asks Steam about that address and joins the Steam game server it finds there. The mod's
// Windows plugin makes that join go to the host player's own Steam ID instead, which it learns by asking the host:
// a small UDP query to port 27018 that the host's plugin answers. A Mac player's partners on a local network are
// Windows players with the plugin, or Mac players with this, so the packets and the rules here are the plugin's
// exactly.
//
// The host's side, this file's first part: while the mod with its plugin is loaded, a thread keeps UDP port 27018
// open and answers the plugin's query (24 bytes, see aae_lan.h) with the player's Steam ID, the plugin's way: only
// queries of exactly that form, only from addresses of a local network, only while the game is in LAN or online
// mode, and only with a Steam ID that is a person's. Nothing else is ever sent.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "aae_lan.h"
#include "aae_plugin.h"
#include "offsets.h"
#include "steam.h"

#define Magic "AAELAN35"
#define Magic_Size 8
#define Kind_Query 1
#define Kind_Reply 2
#define Packet_Number 8 // where the asker's number is in a packet
#define Packet_Kind 12
#define Packet_Id 16

bool aae_lan_is_individual(uint64_t id) {
    return (id >> 52 & 0xf) == 1 && id >> 56 == 1 && (uint32_t)id != 0;
}

bool aae_lan_is_private(uint32_t address) {
    return address >> 24 == 10 || address >> 16 == 0xc0a8 || address >> 20 == 0xac1 || address >> 24 == 0x7f ||
           address >> 16 == 0xa9fe;
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
static void write_u32(uint8_t *bytes, uint32_t value) {
    for (int i = 0; i < 4; i++)
        bytes[i] = (uint8_t)(value >> 8 * i);
}
static void write_u64(uint8_t *bytes, uint64_t value) {
    for (int i = 0; i < 8; i++)
        bytes[i] = (uint8_t)(value >> 8 * i);
}

bool aae_lan_read_query(const uint8_t *packet, size_t length, uint32_t *number) {
    if (length != AAE_LAN_PACKET || memcmp(packet, Magic, Magic_Size) != 0 ||
        read_u32(packet + Packet_Kind) != Kind_Query)
        return false;
    *number = read_u32(packet + Packet_Number);
    return true;
}

bool aae_lan_answers(uint32_t from, int network_mode, uint64_t id) {
    return aae_lan_is_private(from) && (network_mode == 1 || network_mode == 2) && aae_lan_is_individual(id);
}

void aae_lan_reply(uint8_t packet[AAE_LAN_PACKET], uint32_t number, uint64_t id) {
    memcpy(packet, Magic, Magic_Size);
    write_u32(packet + Packet_Number, number);
    write_u32(packet + Packet_Kind, Kind_Reply);
    write_u64(packet + Packet_Id, id);
}

// ---- the host's side
//
// The plugin opens the port as soon as it knows the player's Steam ID (the game shows it that whenever it asks whether
// a player is one of this machine's, or sets up its Steam game server) and keeps it until it is unloaded with the
// mod. The Steam ID is known here from the game's start (steam.c), so the port is open while the mod with its plugin
// is loaded. When the port can't be had (another program, or a second game, has it), the plugin's thread ends and
// starts anew the next time the game shows it the Steam ID; this says so once and leaves it until the mod is loaded
// anew.

#define Listen_Wait_Ms 1000 // the plugin's receive timeout, and so how often the thread looks at the mod
#define Logged_Most 32      // the plugin logs its first 32 replies

static int debug;
static const int32_t *lobby_network_mode; // the game's: 0 offline, 1 LAN, 2 online
static uint32_t listen_address = INADDR_ANY; // host order; the tests listen on the loopback only
static uint16_t listen_port = AAE_LAN_PORT;

static uint64_t steam_local_id(void) {
    return steam_id().id;
}
static uint64_t (*local_id)(void) = steam_local_id;

static const char *address_text(uint32_t address, char text[16]) {
    snprintf(text, 16, "%u.%u.%u.%u", address >> 24, address >> 16 & 0xff, address >> 8 & 0xff, address & 0xff);
    return text;
}

// A UDP socket bound to the address and port, with the plugin's receive timeout; -1 (errno set) when it can't be had.
// Like the plugin's, it doesn't share the port: a second one can't have it.
static int listener_open(uint32_t address, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return -1;
    struct timeval wait = { .tv_sec = Listen_Wait_Ms / 1000, .tv_usec = Listen_Wait_Ms % 1000 * 1000 };
    struct sockaddr_in at = { .sin_len = sizeof(at), .sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = htonl(address) };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait)) != 0 ||
        bind(fd, (struct sockaddr *)&at, sizeof(at)) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

// One datagram (or the receive timeout): a query the plugin would answer gets the plugin's reply.
static unsigned replies_logged, refusals_logged;
static void listener_serve(int fd) {
    uint8_t packet[512]; // more than a query: a longer datagram must not look like one (the plugin's Windows fails it)
    struct sockaddr_in from;
    socklen_t from_size = sizeof(from);
    ssize_t length = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &from_size);
    if (length < 0 || from_size != sizeof(from) || from.sin_family != AF_INET)
        return;
    uint32_t address = ntohl(from.sin_addr.s_addr), number;
    char text[16];
    if (!aae_lan_read_query(packet, (size_t)length, &number)) {
        if (debug && refusals_logged++ < Logged_Most)
            printf("lan join: ignored %zd bytes from %s:%u: not the plugin's query\n", length,
                   address_text(address, text), ntohs(from.sin_port));
        return;
    }
    uint64_t id = local_id();
    int mode = *lobby_network_mode;
    if (!aae_lan_answers(address, mode, id)) {
        if (debug && refusals_logged++ < Logged_Most)
            printf("lan join: not answering the query from %s:%u: %s\n", address_text(address, text),
                   ntohs(from.sin_port),
                   !aae_lan_is_private(address)    ? "not an address of a local network"
                   : mode != 1 && mode != 2        ? "the game is in neither LAN nor online mode"
                                                   : "no Steam ID of a person to give");
        return;
    }
    uint8_t reply[AAE_LAN_PACKET];
    aae_lan_reply(reply, number, id);
    ssize_t sent = sendto(fd, reply, sizeof(reply), 0, (struct sockaddr *)&from, from_size);
    if (debug && replies_logged++ < Logged_Most)
        printf("lan join: answered the query %08x from %s:%u with Steam ID %llu%s (network mode %d)\n", number,
               address_text(address, text), ntohs(from.sin_port), (unsigned long long)id,
               sent == (ssize_t)sizeof(reply) ? "" : " (not sent)", mode);
}

// The listener's state and one turn of its loop: true when the thread is to wait a while before the next.
typedef struct {
    int fd;       // the socket while it is open, -1 otherwise
    bool gave_up; // the port couldn't be had since the mod was loaded
} listener_t;
static bool listener_turn(listener_t *listener) {
    bool loaded = aae_plugin_loaded_lately(), person = aae_lan_is_individual(local_id());
    if (!loaded || !person) {
        if (listener->fd >= 0) {
            close(listener->fd);
            listener->fd = -1;
            printf("lan join: stopped answering on UDP port %u: %s\n", listen_port,
                   !loaded ? "the mod isn't loaded any more" : "no Steam ID of a person any more");
        }
        listener->gave_up = false;
        return true;
    }
    if (listener->fd < 0) {
        if (listener->gave_up)
            return true;
        listener->fd = listener_open(listen_address, listen_port);
        if (listener->fd < 0) {
            listener->gave_up = true;
            printf("lan join: UDP port %u can't be had (%s): players on the local network can't learn this Mac's "
                   "Steam ID, so their plugin can't join its games\n", listen_port, strerror(errno));
            return true;
        }
        printf("lan join: answering the mod's queries for the host's Steam ID on UDP port %u\n", listen_port);
    }
    listener_serve(listener->fd);
    return false;
}

static void *listener_main(void *unused) {
    (void)unused;
    pthread_setname_np("BO3MacFix LAN join");
    listener_t listener = { .fd = -1 };
    for (;;) {
        if (listener_turn(&listener))
            usleep(Listen_Wait_Ms * 1000);
    }
    return NULL;
}

// how the threads are started (the tests start none)
static int (*start_thread)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = pthread_create;

void aae_lan_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    const char *env = getenv("BO3MACFIX_LANDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    aae_plugin_init(game_base_address);
    lobby_network_mode = (const int32_t *)(game_base_address + ADDR_lobbyNetworkMode);
    pthread_t thread;
    if (start_thread(&thread, NULL, listener_main, NULL) == 0)
        pthread_detach(thread);
    else
        printf("lan join: no thread for answering the mod's queries\n");
}
