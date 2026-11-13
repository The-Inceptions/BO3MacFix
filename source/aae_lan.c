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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "aae_lan.h"
#include "aae_plugin.h"
#include "detour.h"
#include "dobby.h"
#include "gsc_obj.h"
#include "offsets.h"
#include "prepatch.h"
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

// ---- the joiner's side: the rules

#define Number_Constant 0xaabbef35u // the plugin's, in the number of a query
#define Window_Ms 90000             // how long after a connect begins its ping and Join count
#define Target_Ms 10000             // how long after it begins its target can be found
#define Answer_Kept_Ms 5000         // how long a host's Steam ID is kept
#define No_Answer_Kept_Ms 600       // and that a host didn't answer
#define Join_Direct 4               // the join type for a direct join to a host
#define Account_Game_Server 3       // Steam account types: a game server, and (4) an anonymous one
#define Address_Most 95             // the most characters of the address before "@" the plugin keeps

void aae_lan_query(uint8_t packet[AAE_LAN_PACKET], uint32_t number) {
    memset(packet, 0, AAE_LAN_PACKET);
    memcpy(packet, Magic, Magic_Size);
    write_u32(packet + Packet_Number, number);
    write_u32(packet + Packet_Kind, Kind_Query);
}

uint32_t aae_lan_number(uint64_t now, uint32_t attempt) {
    return (uint32_t)now ^ attempt << 16 ^ Number_Constant;
}

bool aae_lan_read_reply(const uint8_t *packet, size_t length, uint32_t from, uint16_t from_port, uint32_t asked,
                        uint16_t asked_port, uint32_t number, uint64_t *id) {
    if (length != AAE_LAN_PACKET || from != asked || from_port != asked_port ||
        memcmp(packet, Magic, Magic_Size) != 0 || read_u32(packet + Packet_Kind) != Kind_Reply ||
        read_u32(packet + Packet_Number) != number)
        return false;
    uint64_t steam_id = (uint64_t)read_u32(packet + Packet_Id) | (uint64_t)read_u32(packet + Packet_Id + 4) << 32;
    if (!aae_lan_is_individual(steam_id))
        return false;
    *id = steam_id;
    return true;
}

// The plugin's reading: digits in the base until the text ends; anything else, or more than 64 bits, and it isn't a
// number.
bool aae_lan_split(const char *text, char address[AAE_LAN_ADDRESS], uint64_t *id) {
    size_t at = 0;
    while (text[at] != '\0' && text[at] != '@' && at < Address_Most)
        at++;
    if (text[at] != '@' || text[at + 1] == '\0')
        return false;
    const char *digits = text + at + 1;
    unsigned base = 10;
    if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits += 2;
        if (*digits == '\0')
            return false;
        base = 16;
    }
    uint64_t value = 0;
    for (; *digits != '\0'; digits++) {
        unsigned char c = (unsigned char)*digits;
        unsigned digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return false;
        if (value > (UINT64_MAX - digit) / base)
            return false;
        value = value * base + digit;
    }
    if (!aae_lan_is_individual(value))
        return false;
    memset(address, 0, AAE_LAN_ADDRESS);
    memcpy(address, text, at);
    *id = value;
    return true;
}

void aae_lan_connect(aae_lan_join_t *join, uint64_t now) {
    join->attempt++;
    join->matched = false;
    join->target = 0;
    join->pinged = 0;
    join->given_id = 0;
    join->started = now;
}

// the connect began, and less than 90 s ago (the plugin's ping asks for a count of connects too, its Join doesn't)
static bool in_window(const aae_lan_join_t *join, uint64_t now) {
    return join->started != 0 && now >= join->started && now - join->started < Window_Ms;
}

bool aae_lan_cached(const aae_lan_join_t *join, uint32_t address, uint64_t now, uint64_t *id) {
    for (int i = 0; i < AAE_LAN_CACHED; i++) {
        if (join->cached[i].address == address && now < join->cached[i].expires) {
            *id = join->cached[i].id;
            return true;
        }
    }
    return false;
}

// The plugin takes the first entry that is this address's or has expired, in the order they are kept; with none such,
// the first of those that expire first.
void aae_lan_cache(aae_lan_join_t *join, uint32_t address, uint64_t id, uint64_t now) {
    aae_lan_cached_t *entry = NULL, *first_to_expire = &join->cached[0];
    for (int i = 0; i < AAE_LAN_CACHED && entry == NULL; i++) {
        aae_lan_cached_t *this = &join->cached[i];
        if (this->address == address || this->expires <= now)
            entry = this;
        else if (this->expires < first_to_expire->expires)
            first_to_expire = this;
    }
    if (entry == NULL)
        entry = first_to_expire;
    entry->address = address;
    entry->id = id;
    entry->expires = now + (id != 0 ? Answer_Kept_Ms : No_Answer_Kept_Ms);
}

