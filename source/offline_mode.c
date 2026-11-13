// OfflineMode: the game's process without the internet (see offline_mode.h for what it is for).
//
// What the game does without its online service, read in the Mac game and the mod's Lua (wip/night/impl-offline.md):
// Return at All-around Enhancement's start screen runs the stock CoD.LobbyBase.BeginPlay, which goes to LAN play when
// the player isn't signed in to Demonware (ShouldBeginLAN) and to LOCAL play when Steam is offline
// (Engine.CheckNetConnection is SteamUser()->BLoggedOn()); only a player who is signed in goes to LIVE, where the
// "Connecting to Online Service" overlay waits up to 30 s for Demonware's fetching and then shows an error that needs a
// button. The mod's ModeSelect then goes to its online lobby only in LIVE. So a game that can't reach Demonware takes
// the LAN path at once, by the game's own rules; this file makes sure it can't reach it, and can't wait on it either:
// no lookup or connection of a name or address outside this Mac and its local network leaves the process.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <os/lock.h>
#include <poll.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "dyld-interposing.h"
#include "offline_mode.h"

#pragma mark - which addresses and names are local

// in host order
static bool ipv4_is_local(uint32_t address) {
    uint32_t first = address >> 24;
    // "this network" 0/8 (0.0.0.0 among it), the private 10/8, loopback 127/8
    if (first == 0 || first == 10 || first == 127)
        return true;
    // the private 172.16/12 and 192.168/16, link-local 169.254/16
    if ((address & 0xfff00000u) == 0xac100000u || (address & 0xffff0000u) == 0xc0a80000u ||
        (address & 0xffff0000u) == 0xa9fe0000u)
        return true;
    // multicast 224/4 (LAN discovery), and the broadcast to the local network
    return (address & 0xf0000000u) == 0xe0000000u || address == 0xffffffffu;
}

static bool ipv6_is_local(const uint8_t *bytes) {
    static const uint8_t zeros[16] = { 0 };
    // :: and ::1
    if (memcmp(bytes, zeros, 15) == 0 && bytes[15] <= 1)
        return true;
    // link-local fe80::/10, unique local fc00::/7, multicast ff00::/8
    if ((bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) || (bytes[0] & 0xfe) == 0xfc || bytes[0] == 0xff)
        return true;
    // an IPv4 address mapped into IPv6, ::ffff:a.b.c.d
    if (memcmp(bytes, zeros, 10) == 0 && bytes[10] == 0xff && bytes[11] == 0xff)
        return ipv4_is_local((uint32_t)bytes[12] << 24 | (uint32_t)bytes[13] << 16 | (uint32_t)bytes[14] << 8 |
                             bytes[15]);
    return false;
}

bool offline_address_is_local(const struct sockaddr *address, socklen_t length) {
    if (address == NULL || length < (socklen_t)offsetof(struct sockaddr, sa_data))
        return true;
    if (address->sa_family == AF_INET) {
        if (length < (socklen_t)sizeof(struct sockaddr_in))
            return true;
        struct sockaddr_in v4;
        memcpy(&v4, address, sizeof(v4));
        return ipv4_is_local(ntohl(v4.sin_addr.s_addr));
    }
    if (address->sa_family == AF_INET6) {
        if (length < (socklen_t)sizeof(struct sockaddr_in6))
            return true;
        struct sockaddr_in6 v6;
        memcpy(&v6, address, sizeof(v6));
        return ipv6_is_local(v6.sin6_addr.s6_addr);
    }
    // Unix sockets (the system's own services) and anything else that isn't the internet
    return true;
}

