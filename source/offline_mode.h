// OfflineMode: the game without the internet, so a bad or missing connection (airplane Wi-Fi) never holds the player
// up or throws them out. With it on, or in its automatic mode when the internet doesn't answer quickly at launch, the
// game's process gets no internet: a name lookup of anything but a local name fails at once, and so does a connection
// or a datagram to anything but this Mac, its local network or multicast. Nothing is sent, so nothing can wait on a
// connection, and the game takes its own way for a Mac without network: it never signs in to its online service
// (Demonware), so Return at All-around Enhancement's start screen goes to the mod's LAN lobby at once, and a game
// that never was online can't lose its connection. LAN play stays as it is.
//
// The rules (which addresses and names are local, the automatic test, what is decided when) are plain C with no game
// addresses, so tests/test_offline_mode.c tests them as they are; the replacements of the system's network calls at
// the end of offline_mode.c are the thin layer, and they call the system through offline_system, which the tests
// replace so that nothing leaves the machine.
#pragma once
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

// OfflineMode's values
enum { Offline_Off = 0, Offline_Always = 1, Offline_Automatic = 2 };

// what the automatic test decided for this session
enum { Verdict_Pending = 0, Verdict_Online = 1, Verdict_Offline = 2 };

// The automatic test asks the game's own first stop online: Demonware's lobby server, which the game looks up as its
// "lsg address" and connects to on TCP 3074 (Mac 0x1011a4dbb; bdAddr(address, 0xc02) at 0x1011a4e82). Only the
// connection is opened and closed again; nothing is sent on it.
#define Offline_Probe_Host "ops3-pc-lobby.prod.demonware.net"
#define Offline_Probe_Port 3074
// how long the test may take in all, counted from the library's load. A connection that needs longer than this for a
// name lookup and a connection is too slow for the game's online start, which then waits on dozens of requests.
#define Offline_Probe_Limit_Ms 1500
// how much longer than that a game call waits for the test's verdict before deciding "offline" itself
#define Offline_Verdict_Grace_Ms 250

// whether a connection or datagram to address stays open in offline mode: this Mac (loopback, unspecified), its
// local network (10/8, 172.16/12, 192.168/16, link-local 169.254/16, IPv6 fe80::/10 and fc00::/7), multicast,
// broadcast, IPv4 mapped into IPv6 by the same rules, and every family that isn't IP (Unix sockets). An address too
// short for its family counts as local, so the system refuses it as it always did.
bool offline_address_is_local(const struct sockaddr *address, socklen_t length);

// whether a name lookup of name stays open in offline mode: numbers (no lookup happens), "localhost" and names under
// it, names under ".local" (Bonjour, answered on the local network) and this Mac's own host name.
bool offline_name_is_local(const char *name, const char *own_host_name);

// a monotonic clock in milliseconds
uint64_t offline_now_ms(void);

#pragma mark - the automatic test

typedef struct {
    // looks host up and gives its first IPv4 address in *address (the port isn't set): 0, or a getaddrinfo error
    int (*resolve)(const char *host, struct sockaddr_in *address);
    // opens a TCP connection to address and closes it again, giving up after timeout_ms: 0 when it was accepted,
    // else an errno value (ETIMEDOUT when the time ran out)
    int (*connect_within)(const struct sockaddr_in *address, int timeout_ms);
} offline_probe_ops_t;

// the system's own calls, as the library itself makes them (never through the replacements below)
extern const offline_probe_ops_t offline_probe_system_ops;
// how many name lookups of the test are allocated (for the tests: one left behind frees itself when it is done)
extern _Atomic int offline_lookups_alive;

typedef struct {
    int verdict;            // Verdict_Online or Verdict_Offline
    int lookup_ms;          // how long the name lookup took, -1 when it hadn't answered by the limit
    int connect_ms;         // how long the connection took, -1 when none was tried or it hadn't answered
    char reason[160];       // what happened, for the log
} offline_probe_result_t;

// Looks host up and opens a connection to it on port, everything within limit_ms (the lookup runs in a thread of its
// own, which is left behind when it doesn't answer in time). Online only when both answered in time.
void offline_probe_run(const char *host, int port, int limit_ms, const offline_probe_ops_t *ops,
                       offline_probe_result_t *result);