int aae_lan_ask(aae_lan_join_t *join, uint32_t address, uint32_t attempt, uint64_t now) {
    uint64_t id;
    if (address == 0 || !aae_lan_is_private(address))
        return 0;
    if (aae_lan_cached(join, address, now, &id) || join->asking == address || join->requested == address)
        return 1;
    join->requested = address;
    join->requested_attempt = attempt;
    return 2;
}

bool aae_lan_ping(aae_lan_join_t *join, uint32_t address, uint64_t now) {
    bool matched = false;
    if (join->attempt != 0 && in_window(join, now)) {
        if (join->target == 0 && now - join->started < Target_Ms && aae_lan_is_private(address))
            join->target = address;
        matched = join->target != 0 && join->target == address;
    }
    join->pinged = address;
    join->matched = matched;
    if (!matched || aae_lan_is_individual(join->given_id))
        return false;
    return aae_lan_ask(join, address, join->attempt, now) == 2;
}

aae_lan_plan_t aae_lan_plan(aae_lan_join_t *join, uint64_t now, int network_mode, uint64_t server, int join_type) {
    aae_lan_plan_t plan = { .join_type = join_type, .attempt = join->attempt, .target = join->target };
    bool matched = join->matched;
    join->matched = false;
    plan.window = matched && in_window(join, now) && join->target != 0 && join->pinged == join->target;
    plan.swap = plan.window && (network_mode == 1 || network_mode == 2) && aae_lan_is_private(join->pinged);
    if (!plan.swap)
        return plan;
    if (join_type == 0)
        plan.join_type = Join_Direct;
    if (aae_lan_is_individual(join->given_id))
        plan.id = join->given_id;
    else
        plan.wait = (unsigned)(server >> 52 & 0xf) - Account_Game_Server <= 1;
    return plan;
}

// the connect and its target are still the ones the Join was planned for
static bool unchanged(const aae_lan_join_t *join, const aae_lan_plan_t *plan) {
    return join->attempt == plan->attempt && join->target == plan->target;
}

bool aae_lan_waited(const aae_lan_join_t *join, const aae_lan_plan_t *plan, uint64_t now, uint64_t *id) {
    return !unchanged(join, plan) || aae_lan_cached(join, plan->target, now, id);
}

bool aae_lan_dropped(const aae_lan_join_t *join, const aae_lan_plan_t *plan) {
    return plan->window && !unchanged(join, plan);
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

// how the threads are started (the tests start their own)
static int (*start_thread)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = pthread_create;

// ---- the joiner's side: the game's connect, and asking the host
//
// The plugin replaces four calls in the game: the connect command's first (a connect begins), the reading of its
// address (a Steam ID after "@" is taken off), the ping of the server at that address (the host is asked for its
// Steam ID), and the Join the ping's answer makes (the host's Steam ID in place of the game server's, and a direct
// join). Here the functions those calls go to are hooked, the Join only for the call the ping's answer makes, as on
// Windows. All four run on the game's main thread. The host is asked by a thread of its own, started the first time
// one is to be asked, as the plugin's is; a Join waits for its answer at most 350 ms.

#define Tries 2             // queries to the host
#define Reply_Wait_Ms 140   // how long each waits for the reply
#define Answer_Wait_Ms 350  // how long the Join waits for the answer

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t asker_wakes = PTHREAD_COND_INITIALIZER, answered = PTHREAD_COND_INITIALIZER;
static aae_lan_join_t join; // under lock, and so is this:
static bool asker_started;
static uint16_t query_port = AAE_LAN_PORT; // the tests ask a host of their own on another port
static const void *join_from_ping;         // where the ping's answer goes on after its Join
// what the debug line of a connect shows of the game
static const int32_t *lobby_ui_screen;
static int (*PlaylistVersion_Get)(void), (*FFOTDVersion_Get)(void);
static const uint64_t *const *local_users;

static uint64_t uptime_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}
static uint64_t (*now_ms)(void) = uptime_ms;