// a name that is an address written out, which getaddrinfo reads without asking anybody
static bool is_numeric_name(const char *name) {
    struct in_addr v4;
    if (strspn(name, "0123456789abcdefABCDEFxX.") == strlen(name) && inet_aton(name, &v4) == 1)
        return true;
    // an IPv6 address, maybe with a scope that names an interface ("fe80::1%en0")
    char copy[INET6_ADDRSTRLEN + 1];
    size_t length = strcspn(name, "%");
    const char *scope = name[length] == '%' ? name + length + 1 : NULL;
    if (length >= sizeof(copy) ||
        (scope != NULL && (scope[0] == '\0' ||
                           strspn(scope, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") !=
                               strlen(scope))))
        return false;
    memcpy(copy, name, length);
    copy[length] = '\0';
    struct in6_addr v6;
    return inet_pton(AF_INET6, copy, &v6) == 1;
}

// whether the first length characters of name end in suffix, whatever the case
static bool ends_with(const char *name, size_t length, const char *suffix) {
    size_t suffix_length = strlen(suffix);
    return length >= suffix_length && strncasecmp(name + length - suffix_length, suffix, suffix_length) == 0;
}

bool offline_name_is_local(const char *name, const char *own_host_name) {
    if (name == NULL || name[0] == '\0' || is_numeric_name(name))
        return true;
    // a name written as absolute ("localhost.") is the same name
    size_t length = strlen(name);
    if (name[length - 1] == '.')
        length--;
    if ((length == 9 && strncasecmp(name, "localhost", 9) == 0) || ends_with(name, length, ".localhost") ||
        ends_with(name, length, ".local"))
        return true;
    // the game asks for its own host name ("Hostname: ..." in its console)
    if (own_host_name == NULL || own_host_name[0] == '\0')
        return false;
    size_t own_length = strlen(own_host_name);
    if (own_host_name[own_length - 1] == '.')
        own_length--;
    return length == own_length && strncasecmp(name, own_host_name, length) == 0;
}

uint64_t offline_now_ms(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
}

static struct timespec timespec_of_ms(uint64_t ms) {
    struct timespec span = { .tv_sec = (time_t)(ms / 1000), .tv_nsec = (long)(ms % 1000) * 1000000 };
    return span;
}

#pragma mark - the automatic test

static int system_resolve(const char *host, struct sockaddr_in *address) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *found = NULL;
    int code = getaddrinfo(host, NULL, &hints, &found);
    if (code != 0)
        return code;
    // with AF_INET asked for every answer is an IPv4 address; the game too takes the first one (a count of 1 at
    // 0x1011a4e13)
    memcpy(address, found->ai_addr, sizeof(*address));
    freeaddrinfo(found);
    return 0;
}

static int system_connect_within(const struct sockaddr_in *address, int timeout_ms) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return errno;
    int on = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    int result = 0;
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        result = errno;
    } else if (connect(socket_fd, (const struct sockaddr *)address, sizeof(*address)) != 0) {
        if (errno != EINPROGRESS) {
            result = errno;
        } else {
            uint64_t give_up = offline_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
            struct pollfd wanted = { .fd = socket_fd, .events = POLLOUT };
            int ready;
            for (;;) {
                uint64_t now = offline_now_ms();
                ready = poll(&wanted, 1, now < give_up ? (int)(give_up - now) : 0);
                if (ready >= 0 || errno != EINTR)
                    break;
            }
            if (ready == 0) {
                result = ETIMEDOUT;
            } else if (ready < 0) {
                result = errno;
            } else {
                int error = 0;
                socklen_t size = sizeof(error);
                result = getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 ? errno : error;
            }
        }
    }
    close(socket_fd);
    return result;
}

const offline_probe_ops_t offline_probe_system_ops = { system_resolve, system_connect_within };

// a name lookup in a thread of its own: it can't be stopped, so when it doesn't answer in time the test goes on
// without it and the thread frees this when it is done
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t done_signal;
    bool done, left_behind;
    int code;
    struct sockaddr_in address;
    int (*resolve)(const char *host, struct sockaddr_in *address);
    char host[256];
} lookup_t;

_Atomic int offline_lookups_alive = 0;

static void free_lookup(lookup_t *lookup) {
    pthread_cond_destroy(&lookup->done_signal);
    pthread_mutex_destroy(&lookup->lock);
    free(lookup);
    atomic_fetch_sub(&offline_lookups_alive, 1);
}

static void *lookup_thread(void *argument) {
    lookup_t *lookup = argument;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    int code = lookup->resolve(lookup->host, &address);
    pthread_mutex_lock(&lookup->lock);
    lookup->code = code;
    lookup->address = address;
    lookup->done = true;
    bool left_behind = lookup->left_behind;
    pthread_cond_signal(&lookup->done_signal);
    pthread_mutex_unlock(&lookup->lock);
    if (left_behind)
        free_lookup(lookup);
    return NULL;
}

static bool start_detached(void *(*body)(void *), void *argument) {
    pthread_attr_t attributes;
    pthread_t thread;
    if (pthread_attr_init(&attributes) != 0)
        return false;
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    bool started = pthread_create(&thread, &attributes, body, argument) == 0;
    pthread_attr_destroy(&attributes);
    return started;
}