#pragma mark - the session

typedef struct {
    _Atomic int mode;           // Offline_*
    _Atomic int verdict;        // Verdict_*: Online when off, Offline when always, Pending until the test answers
    uint64_t deadline_ms;       // automatic: the test's limit, on offline_now_ms's clock
    unsigned generation;        // counts the starts, so that a test of an earlier start can't decide for this one
    pthread_mutex_t lock;
    pthread_cond_t decided;
} offline_state_t;
#define Offline_State_Initializer { .lock = PTHREAD_MUTEX_INITIALIZER, .decided = PTHREAD_COND_INITIALIZER }

// (Re)starts a session (a state made with Offline_State_Initializer) and returns its generation.
unsigned offline_state_start(offline_state_t *state, int mode, uint64_t now_ms, int limit_ms);
// The automatic test's verdict for the start with that generation; only the first one counts. Returns whether this
// one did.
bool offline_state_decide(offline_state_t *state, unsigned generation, int verdict);
// Whether the internet is cut now. Before the automatic test's verdict this waits for it, at most until its limit
// plus Offline_Verdict_Grace_Ms, and then decides "offline" itself (*decided_here tells, when not NULL).
bool offline_state_cut(offline_state_t *state, bool *decided_here);

#pragma mark - the debug lines

// Which refusals have been told already, so that each target is told once (BO3MACFIX_OFFLINEDEBUG=1).
#define Offline_Note_Slots 64
typedef struct {
    uint64_t keys[Offline_Note_Slots];
    int count;
    bool full_told;
} offline_notes_t;

enum { Note_New, Note_Seen, Note_Full_First, Note_Full };
// Note_New the first time key comes (and it is remembered), Note_Seen after that; once every slot is taken a new key
// gives Note_Full_First once (for a line saying that more aren't listed), then Note_Full.
int offline_note(offline_notes_t *notes, uint64_t key);
// FNV-1a over a kind and a target, the key for offline_note
uint64_t offline_note_key(const char *kind, const char *target);

#pragma mark - the game's process

// The system calls the replacements use when they let a call through. The tests put stand-ins here.
typedef struct {
    int (*connect)(int, const struct sockaddr *, socklen_t);
    int (*connectx)(int, const sa_endpoints_t *, sae_associd_t, unsigned int, const struct iovec *, unsigned int,
                    size_t *, sae_connid_t *);
    ssize_t (*sendto)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
    ssize_t (*sendmsg)(int, const struct msghdr *, int);
    int (*getaddrinfo)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
    struct hostent *(*gethostbyname)(const char *);
    struct hostent *(*gethostbyname2)(const char *, int);
} offline_system_t;
extern offline_system_t offline_system;

// From the library's constructor, in the game's process only, before the game runs: OfflineMode's value and whether
// BO3MACFIX_OFFLINEDEBUG asks for the debug lines. Off changes nothing: every call goes to the system as before.
// probe_target is BO3MACFIX_OFFLINEPROBE ("host:port", for tests: "127.0.0.1:9" fails at once without leaving the
// Mac), NULL for the game's own lobby server.
void offline_mode_init(int mode, bool debug, const char *probe_target);
// offline_mode_init with every part given (the tests: their own host name, test operations and limit). own_name is
// the Mac's host name, which stays a local name.
void offline_mode_start(int mode, bool debug, const char *own_name, const char *host, int port, int limit_ms,
                        const offline_probe_ops_t *ops);
// whether the game's internet is cut now (for the library's own online requests, AAEOnline)
bool offline_mode_cut(void);

// the replacements (DYLD_INTERPOSE in offline_mode.c); public for the tests
int offline_connect(int socket, const struct sockaddr *address, socklen_t length);
int offline_connectx(int socket, const sa_endpoints_t *endpoints, sae_associd_t association, unsigned int flags,
                     const struct iovec *data, unsigned int data_count, size_t *length, sae_connid_t *connection);
ssize_t offline_sendto(int socket, const void *buffer, size_t size, int flags, const struct sockaddr *address,
                       socklen_t length);
ssize_t offline_sendmsg(int socket, const struct msghdr *message, int flags);
int offline_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result);
struct hostent *offline_gethostbyname(const char *name);
struct hostent *offline_gethostbyname2(const char *name, int family);