// The plugin's questioning of a host: one socket, the query sent up to twice, each time waiting 140 ms for one
// datagram, which counts only when it is the reply from that host. The Steam ID it gave, or 0.
static uint64_t ask_host(uint32_t address, uint32_t attempt, int *tries) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return 0;
    struct timeval wait = { .tv_sec = 0, .tv_usec = Reply_Wait_Ms * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET, .sin_port = htons(query_port),
                              .sin_addr.s_addr = htonl(address) };
    uint32_t number = aae_lan_number(now_ms(), attempt);
    uint8_t query[AAE_LAN_PACKET];
    aae_lan_query(query, number);
    uint64_t id = 0;
    while (id == 0 && *tries < Tries) {
        ++*tries;
        if (sendto(fd, query, sizeof(query), 0, (struct sockaddr *)&to, sizeof(to)) != (ssize_t)sizeof(query))
            continue;
        uint8_t packet[512]; // more than a reply: a longer datagram must not look like one
        struct sockaddr_in from;
        socklen_t from_size = sizeof(from);
        ssize_t length = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&from, &from_size);
        if (length >= 0 && from_size == sizeof(from) && from.sin_family == AF_INET)
            aae_lan_read_reply(packet, (size_t)length, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), address,
                               query_port, number, &id);
    }
    close(fd);
    return id;
}

// The asking thread: takes the address last requested, asks it unless its answer is kept, and keeps the answer (none
// too) for the Joins waiting on it.
static void *asker_main(void *unused) {
    (void)unused;
    pthread_setname_np("BO3MacFix LAN join query");
    pthread_mutex_lock(&lock);
    for (;;) {
        while (join.requested == 0)
            pthread_cond_wait(&asker_wakes, &lock);
        uint32_t address = join.requested, attempt = join.requested_attempt;
        uint64_t id = 0;
        join.requested = 0;
        join.asking = address;
        bool kept = aae_lan_cached(&join, address, now_ms(), &id);
        pthread_mutex_unlock(&lock);
        if (aae_lan_is_private(address) && !kept) {
            int tries = 0;
            id = ask_host(address, attempt, &tries);
            pthread_mutex_lock(&lock);
            aae_lan_cache(&join, address, id, now_ms());
            pthread_cond_broadcast(&answered);
            pthread_mutex_unlock(&lock);
            char text[16];
            if (id != 0)
                printf("lan join: the host at %s says its Steam ID is %llu (connect %u, %d %s)\n",
                       address_text(address, text), (unsigned long long)id, attempt, tries,
                       tries == 1 ? "query" : "queries");
            else
                printf("lan join: the host at %s didn't give its Steam ID (connect %u, %d queries): it has neither the "
                       "mod's plugin nor this, or isn't in a LAN or online game\n", address_text(address, text),
                       attempt, tries);
        }
        pthread_mutex_lock(&lock);
        join.asking = 0;
    }
    return NULL;
}

// Sends the asking thread to work, starting it the first time. False when it can't be started. Under the lock.
static bool wake_asker(void) {
    if (!asker_started) {
        pthread_t thread;
        if (start_thread(&thread, NULL, asker_main, NULL) != 0)
            return false;
        pthread_detach(thread);
        asker_started = true;
    }
    pthread_cond_signal(&asker_wakes);
    return true;
}

// The Join's wait for the host's answer, under the lock (which the waiting lets go of): the Steam ID the host gave,
// 0 for none. Ends early when another connect begins, as the plugin's does.
static uint64_t wait_for_answer(const aae_lan_plan_t *plan) {
    uint64_t id = 0, now = now_ms();
    if (!unchanged(&join, plan))
        return 0;
    if (aae_lan_cached(&join, plan->target, now, &id))
        return id;
    int asked = aae_lan_ask(&join, plan->target, plan->attempt, now);
    if (asked == 2 && !wake_asker()) {
        join.requested = 0;
        asked = 0;
    }
    if (asked == 0)
        return 0;
    uint64_t until = now + Answer_Wait_Ms;
    // the plugin's wait: until it's over, looking once more when the time is up
    while (!aae_lan_waited(&join, plan, now = now_ms(), &id) && now < until) {
        uint64_t left = until - now;
        struct timespec span = { .tv_sec = (time_t)(left / 1000), .tv_nsec = (long)(left % 1000) * 1000000 };
        pthread_cond_timedwait_relative_np(&answered, &lock, &span);
    }
    return unchanged(&join, plan) ? id : 0;
}