void offline_probe_run(const char *host, int port, int limit_ms, const offline_probe_ops_t *ops,
                       offline_probe_result_t *result) {
    uint64_t start = offline_now_ms();
    uint64_t deadline = start + (uint64_t)(limit_ms > 0 ? limit_ms : 0);
    result->verdict = Verdict_Offline;
    result->lookup_ms = -1;
    result->connect_ms = -1;
    result->reason[0] = '\0';

    lookup_t *lookup = calloc(1, sizeof(*lookup));
    if (lookup == NULL) {
        snprintf(result->reason, sizeof(result->reason), "no memory for the test");
        return;
    }
    atomic_fetch_add(&offline_lookups_alive, 1);
    pthread_mutex_init(&lookup->lock, NULL);
    pthread_cond_init(&lookup->done_signal, NULL);
    lookup->resolve = ops->resolve;
    strlcpy(lookup->host, host, sizeof(lookup->host));
    if (!start_detached(lookup_thread, lookup)) {
        free_lookup(lookup);
        snprintf(result->reason, sizeof(result->reason), "the test's name lookup couldn't be started");
        return;
    }
    pthread_mutex_lock(&lookup->lock);
    for (;;) {
        uint64_t now = offline_now_ms();
        if (lookup->done || now >= deadline)
            break;
        struct timespec wait = timespec_of_ms(deadline - now);
        pthread_cond_timedwait_relative_np(&lookup->done_signal, &lookup->lock, &wait);
    }
    bool done = lookup->done;
    int code = lookup->code;
    struct sockaddr_in address = lookup->address;
    lookup->left_behind = !done;
    pthread_mutex_unlock(&lookup->lock);
    if (done)
        free_lookup(lookup);

    uint64_t looked_up = offline_now_ms();
    if (!done) {
        snprintf(result->reason, sizeof(result->reason), "the name lookup of %s didn't answer within %d ms", host,
                 limit_ms);
        return;
    }
    result->lookup_ms = (int)(looked_up - start);
    if (code != 0) {
        snprintf(result->reason, sizeof(result->reason), "the name lookup of %s failed after %d ms (%s)", host,
                 result->lookup_ms, gai_strerror(code));
        return;
    }
    // (a lookup that answers in the very moment the time runs out)
    if (looked_up >= deadline) {
        snprintf(result->reason, sizeof(result->reason), "the name lookup of %s took %d ms of the %d ms", host,
                 result->lookup_ms, limit_ms);
        return;
    }

    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    char shown[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &address.sin_addr, shown, sizeof(shown)) == NULL)
        strlcpy(shown, "?", sizeof(shown));
    int error = ops->connect_within(&address, (int)(deadline - looked_up));
    uint64_t connected = offline_now_ms();
    if (error != 0) {
        snprintf(result->reason, sizeof(result->reason), "the connection to %s (%s) port %d failed after %d ms (%s)",
                 host, shown, port, (int)(connected - looked_up), strerror(error));
        return;
    }
    result->connect_ms = (int)(connected - looked_up);
    if (connected - start > (uint64_t)(limit_ms > 0 ? limit_ms : 0)) {
        snprintf(result->reason, sizeof(result->reason), "%s (%s) port %d answered after %d ms, more than the %d ms",
                 host, shown, port, (int)(connected - start), limit_ms);
        return;
    }
    result->verdict = Verdict_Online;
    snprintf(result->reason, sizeof(result->reason), "%s (%s) port %d answered in %d ms (lookup %d ms, connection %d ms)",
             host, shown, port, (int)(connected - start), result->lookup_ms, result->connect_ms);
}

#pragma mark - the session

unsigned offline_state_start(offline_state_t *state, int mode, uint64_t now_ms, int limit_ms) {
    // anything but the settings page's values is off, as the game is without this
    if (mode != Offline_Always && mode != Offline_Automatic)
        mode = Offline_Off;
    pthread_mutex_lock(&state->lock);
    state->generation++;
    state->deadline_ms = now_ms + (uint64_t)(limit_ms > 0 ? limit_ms : 0);
    atomic_store(&state->verdict, mode == Offline_Always      ? Verdict_Offline
                                  : mode == Offline_Automatic ? Verdict_Pending
                                                              : Verdict_Online);
    atomic_store(&state->mode, mode);
    unsigned generation = state->generation;
    pthread_cond_broadcast(&state->decided);
    pthread_mutex_unlock(&state->lock);
    return generation;
}

bool offline_state_decide(offline_state_t *state, unsigned generation, int verdict) {
    if (verdict != Verdict_Online && verdict != Verdict_Offline)
        return false;
    pthread_mutex_lock(&state->lock);
    bool counts = generation == state->generation && atomic_load(&state->verdict) == Verdict_Pending;
    if (counts) {
        atomic_store(&state->verdict, verdict);
        pthread_cond_broadcast(&state->decided);
    }
    pthread_mutex_unlock(&state->lock);
    return counts;
}

bool offline_state_cut(offline_state_t *state, bool *decided_here) {
    if (decided_here != NULL)
        *decided_here = false;
    int verdict = atomic_load(&state->verdict);
    if (verdict != Verdict_Pending)
        return verdict == Verdict_Offline;
    pthread_mutex_lock(&state->lock);
    uint64_t give_up = state->deadline_ms + Offline_Verdict_Grace_Ms;
    while (atomic_load(&state->verdict) == Verdict_Pending) {
        uint64_t now = offline_now_ms();
        if (now >= give_up) {
            // the test hasn't answered: the internet is too slow for the game, whatever comes later
            atomic_store(&state->verdict, Verdict_Offline);
            pthread_cond_broadcast(&state->decided);
            if (decided_here != NULL)
                *decided_here = true;
            break;
        }
        struct timespec wait = timespec_of_ms(give_up - now);
        pthread_cond_timedwait_relative_np(&state->decided, &state->lock, &wait);
    }
    verdict = atomic_load(&state->verdict);
    pthread_mutex_unlock(&state->lock);
    return verdict == Verdict_Offline;
}

#pragma mark - the debug lines

uint64_t offline_note_key(const char *kind, const char *target) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char *text = kind; *text != '\0'; text++)
        hash = (hash ^ (uint8_t)*text) * 0x100000001b3ull;
    hash = (hash ^ '|') * 0x100000001b3ull;
    for (const char *text = target; *text != '\0'; text++)
        hash = (hash ^ (uint8_t)*text) * 0x100000001b3ull;
    return hash;
}

int offline_note(offline_notes_t *notes, uint64_t key) {
    for (int i = 0; i < notes->count; i++) {
        if (notes->keys[i] == key)
            return Note_Seen;
    }
    if (notes->count < Offline_Note_Slots) {
        notes->keys[notes->count++] = key;
        return Note_New;
    }
    if (notes->full_told)
        return Note_Full;
    notes->full_told = true;
    return Note_Full_First;
}

#pragma mark - the game's process

offline_system_t offline_system = { connect, connectx, sendto, sendmsg, getaddrinfo, gethostbyname, gethostbyname2 };

static offline_state_t session = Offline_State_Initializer;
static bool debug_lines = false;
static char own_host_name[256];
static offline_notes_t notes;
static os_unfair_lock notes_lock = OS_UNFAIR_LOCK_INIT;

#define Cut_Line "the game gets no internet, only this Mac, its local network and multicast"

// one debug line per kind and target ("refused a connection to", "1.2.3.4:3074 (connect)"), with detail behind it
static void tell_once(const char *kind, const char *target, const char *detail) {
    os_unfair_lock_lock(&notes_lock);
    int note = offline_note(&notes, offline_note_key(kind, target));
    os_unfair_lock_unlock(&notes_lock);
    if (note == Note_New)
        printf("BO3MacFix: offline mode: %s %s%s\n", kind, target, detail);
    else if (note == Note_Full_First)
        printf("BO3MacFix: offline mode: more than %d refusals and waits, the others aren't listed\n",
               Offline_Note_Slots);
}

static void describe_address(const struct sockaddr *address, char *text, size_t size) {
    char shown[INET6_ADDRSTRLEN] = "?";
    int port = 0;
    if (address->sa_family == AF_INET) {
        struct sockaddr_in v4;
        memcpy(&v4, address, sizeof(v4));
        inet_ntop(AF_INET, &v4.sin_addr, shown, sizeof(shown));
        port = ntohs(v4.sin_port);
        snprintf(text, size, "%s:%d", shown, port);
    } else {
        struct sockaddr_in6 v6;
        memcpy(&v6, address, sizeof(v6));
        inet_ntop(AF_INET6, &v6.sin6_addr, shown, sizeof(shown));
        port = ntohs(v6.sin6_port);
        snprintf(text, size, "[%s]:%d", shown, port);
    }
}