// a connect command: (), what it reads it takes from the command's arguments
install_hook_name(LanJoin_Connect, void, void) {
    if (aae_plugin_loaded_lately()) {
        pthread_mutex_lock(&lock);
        aae_lan_connect(&join, now_ms());
        uint32_t attempt = join.attempt;
        pthread_cond_broadcast(&answered);
        pthread_mutex_unlock(&lock);
        if (debug)
            printf("lan join: connect %u begins (network mode %d, lobby UI screen %d, playlist version %d, ffotd "
                   "version %d; the game's local user %llu, Steam's %llu)\n", attempt, *lobby_network_mode,
                   *lobby_ui_screen, PlaylistVersion_Get(), FFOTDVersion_Get(),
                   local_users[0] != NULL ? (unsigned long long)*local_users[0] : 0ull,
                   (unsigned long long)local_id());
    }
    orig_LanJoin_Connect();
}

// the connect's address read into the game's: (text, the network address it fills), true when it could be read
install_hook_name(LanJoin_ParseAddress, uint64_t, const char *text, void *address) {
    char cut[AAE_LAN_ADDRESS];
    uint64_t id;
    if (text == NULL || !aae_plugin_loaded_lately() || !aae_lan_split(text, cut, &id))
        return orig_LanJoin_ParseAddress(text, address);
    pthread_mutex_lock(&lock);
    join.given_id = id;
    pthread_mutex_unlock(&lock);
    if (debug)
        printf("lan join: the host's Steam ID %llu is given with the address %s\n", (unsigned long long)id, cut);
    return orig_LanJoin_ParseAddress(cut, address);
}

// Steam is asked about the server at the connect's address: (its IPv4 address, its first number high; its port)
install_hook_name(LanJoin_PingServer, uint64_t, uint64_t ip, uint64_t port) {
    if (aae_plugin_loaded_lately()) {
        uint32_t address = (uint32_t)ip;
        pthread_mutex_lock(&lock);
        bool ask = aae_lan_ping(&join, address, now_ms());
        if (ask && !wake_asker())
            join.requested = 0;
        bool target = join.matched;
        uint32_t attempt = join.attempt;
        pthread_mutex_unlock(&lock);
        char text[16];
        if (debug)
            printf("lan join: connect %u pings %s:%u: %s\n", attempt, address_text(address, text),
                   (unsigned)(uint16_t)port, !target ? "not its target on a local network"
                                             : ask   ? "its target; asking the host for its Steam ID"
                                                     : "its target");
    }
    return orig_LanJoin_PingServer(ip, port);
}

// the lobby VM's Lua call "Join": (controller, Steam ID, join type), which is hooked below
static uint64_t (*orig_LanJoin_Join)(uint64_t controller, uint64_t server, uint64_t join_type);

// What the Join to the server the ping found becomes. from_ping: the ping's answer is the one asking, the only Join
// the plugin changes.
static uint64_t join_server(uint64_t controller, uint64_t server, uint64_t join_type, bool from_ping) {
    if (!from_ping || !aae_plugin_loaded_lately())
        return orig_LanJoin_Join(controller, server, join_type);
    pthread_mutex_lock(&lock);
    aae_lan_plan_t plan = aae_lan_plan(&join, now_ms(), *lobby_network_mode, server, (int)join_type);
    uint64_t id = plan.wait ? wait_for_answer(&plan) : plan.id;
    bool dropped = aae_lan_dropped(&join, &plan);
    if (!dropped)
        join.given_id = 0;
    pthread_mutex_unlock(&lock);
    char text[16];
    address_text(plan.target, text);
    if (dropped) {
        printf("lan join: another connect began while the host at %s was asked: this one's join is dropped\n", text);
        return 0;
    }
    if (plan.swap && id != 0)
        printf("lan join: joining the host at %s by its own Steam ID %llu instead of the server's %llu, join type "
               "%d\n", text, (unsigned long long)id, (unsigned long long)server, plan.join_type);
    else if (plan.swap)
        printf("lan join: no Steam ID from the host at %s: joining the server's %llu with join type %d\n", text,
               (unsigned long long)server, plan.join_type);
    else if (debug)
        printf("lan join: the join to %llu goes as the game made it\n", (unsigned long long)server);
    // what the game gave is handed on as it came where the plugin doesn't change it
    return orig_LanJoin_Join(controller, id != 0 ? id : server,
                             plan.join_type != (int)join_type ? (uint64_t)plan.join_type : join_type);
}