// Whether the game's call to something outside this Mac and its local network is refused now. In automatic mode
// before the test's verdict this waits for it (at most until its limit and a little more). what is "a connection to",
// "a name lookup of" ..., call the system call's name; address is NULL for a name.
static bool refused(const char *what, const struct sockaddr *address, const char *name, const char *call) {
    bool was_pending = atomic_load(&session.verdict) == Verdict_Pending;
    uint64_t before = offline_now_ms();
    bool decided_here = false;
    bool cut = offline_state_cut(&session, &decided_here);
    uint64_t waited = offline_now_ms() - before;
    if (decided_here)
        printf("BO3MacFix: offline mode: automatic: offline for this session, the test hadn't answered in time ("
               Cut_Line ")\n");
    if (!debug_lines || (!cut && !was_pending))
        return cut;
    char target[300];
    if (address != NULL) {
        char shown[80];
        describe_address(address, shown, sizeof(shown));
        snprintf(target, sizeof(target), "%s (%s)", shown, call);
    } else {
        snprintf(target, sizeof(target), "%s (%s)", name, call);
    }
    if (was_pending) {
        char kind[120], detail[80];
        snprintf(kind, sizeof(kind), "waited for the automatic test's verdict on %s", what);
        snprintf(detail, sizeof(detail), ": %llu ms, then %s", waited, cut ? "refused" : "let through");
        tell_once(kind, target, detail);
    }
    if (cut) {
        char kind[120];
        snprintf(kind, sizeof(kind), "refused %s", what);
        tell_once(kind, target, "");
    }
    return cut;
}

// whether a call to address is refused: never while off (one load of the mode), never for a local address
static bool address_refused(const struct sockaddr *address, socklen_t length, const char *what, const char *call) {
    return atomic_load_explicit(&session.mode, memory_order_relaxed) != Offline_Off &&
           !offline_address_is_local(address, length) && refused(what, address, NULL, call);
}

static bool name_refused(const char *name, const char *call) {
    return atomic_load_explicit(&session.mode, memory_order_relaxed) != Offline_Off &&
           !offline_name_is_local(name, own_host_name) && refused("a name lookup of", NULL, name, call);
}

int offline_connect(int socket, const struct sockaddr *address, socklen_t length) {
    if (address_refused(address, length, "a connection to", "connect")) {
        errno = ENETUNREACH;
        return -1;
    }
    return offline_system.connect(socket, address, length);
}

int offline_connectx(int socket, const sa_endpoints_t *endpoints, sae_associd_t association, unsigned int flags,
                     const struct iovec *data, unsigned int data_count, size_t *length, sae_connid_t *connection) {
    if (endpoints != NULL &&
        address_refused(endpoints->sae_dstaddr, endpoints->sae_dstaddrlen, "a connection to", "connectx")) {
        errno = ENETUNREACH;
        return -1;
    }
    return offline_system.connectx(socket, endpoints, association, flags, data, data_count, length, connection);
}

ssize_t offline_sendto(int socket, const void *buffer, size_t size, int flags, const struct sockaddr *address,
                       socklen_t length) {
    if (address_refused(address, length, "a datagram to", "sendto")) {
        errno = ENETUNREACH;
        return -1;
    }
    return offline_system.sendto(socket, buffer, size, flags, address, length);
}

ssize_t offline_sendmsg(int socket, const struct msghdr *message, int flags) {
    if (message != NULL &&
        address_refused((const struct sockaddr *)message->msg_name, message->msg_namelen, "a datagram to",
                        "sendmsg")) {
        errno = ENETUNREACH;
        return -1;
    }
    return offline_system.sendmsg(socket, message, flags);
}

int offline_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result) {
    if (name_refused(node, "getaddrinfo")) {
        if (result != NULL)
            *result = NULL;
        return EAI_NONAME;
    }
    return offline_system.getaddrinfo(node, service, hints, result);
}

struct hostent *offline_gethostbyname(const char *name) {
    if (name_refused(name, "gethostbyname")) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    return offline_system.gethostbyname(name);
}

struct hostent *offline_gethostbyname2(const char *name, int family) {
    if (name_refused(name, "gethostbyname2")) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    return offline_system.gethostbyname2(name, family);
}

DYLD_INTERPOSE(offline_connect, connect);
DYLD_INTERPOSE(offline_connectx, connectx);
DYLD_INTERPOSE(offline_sendto, sendto);
DYLD_INTERPOSE(offline_sendmsg, sendmsg);
DYLD_INTERPOSE(offline_getaddrinfo, getaddrinfo);
DYLD_INTERPOSE(offline_gethostbyname, gethostbyname);
DYLD_INTERPOSE(offline_gethostbyname2, gethostbyname2);

typedef struct {
    unsigned generation;
    char host[256];
    int port, limit_ms;
    const offline_probe_ops_t *ops;
} probe_job_t;

static void *probe_thread(void *argument) {
    probe_job_t *job = argument;
    offline_probe_result_t result;
    offline_probe_run(job->host, job->port, job->limit_ms, job->ops, &result);
    if (offline_state_decide(&session, job->generation, result.verdict)) {
        if (result.verdict == Verdict_Online)
            printf("BO3MacFix: offline mode: automatic: online as usual, %s\n", result.reason);
        else
            printf("BO3MacFix: offline mode: automatic: offline for this session, %s (" Cut_Line ")\n", result.reason);
    } else if (debug_lines) {
        printf("BO3MacFix: offline mode: automatic: the test's answer came too late to count: %s\n", result.reason);
    }
    free(job);
    return NULL;
}

void offline_mode_start(int mode, bool debug, const char *own_name, const char *host, int port, int limit_ms,
                        const offline_probe_ops_t *ops) {
    debug_lines = debug;
    strlcpy(own_host_name, own_name != NULL ? own_name : "", sizeof(own_host_name));
    os_unfair_lock_lock(&notes_lock);
    memset(&notes, 0, sizeof(notes));
    os_unfair_lock_unlock(&notes_lock);

    unsigned generation = offline_state_start(&session, mode, offline_now_ms(), limit_ms);
    mode = atomic_load(&session.mode);
    if (mode == Offline_Always) {
        printf("BO3MacFix: offline mode: always (OfflineMode 1): " Cut_Line "\n");
    } else if (mode == Offline_Automatic) {
        printf("BO3MacFix: offline mode: automatic (OfflineMode 2): asking %s port %d whether the internet answers "
               "within %d ms\n", host, port, limit_ms);
        probe_job_t *job = calloc(1, sizeof(*job));
        if (job != NULL) {
            job->generation = generation;
            strlcpy(job->host, host, sizeof(job->host));
            job->port = port;
            job->limit_ms = limit_ms;
            job->ops = ops;
        }
        if (job == NULL || !start_detached(probe_thread, job)) {
            free(job);
            if (offline_state_decide(&session, generation, Verdict_Offline))
                printf("BO3MacFix: offline mode: automatic: offline for this session, the test couldn't be started "
                       "(" Cut_Line ")\n");
        }
    }
}

void offline_mode_init(int mode, bool debug, const char *probe_target) {
    char host[256];
    int port = Offline_Probe_Port;
    strlcpy(host, Offline_Probe_Host, sizeof(host));
    // BO3MACFIX_OFFLINEPROBE=host:port asks another server (a test: "127.0.0.1:9" is refused at once, on this Mac)
    const char *colon = probe_target != NULL ? strrchr(probe_target, ':') : NULL;
    if (colon != NULL && colon != probe_target && (size_t)(colon - probe_target) < sizeof(host)) {
        int given = atoi(colon + 1);
        if (given > 0 && given < 65536) {
            memcpy(host, probe_target, (size_t)(colon - probe_target));
            host[colon - probe_target] = '\0';
            port = given;
        }
    }
    // the name the game asks for as its own ("Hostname: ..." in its console), from the same call
    char own_name[256];
    if (gethostname(own_name, sizeof(own_name)) != 0)
        own_name[0] = '\0';
    own_name[sizeof(own_name) - 1] = '\0';
    offline_mode_start(mode, debug, own_name, host, port, Offline_Probe_Limit_Ms, &offline_probe_system_ops);
}

bool offline_mode_cut(void) {
    return atomic_load_explicit(&session.mode, memory_order_relaxed) != Offline_Off && offline_state_cut(&session, NULL);
}