static uint64_t fake_LanJoin_Join(uint64_t controller, uint64_t server, uint64_t join_type) {
    return join_server(controller, server, join_type, __builtin_return_address(0) == join_from_ping);
}

// ---- lobby messages: through the player's own Steam networking first
//
// The game sends a lobby message to a player (SendLobbyPacket: an int 0x28, then '1', the message's type and data,
// 0x408 bytes in all) through its Steam game server's networking, and through the player's own only when that fails.
// A joiner who joined the host player's own Steam ID (above) knows the host by that ID, so for messages to a person
// the plugin turns that round: the player's own networking first, the game server's when it fails.

#define Lobby_Packet_Mark 0x28
#define Lobby_Message '1'
#define Lobby_Packet_Least 8

bool aae_lan_lobby_message(uint64_t to, const uint8_t *packet, uint32_t length) {
    return aae_lan_is_individual(to) && packet != NULL && length >= Lobby_Packet_Least &&
           read_u32(packet) == Lobby_Packet_Mark && packet[4] == Lobby_Message;
}

// the player's own Steam networking: (Steam ID, packet, length), its answer (true when sent) in al
static uint64_t (*UserNetworking_Send)(uint64_t to, const uint8_t *packet, uint64_t length);

// the game server's: the same
install_hook_name(LanJoin_SendLobbyPacket, uint64_t, uint64_t to, const uint8_t *packet, uint64_t length) {
    if (aae_plugin_loaded_lately() && aae_lan_lobby_message(to, packet, (uint32_t)length) &&
        (uint8_t)UserNetworking_Send(to, packet, length) != 0) {
        static _Atomic bool said;
        if (debug && !atomic_exchange(&said, true))
            printf("lan join: lobby messages to players go through the player's own Steam networking first (the "
                   "first to %llu)\n", (unsigned long long)to);
        return 1;
    }
    return orig_LanJoin_SendLobbyPacket(to, packet, length);
}

// ---- the host's join checks
//
// The host's join handler (0x1012334e4) turns a joiner away for many reasons; the plugin changes two of its checks.
// In LAN mode the joiner has to be in LAN mode too: the plugin lets a joiner in online mode count as one in LAN mode
// when it joins directly (join type 4, as its joiners do) and its dvar aae_offlinejoin is on at the host. In online
// mode the joiner's playlist and ffotd versions have to be the host's: the plugin makes both checks pass (the game's
// two getters answer with the joiner's own versions there). Both only from the pre-patched executable (detour.h).
//
// aae_offlinejoin: the plugin makes it, as text "0", for the player to set. Without the plugin there is none until
// somebody sets it (then it is text too), and none counts as the plugin's "0".

#define Dvar_Type 0x1c  // the dvar's kind, an int
#define Dvar_Value 0x28 // its value: a bool's is a byte, an int's or an enum's an int
enum { Dvar_Bool = 1, Dvar_Int = 6, Dvar_Enum = 7, Dvar_String = 8 };
#define Frame_JoinType (-0x340) // the join handler's join type of the joiner, from its frame pointer

bool aae_lan_switch_on(int type, const char *text, int32_t value) {
    if (type == Dvar_String)
        return text != NULL && strcmp(text, "1") == 0;
    return (type == Dvar_Bool || type == Dvar_Int || type == Dvar_Enum) && value == 1;
}

bool aae_lan_offline_join(int joiner_mode, int join_type, int host_mode, bool switched_on) {
    return joiner_mode == 2 && join_type == Join_Direct && host_mode == 1 && switched_on;
}

static void *(*Dvar_FindByHash)(uint32_t hash);
static const char *(*Dvar_GetString)(void *dvar);

static bool offline_join_on(void) {
    const uint8_t *dvar = Dvar_FindByHash(gsc_hash("aae_offlinejoin"));
    if (dvar == NULL)
        return false;
    int32_t type, value = 0;
    memcpy(&type, dvar + Dvar_Type, sizeof(type));
    if (type == Dvar_Bool)
        value = dvar[Dvar_Value];
    else
        memcpy(&value, dvar + Dvar_Value, sizeof(value));
    return aae_lan_switch_on(type, type == Dvar_String ? Dvar_GetString((void *)dvar) : NULL, value);
}

// The LAN branch's "cmp ecx, 1", the joiner's network mode in ecx and the host's (1 on this branch) in eax. That
// compare is the first of the instructions this place's stub runs after the handler, so the jne behind it reads flags
// set from the handler's ecx.
detour_with_registers(LanJoin_JoinerNetworkMode) {
    int32_t join_type;
    memcpy(&join_type, (const uint8_t *)registers->rbp + Frame_JoinType, sizeof(join_type));
    if (!aae_lan_offline_join((int32_t)registers->rcx, join_type, (int32_t)registers->rax, true) ||
        !aae_plugin_loaded_lately() || !offline_join_on())
        return;
    registers->rcx = 1;
    printf("lan join: a joiner in online mode joining directly counts as one in LAN mode (aae_offlinejoin is 1)\n");
}

// The online branch's two checks, each turning the joiner away at the place: there the joiner's version is in ebx and
// the host's in eax, and the next instruction loads r15 anew, followed by a call, so nothing is live but what the
// check behind it loads again. Going on with that check instead is the game's own way when the versions are the same.
static bool versions_pass(const char *which, uint64_t joiner, uint64_t host) {
    if (!aae_plugin_loaded_lately())
        return false;
    printf("lan join: a joiner with %s version %d (this game's %d) isn't turned away for it\n", which, (int32_t)joiner,
           (int32_t)host);
    return true;
}
detour_choice(LanJoin_PlaylistVersion, "mov %rbx, %rdi\n mov %rax, %rsi\n") {
    return versions_pass("playlist", first, second);
}
detour_choice(LanJoin_FFOTDVersion, "mov %rbx, %rdi\n mov %rax, %rsi\n") {
    return versions_pass("ffotd", first, second);
}

void aae_lan_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    const char *env = getenv("BO3MACFIX_LANDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    aae_plugin_init(game_base_address);
    lobby_network_mode = (const int32_t *)(game_base_address + ADDR_lobbyNetworkMode);
    join_from_ping = (const void *)(game_base_address + ADDR_LanJoin_JoinFromPing);
    lobby_ui_screen = (const int32_t *)(game_base_address + ADDR_lobbyUIScreen);
    PlaylistVersion_Get = (int (*)(void))(game_base_address + ADDR_PlaylistVersion_Get);
    FFOTDVersion_Get = (int (*)(void))(game_base_address + ADDR_FFOTDVersion_Get);
    local_users = (const uint64_t *const *)(game_base_address + ADDR_localUsers);
    install_hook_LanJoin_Connect((void *)(game_base_address + ADDR_LanJoin_Connect));
    install_hook_LanJoin_ParseAddress((void *)(game_base_address + ADDR_LanJoin_ParseAddress));
    install_hook_LanJoin_PingServer((void *)(game_base_address + ADDR_LanJoin_PingServer));
    DobbyHook((void *)(game_base_address + ADDR_LanJoin_Join), (void *)fake_LanJoin_Join, (void **)&orig_LanJoin_Join);
    UserNetworking_Send =
        (uint64_t (*)(uint64_t, const uint8_t *, uint64_t))(game_base_address + ADDR_LanJoin_UserSend);
    install_hook_LanJoin_SendLobbyPacket((void *)(game_base_address + ADDR_LanJoin_SendLobbyPacket));
    Dvar_FindByHash = (void *(*)(uint32_t))(game_base_address + ADDR_Dvar_FindByHash);
    Dvar_GetString = (const char *(*)(void *))(game_base_address + ADDR_Dvar_GetString);
    if (!prepatch_has_hook(ADDR_LanJoin_JoinerNetworkMode) || !prepatch_has_hook(ADDR_LanJoin_PlaylistRejected) ||
        !prepatch_has_hook(ADDR_LanJoin_FFOTDRejected)) {
        printf("lan join: not running from the pre-patched executable: the host's join checks are the game's own\n");
        return;
    }
    install_detour_LanJoin_JoinerNetworkMode((void *)(game_base_address + ADDR_LanJoin_JoinerNetworkMode));
    install_detour_LanJoin_PlaylistVersion((void *)(game_base_address + ADDR_LanJoin_PlaylistRejected),
                                           (void *)(game_base_address + ADDR_LanJoin_PlaylistChecked));
    install_detour_LanJoin_FFOTDVersion((void *)(game_base_address + ADDR_LanJoin_FFOTDRejected),
                                        (void *)(game_base_address + ADDR_LanJoin_FFOTDChecked));
    pthread_t thread;
    if (start_thread(&thread, NULL, listener_main, NULL) == 0)
        pthread_detach(thread);
    else
        printf("lan join: no thread for answering the mod's queries\n");
}
