// Tests for offline_mode.c (OfflineMode): which addresses and names count as local, the automatic test against
// made-up lookups and connections and against this Mac's loopback, the session's verdict and its waiting, the debug
// lines, and the replacements of the system's network calls against stand-ins. Nothing here leaves the machine: the
// replacements' system calls are stand-ins whenever an address or name outside this Mac is passed, and the real
// lookups and connections only ever get "127.0.0.1".
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "../source/offline_mode.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#pragma mark - helpers

static struct sockaddr_in v4(const char *text, int port) {
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, text, &address.sin_addr) != 1)
        printf("FAIL: bad test address %s\n", text), failures++;
    return address;
}

static struct sockaddr_in6 v6(const char *text, int port) {
    struct sockaddr_in6 address;
    memset(&address, 0, sizeof(address));
    address.sin6_len = sizeof(address);
    address.sin6_family = AF_INET6;
    address.sin6_port = htons((uint16_t)port);
    if (inet_pton(AF_INET6, text, &address.sin6_addr) != 1)
        printf("FAIL: bad test address %s\n", text), failures++;
    return address;
}

static bool local4(const char *text) {
    struct sockaddr_in address = v4(text, 3074);
    return offline_address_is_local((const struct sockaddr *)&address, sizeof(address));
}

static bool local6(const char *text) {
    struct sockaddr_in6 address = v6(text, 3074);
    return offline_address_is_local((const struct sockaddr *)&address, sizeof(address));
}

static void sleep_ms(int ms) {
    usleep((useconds_t)ms * 1000);
}

// stdout into a pipe while a part runs, to read the lines it printed
static int saved_stdout = -1, capture_pipe[2];
static char captured[65536];
static size_t captured_length;

static void capture_start(void) {
    fflush(stdout);
    if (pipe(capture_pipe) != 0)
        printf("FAIL: no pipe\n"), failures++;
    fcntl(capture_pipe[0], F_SETFL, O_NONBLOCK);
    saved_stdout = dup(STDOUT_FILENO);
    dup2(capture_pipe[1], STDOUT_FILENO);
    captured_length = 0;
    captured[0] = '\0';
}

// what has come through the pipe so far
static void capture_read(void) {
    fflush(stdout);
    for (;;) {
        ssize_t got = read(capture_pipe[0], captured + captured_length, sizeof(captured) - 1 - captured_length);
        if (got <= 0)
            break;
        captured_length += (size_t)got;
    }
    captured[captured_length] = '\0';
}

// Ends the capture once part has been printed, at most 3 s later: the automatic test's thread prints its verdict
// line just after it has decided, so a call that waited for the verdict can be back before the line is out.
static const char *capture_end_after(const char *part) {
    for (int i = 0; i < 300; i++) {
        capture_read();
        if (part == NULL || strstr(captured, part) != NULL)
            break;
        sleep_ms(10);
    }
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    close(capture_pipe[1]);
    capture_read();
    close(capture_pipe[0]);
    return captured;
}

static const char *capture_end(void) {
    return capture_end_after(NULL);
}

static int count_of(const char *text, const char *part) {
    int count = 0;
    for (const char *at = strstr(text, part); at != NULL; at = strstr(at + 1, part))
        count++;
    return count;
}

#pragma mark - addresses and names

static void test_addresses(void) {
    // this Mac, the private networks, link-local, multicast, broadcast
    CHECK(local4("127.0.0.1"));
    CHECK(local4("127.255.255.254"));
    CHECK(local4("0.0.0.0"));
    CHECK(local4("0.255.255.255"));
    CHECK(local4("10.0.0.0"));
    CHECK(local4("10.255.255.255"));
    CHECK(local4("172.16.0.0"));
    CHECK(local4("172.31.255.255"));
    CHECK(local4("192.168.0.0"));
    CHECK(local4("192.168.255.255"));
    CHECK(local4("169.254.0.1"));
    CHECK(local4("169.254.255.255"));
    CHECK(local4("224.0.0.1"));
    CHECK(local4("239.255.255.250"));
    CHECK(local4("255.255.255.255"));
    // the internet, right next to each of them
    CHECK(!local4("1.0.0.0"));
    CHECK(!local4("9.255.255.255"));
    CHECK(!local4("11.0.0.0"));
    CHECK(!local4("126.255.255.255"));
    CHECK(!local4("128.0.0.0"));
    CHECK(!local4("172.15.255.255"));
    CHECK(!local4("172.32.0.0"));
    CHECK(!local4("192.167.255.255"));
    CHECK(!local4("192.169.0.0"));
    CHECK(!local4("169.253.255.255"));
    CHECK(!local4("169.255.0.0"));
    CHECK(!local4("223.255.255.255"));
    CHECK(!local4("240.0.0.0"));
    CHECK(!local4("255.255.255.254"));
    CHECK(!local4("100.64.0.1"));
    CHECK(!local4("8.8.8.8"));
    CHECK(!local4("185.34.107.128"));

    CHECK(local6("::"));
    CHECK(local6("::1"));
    CHECK(local6("fe80::1"));
    CHECK(local6("febf:ffff::1"));
    CHECK(local6("fc00::1"));
    CHECK(local6("fdff:ffff::1"));
    CHECK(local6("ff02::1"));
    CHECK(local6("ff05::1:3"));
    CHECK(local6("::ffff:127.0.0.1"));
    CHECK(local6("::ffff:192.168.1.20"));
    CHECK(!local6("::2"));
    CHECK(!local6("0100::1"));
    CHECK(!local6("fe7f:ffff::1"));
    CHECK(!local6("fec0::1"));
    CHECK(!local6("fbff:ffff::1"));
    CHECK(!local6("fe00::1"));
    CHECK(!local6("2a80::1"));
    CHECK(!local6("fe00::1"));
    CHECK(!local6("2001:4860:4860::8888"));
    CHECK(!local6("64:ff9b::808:808"));
    CHECK(!local6("::ffff:8.8.8.8"));
    CHECK(!local6("::fffe:127.0.0.1"));
    CHECK(!local6("0:0:0:0:1:ffff:7f00:1"));
    CHECK(!local6("::127.0.0.2"));

    // other families, and addresses too short to read: the system decides as always
    struct sockaddr_un unix_address = { .sun_len = sizeof(unix_address), .sun_family = AF_UNIX, .sun_path = "/x" };
    CHECK(offline_address_is_local((const struct sockaddr *)&unix_address, sizeof(unix_address)));
    struct sockaddr_in public4 = v4("8.8.8.8", 53);
    CHECK(!offline_address_is_local((const struct sockaddr *)&public4, sizeof(public4)));
    CHECK(!offline_address_is_local((const struct sockaddr *)&public4, sizeof(public4) + 4));
    CHECK(offline_address_is_local((const struct sockaddr *)&public4, sizeof(public4) - 1));
    CHECK(offline_address_is_local((const struct sockaddr *)&public4, 1));
    CHECK(offline_address_is_local(NULL, 0));
    // shorter than its family field: not even that is read (the sanitizer would catch it in this one-byte buffer)
    uint8_t *one_byte = malloc(1);
    one_byte[0] = 1;
    CHECK(offline_address_is_local((const struct sockaddr *)one_byte, 1));
    free(one_byte);
    struct sockaddr_in6 public6 = v6("2001:4860:4860::8888", 53);
    CHECK(!offline_address_is_local((const struct sockaddr *)&public6, sizeof(public6)));
    CHECK(offline_address_is_local((const struct sockaddr *)&public6, sizeof(public6) - 1));
    CHECK(offline_address_is_local((const struct sockaddr *)&public6, sizeof(struct sockaddr_in)));
    struct sockaddr unspecified = { .sa_len = sizeof(unspecified), .sa_family = AF_UNSPEC };
    CHECK(offline_address_is_local(&unspecified, sizeof(unspecified)));
}

static void test_names(void) {
    const char *own = "adems-amazing-macbook-pro";
    // the game's online service, and other names that need a DNS server
    CHECK(!offline_name_is_local("ops3-pc-lobby.prod.demonware.net", own));
    CHECK(!offline_name_is_local("ops3-pc-auth3.prod.demonware.net.", own));
    CHECK(!offline_name_is_local("stun.us.demonware.net", own));
    CHECK(!offline_name_is_local("printer", own));
    CHECK(!offline_name_is_local("local", own));
    CHECK(!offline_name_is_local("x.localdomain", own));
    CHECK(!offline_name_is_local("localhost.example.com", own));
    CHECK(!offline_name_is_local("mylocalhost", own));
    CHECK(!offline_name_is_local("notlocal", own));
    CHECK(!offline_name_is_local("adems-amazing-macbook-pro2", own));
    CHECK(!offline_name_is_local("adems-amazing-macbook-pr", own));
    CHECK(!offline_name_is_local("1.2.3.4 evil.com", own));
    CHECK(!offline_name_is_local("1.2.3.4.example.com", own));
    CHECK(!offline_name_is_local("fe80::1%en0.example.com", own));
    CHECK(!offline_name_is_local("fe80::1%", own));
    CHECK(!offline_name_is_local("fe80::1%en0 x", own));
    CHECK(!offline_name_is_local("adems-amazing-macbook-pro", NULL));
    CHECK(!offline_name_is_local("adems-amazing-macbook-pro", ""));
    // numbers: nothing is looked up
    CHECK(offline_name_is_local("8.8.8.8", own));
    CHECK(offline_name_is_local("127.1", own));
    CHECK(offline_name_is_local("0x7f.1", own));
    CHECK(offline_name_is_local("2001:4860:4860::8888", own));
    CHECK(offline_name_is_local("fe80::1%en0", own));
    CHECK(offline_name_is_local("::1", own));
    // this Mac and the local network
    CHECK(offline_name_is_local("localhost", own));
    CHECK(offline_name_is_local("LocalHost.", own));
    CHECK(offline_name_is_local("steam.localhost", own));
    CHECK(offline_name_is_local("adems-amazing-macbook-pro.local", own));
    CHECK(offline_name_is_local("Printer.LOCAL.", own));
    CHECK(offline_name_is_local("adems-amazing-macbook-pro", own));
    CHECK(offline_name_is_local("ADEMS-AMAZING-MACBOOK-PRO.", own));
    CHECK(offline_name_is_local("adems-amazing-macbook-pro", "adems-amazing-macbook-pro."));
    CHECK(offline_name_is_local(NULL, own));
    CHECK(offline_name_is_local("", own));
}

static void test_notes(void) {
    offline_notes_t notes;
    memset(&notes, 0, sizeof(notes));
    uint64_t a = offline_note_key("refused a connection to", "1.2.3.4:3074 (connect)");
    uint64_t b = offline_note_key("refused a connection to", "1.2.3.5:3074 (connect)");
    uint64_t c = offline_note_key("refused a datagram to", "1.2.3.4:3074 (connect)");
    CHECK(a != b && a != c && b != c);
    CHECK(offline_note_key("ab", "c") != offline_note_key("a", "bc"));
    CHECK(a == offline_note_key("refused a connection to", "1.2.3.4:3074 (connect)"));
    CHECK(offline_note(&notes, a) == Note_New);
    CHECK(offline_note(&notes, a) == Note_Seen);
    CHECK(offline_note(&notes, b) == Note_New);
    CHECK(offline_note(&notes, a) == Note_Seen);
    CHECK(offline_note(&notes, b) == Note_Seen);
    for (uint64_t key = 1000; notes.count < Offline_Note_Slots; key++)
        CHECK(offline_note(&notes, key) == Note_New);
    CHECK(offline_note(&notes, 1000) == Note_Seen);
    CHECK(offline_note(&notes, b) == Note_Seen);
    CHECK(offline_note(&notes, 99999) == Note_Full_First);
    CHECK(offline_note(&notes, 99998) == Note_Full);
    CHECK(offline_note(&notes, 99999) == Note_Full);
    CHECK(notes.count == Offline_Note_Slots);
}

#pragma mark - the session's verdict

typedef struct {
    offline_state_t *state;
    unsigned generation;
    int verdict, after_ms;
} later_t;

static void *decide_later(void *argument) {
    later_t *later = argument;
    sleep_ms(later->after_ms);
    offline_state_decide(later->state, later->generation, later->verdict);
    return NULL;
}

static void test_state(void) {
    static offline_state_t state = Offline_State_Initializer;
    uint64_t now = offline_now_ms();

    unsigned first = offline_state_start(&state, Offline_Off, now, 1500);
    CHECK(atomic_load(&state.mode) == Offline_Off);
    CHECK(!offline_state_cut(&state, NULL));
    CHECK(!offline_state_decide(&state, first, Verdict_Offline));
    CHECK(!offline_state_cut(&state, NULL));

    unsigned second = offline_state_start(&state, Offline_Always, now, 1500);
    CHECK(second != first);
    CHECK(atomic_load(&state.mode) == Offline_Always);
    CHECK(offline_state_cut(&state, NULL));
    CHECK(!offline_state_decide(&state, second, Verdict_Online));
    CHECK(offline_state_cut(&state, NULL));

    // values the settings page doesn't have are off
    offline_state_start(&state, 3, now, 1500);
    CHECK(atomic_load(&state.mode) == Offline_Off && !offline_state_cut(&state, NULL));
    offline_state_start(&state, -1, now, 1500);
    CHECK(atomic_load(&state.mode) == Offline_Off && !offline_state_cut(&state, NULL));

    // automatic: the first verdict of this start counts, nothing else
    unsigned third = offline_state_start(&state, Offline_Automatic, now, 1500);
    CHECK(atomic_load(&state.mode) == Offline_Automatic);
    CHECK(atomic_load(&state.verdict) == Verdict_Pending);
    CHECK(!offline_state_decide(&state, second, Verdict_Online));
    CHECK(!offline_state_decide(&state, third, Verdict_Pending));
    CHECK(!offline_state_decide(&state, third, 7));
    CHECK(atomic_load(&state.verdict) == Verdict_Pending);
    CHECK(offline_state_decide(&state, third, Verdict_Online));
    CHECK(!offline_state_decide(&state, third, Verdict_Offline));
    bool decided_here = true;
    CHECK(!offline_state_cut(&state, &decided_here));
    CHECK(!decided_here);
    unsigned fourth = offline_state_start(&state, Offline_Automatic, now, 1500);
    CHECK(offline_state_decide(&state, fourth, Verdict_Offline));
    CHECK(offline_state_cut(&state, &decided_here));
    CHECK(!decided_here);

    // a call before the verdict waits for it
    unsigned fifth = offline_state_start(&state, Offline_Automatic, offline_now_ms(), 1500);
    later_t later = { &state, fifth, Verdict_Online, 120 };
    pthread_t thread;
    pthread_create(&thread, NULL, decide_later, &later);
    uint64_t before = offline_now_ms();
    CHECK(!offline_state_cut(&state, &decided_here));
    uint64_t waited = offline_now_ms() - before;
    CHECK(waited >= 100 && waited < 1200);
    CHECK(!decided_here);
    pthread_join(thread, NULL);

    unsigned sixth = offline_state_start(&state, Offline_Automatic, offline_now_ms(), 1500);
    later = (later_t){ &state, sixth, Verdict_Offline, 120 };
    pthread_create(&thread, NULL, decide_later, &later);
    before = offline_now_ms();
    CHECK(offline_state_cut(&state, &decided_here));
    waited = offline_now_ms() - before;
    CHECK(waited >= 100 && waited < 1200);
    CHECK(!decided_here);
    pthread_join(thread, NULL);

    // no verdict: at the limit plus the grace the call decides "offline" itself, and later calls don't wait
    before = offline_now_ms();
    unsigned seventh = offline_state_start(&state, Offline_Automatic, before, 200);
    CHECK(offline_state_cut(&state, &decided_here));
    waited = offline_now_ms() - before;
    CHECK(waited >= 200 + Offline_Verdict_Grace_Ms - 5 && waited < 1500);
    CHECK(decided_here);
    CHECK(atomic_load(&state.verdict) == Verdict_Offline);
    CHECK(!offline_state_decide(&state, seventh, Verdict_Online));
    before = offline_now_ms();
    CHECK(offline_state_cut(&state, &decided_here));
    CHECK(!decided_here);
    CHECK(offline_now_ms() - before < 200);

    // a limit already over: no waiting beyond the grace
    before = offline_now_ms();
    offline_state_start(&state, Offline_Automatic, before - 5000, 1500);
    CHECK(offline_state_cut(&state, &decided_here));
    CHECK(decided_here);
    CHECK(offline_now_ms() - before < 300);
}

#pragma mark - the automatic test

static _Atomic int lookup_delay_ms, lookup_code, connect_delay_ms, connect_code, connect_ignores_timeout;
static _Atomic int connect_calls, connect_port, connect_timeout_ms, lookups_running;
static char lookup_host[256];
static uint32_t connect_address;

static int fake_resolve(const char *host, struct sockaddr_in *address) {
    atomic_fetch_add(&lookups_running, 1);
    strlcpy(lookup_host, host, sizeof(lookup_host));
    sleep_ms(atomic_load(&lookup_delay_ms));
    *address = v4("203.0.113.7", 0);
    atomic_fetch_sub(&lookups_running, 1);
    return atomic_load(&lookup_code);
}

static int fake_connect_within(const struct sockaddr_in *address, int timeout_ms) {
    atomic_fetch_add(&connect_calls, 1);
    atomic_store(&connect_port, ntohs(address->sin_port));
    atomic_store(&connect_timeout_ms, timeout_ms);
    connect_address = ntohl(address->sin_addr.s_addr);
    int delay = atomic_load(&connect_delay_ms);
    if (!atomic_load(&connect_ignores_timeout) && delay > timeout_ms) {
        sleep_ms(timeout_ms);
        return ETIMEDOUT;
    }
    sleep_ms(delay);
    return atomic_load(&connect_code);
}

static const offline_probe_ops_t fake_ops = { fake_resolve, fake_connect_within };

static void set_fakes(int lookup_ms, int lookup_result, int connect_ms, int connect_result, bool ignores_timeout) {
    atomic_store(&lookup_delay_ms, lookup_ms);
    atomic_store(&lookup_code, lookup_result);
    atomic_store(&connect_delay_ms, connect_ms);
    atomic_store(&connect_code, connect_result);
    atomic_store(&connect_ignores_timeout, ignores_timeout);
    atomic_store(&connect_calls, 0);
    atomic_store(&connect_port, 0);
    atomic_store(&connect_timeout_ms, 0);
}

// until the made-up lookups left behind are done and the test has freed what they had
static void wait_for_lookups(void) {
    for (int i = 0; i < 500 && (atomic_load(&lookups_running) > 0 || atomic_load(&offline_lookups_alive) > 0); i++)
        sleep_ms(10);
    CHECK(atomic_load(&offline_lookups_alive) == 0);
}

static void test_probe(void) {
    offline_probe_result_t result;

    // both answer: online, the lobby server's port, the time that is left
    set_fakes(20, 0, 30, 0, false);
    uint64_t before = offline_now_ms();
    offline_probe_run("lobby.example", 3074, 1500, &fake_ops, &result);
    uint64_t took = offline_now_ms() - before;
    CHECK(result.verdict == Verdict_Online);
    CHECK(strcmp(lookup_host, "lobby.example") == 0);
    CHECK(atomic_load(&connect_calls) == 1);
    CHECK(atomic_load(&connect_port) == 3074);
    CHECK(connect_address == 0xcb007107u);
    CHECK(atomic_load(&connect_timeout_ms) > 900 && atomic_load(&connect_timeout_ms) <= 1480);
    CHECK(result.lookup_ms >= 15 && result.lookup_ms < 1000);
    CHECK(result.connect_ms >= 25 && result.connect_ms < 1000);
    CHECK(took >= 45 && took < 1400);
    CHECK(strstr(result.reason, "lobby.example (203.0.113.7) port 3074 answered in") != NULL);

    // the lookup fails: offline, no connection tried
    set_fakes(10, EAI_NONAME, 10, 0, false);
    offline_probe_run("lobby.example", 3074, 1500, &fake_ops, &result);
    CHECK(result.verdict == Verdict_Offline);
    CHECK(atomic_load(&connect_calls) == 0);
    CHECK(result.lookup_ms >= 5 && result.connect_ms == -1);
    CHECK(strstr(result.reason, "failed") != NULL);

    // the lookup doesn't answer in time: offline at the limit, the lookup left behind
    set_fakes(800, 0, 10, 0, false);
    before = offline_now_ms();
    offline_probe_run("slow.example", 3074, 300, &fake_ops, &result);
    took = offline_now_ms() - before;
    CHECK(result.verdict == Verdict_Offline);
    CHECK(took >= 290 && took < 1200);
    CHECK(atomic_load(&connect_calls) == 0);
    CHECK(result.lookup_ms == -1);
    CHECK(strstr(result.reason, "didn't answer within 300 ms") != NULL);
    wait_for_lookups();
    CHECK(atomic_load(&connect_calls) == 0);

    // the lookup answers just at the limit: nothing left for the connection
    set_fakes(320, 0, 0, 0, false);
    offline_probe_run("slow.example", 3074, 300, &fake_ops, &result);
    CHECK(result.verdict == Verdict_Offline);
    CHECK(atomic_load(&connect_calls) == 0);
    wait_for_lookups();

    // the connection is refused, or doesn't answer in the time that is left
    set_fakes(10, 0, 5, ECONNREFUSED, false);
    offline_probe_run("lobby.example", 3074, 1500, &fake_ops, &result);
    CHECK(result.verdict == Verdict_Offline);
    CHECK(atomic_load(&connect_calls) == 1);
    CHECK(strstr(result.reason, "refused") != NULL);
    set_fakes(100, 0, 5000, 0, false);
    before = offline_now_ms();
    offline_probe_run("lobby.example", 3074, 400, &fake_ops, &result);
    took = offline_now_ms() - before;
    CHECK(result.verdict == Verdict_Offline);
    CHECK(atomic_load(&connect_timeout_ms) > 50 && atomic_load(&connect_timeout_ms) <= 310);
    CHECK(took >= 390 && took < 1200);
    CHECK(strstr(result.reason, "timed out") != NULL);
    // a connection that answers, but later than the limit
    set_fakes(10, 0, 500, 0, true);
    offline_probe_run("lobby.example", 3074, 300, &fake_ops, &result);
    CHECK(result.verdict == Verdict_Offline);
    CHECK(strstr(result.reason, "more than the 300 ms") != NULL);
    wait_for_lookups();
}

// a listening socket on 127.0.0.1, its port in *port
static int listen_on_loopback(int *port) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = v4("127.0.0.1", 0);
    socklen_t size = sizeof(address);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 4) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &size) != 0) {
        printf("FAIL: no listener on 127.0.0.1\n");
        failures++;
    }
    *port = ntohs(address.sin_port);
    return listener;
}

// a port on 127.0.0.1 nobody listens on
static int closed_loopback_port(void) {
    int port;
    int listener = listen_on_loopback(&port);
    close(listener);
    return port;
}

static void test_probe_on_loopback(void) {
    offline_probe_result_t result;
    int port;
    int listener = listen_on_loopback(&port);
    offline_probe_run("127.0.0.1", port, 1500, &offline_probe_system_ops, &result);
    CHECK(result.verdict == Verdict_Online);
    CHECK(result.connect_ms >= 0 && result.lookup_ms >= 0);
    close(listener);

    offline_probe_run("127.0.0.1", closed_loopback_port(), 1500, &offline_probe_system_ops, &result);
    CHECK(result.verdict == Verdict_Offline);
    CHECK(strstr(result.reason, "refused") != NULL);

    // 127.0.0.2 isn't this Mac's address: macOS sends it to the loopback, where nobody answers it (nothing leaves the
    // machine), so the connection runs into the limit
    uint64_t before = offline_now_ms();
    offline_probe_run("127.0.0.2", 9, 300, &offline_probe_system_ops, &result);
    uint64_t took = offline_now_ms() - before;
    CHECK(result.verdict == Verdict_Offline);
    CHECK(strstr(result.reason, "the connection to 127.0.0.2 (127.0.0.2) port 9 failed") != NULL);
    CHECK(strstr(result.reason, "timed out") != NULL);
    CHECK(took >= 290 && took < 1500);
    wait_for_lookups();
}

#pragma mark - the replacements

static _Atomic int system_calls;
static int last_socket;
static const void *last_address;

static int stand_in_connect(int socket, const struct sockaddr *address, socklen_t length) {
    (void)length;
    atomic_fetch_add(&system_calls, 1);
    last_socket = socket;
    last_address = address;
    return 42;
}
static int stand_in_connectx(int socket, const sa_endpoints_t *endpoints, sae_associd_t association,
                             unsigned int flags, const struct iovec *data, unsigned int data_count, size_t *length,
                             sae_connid_t *connection) {
    (void)association, (void)flags, (void)data, (void)data_count, (void)length, (void)connection;
    atomic_fetch_add(&system_calls, 1);
    last_socket = socket;
    last_address = endpoints;
    return 43;
}
static ssize_t stand_in_sendto(int socket, const void *buffer, size_t size, int flags, const struct sockaddr *address,
                               socklen_t length) {
    (void)buffer, (void)flags, (void)length;
    atomic_fetch_add(&system_calls, 1);
    last_socket = socket;
    last_address = address;
    return (ssize_t)size;
}
static ssize_t stand_in_sendmsg(int socket, const struct msghdr *message, int flags) {
    (void)flags;
    atomic_fetch_add(&system_calls, 1);
    last_socket = socket;
    last_address = message;
    return 44;
}
static int stand_in_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                                struct addrinfo **result) {
    (void)service, (void)hints;
    atomic_fetch_add(&system_calls, 1);
    last_address = node;
    *result = NULL;
    return 0;
}
static struct hostent stand_in_host;
static struct hostent *stand_in_gethostbyname(const char *name) {
    atomic_fetch_add(&system_calls, 1);
    last_address = name;
    return &stand_in_host;
}
static struct hostent *stand_in_gethostbyname2(const char *name, int family) {
    (void)family;
    atomic_fetch_add(&system_calls, 1);
    last_address = name;
    return &stand_in_host;
}

static const offline_system_t stand_ins = { stand_in_connect,     stand_in_connectx,     stand_in_sendto,
                                            stand_in_sendmsg,     stand_in_getaddrinfo,  stand_in_gethostbyname,
                                            stand_in_gethostbyname2 };

// each replacement once with a target outside the Mac: how many went to the system
static int outside_calls_let_through(void) {
    struct sockaddr_in public4 = v4("185.34.107.128", 3074);
    struct sockaddr_in6 public6 = v6("2a00:1450::1", 443);
    sa_endpoints_t endpoints = { .sae_dstaddr = (const struct sockaddr *)&public6, .sae_dstaddrlen = sizeof(public6) };
    struct msghdr message = { .msg_name = &public4, .msg_namelen = sizeof(public4) };
    struct addrinfo *found = (struct addrinfo *)&found;
    int before = atomic_load(&system_calls);
    int refusals = 0;
    errno = 0;
    if (offline_connect(7, (const struct sockaddr *)&public4, sizeof(public4)) == -1 && errno == ENETUNREACH)
        refusals++;
    errno = 0;
    if (offline_connectx(7, &endpoints, SAE_ASSOCID_ANY, 0, NULL, 0, NULL, NULL) == -1 && errno == ENETUNREACH)
        refusals++;
    errno = 0;
    if (offline_sendto(7, "x", 1, 0, (const struct sockaddr *)&public6, sizeof(public6)) == -1 && errno == ENETUNREACH)
        refusals++;
    errno = 0;
    if (offline_sendmsg(7, &message, 0) == -1 && errno == ENETUNREACH)
        refusals++;
    if (offline_getaddrinfo("ops3-pc-lobby.prod.demonware.net", "3074", NULL, &found) == EAI_NONAME && found == NULL)
        refusals++;
    h_errno = 0;
    if (offline_gethostbyname("ops3-pc-auth3.prod.demonware.net") == NULL && h_errno == HOST_NOT_FOUND)
        refusals++;
    h_errno = 0;
    if (offline_gethostbyname2("stun.us.demonware.net", AF_INET) == NULL && h_errno == HOST_NOT_FOUND)
        refusals++;
    int through = atomic_load(&system_calls) - before;
    if (through + refusals != 7)
        printf("FAIL: %d calls went through and %d were refused, of 7\n", through, refusals), failures++;
    return through;
}

// each replacement once with a target on this Mac or its network: how many went to the system
static int local_calls_let_through(void) {
    struct sockaddr_in private4 = v4("192.168.1.20", 3074);
    struct sockaddr_in6 link_local = v6("fe80::1", 3074);
    sa_endpoints_t endpoints = { .sae_dstaddr = (const struct sockaddr *)&link_local,
                                 .sae_dstaddrlen = sizeof(link_local) };
    struct sockaddr_in broadcast = v4("255.255.255.255", 28960);
    struct msghdr message = { .msg_name = &broadcast, .msg_namelen = sizeof(broadcast) };
    struct msghdr connected = { .msg_name = NULL, .msg_namelen = 0 };
    struct addrinfo *found = NULL;
    int before = atomic_load(&system_calls);
    CHECK(offline_connect(8, (const struct sockaddr *)&private4, sizeof(private4)) == 42);
    CHECK(last_socket == 8 && last_address == &private4);
    CHECK(offline_connectx(8, &endpoints, SAE_ASSOCID_ANY, 0, NULL, 0, NULL, NULL) == 43);
    CHECK(last_address == &endpoints);
    CHECK(offline_connectx(8, NULL, SAE_ASSOCID_ANY, 0, NULL, 0, NULL, NULL) == 43);
    CHECK(offline_sendto(8, "xy", 2, 0, (const struct sockaddr *)&broadcast, sizeof(broadcast)) == 2);
    CHECK(offline_sendto(8, "xyz", 3, 0, NULL, 0) == 3);
    CHECK(offline_sendmsg(8, &message, 0) == 44);
    CHECK(offline_sendmsg(8, &connected, 0) == 44);
    CHECK(offline_sendmsg(8, NULL, 0) == 44);
    CHECK(offline_getaddrinfo("localhost", "3074", NULL, &found) == 0);
    CHECK(offline_getaddrinfo(NULL, "3074", NULL, &found) == 0);
    CHECK(offline_getaddrinfo("127.0.0.1", NULL, NULL, &found) == 0);
    CHECK(offline_getaddrinfo("test-mac", NULL, NULL, &found) == 0);
    CHECK(offline_gethostbyname("Test-Mac.") == &stand_in_host);
    CHECK(offline_gethostbyname("adems-amazing-macbook-pro.local") == &stand_in_host);
    CHECK(offline_gethostbyname2("localhost", AF_INET6) == &stand_in_host);
    return atomic_load(&system_calls) - before;
}

// the notice bo3macfix.c shows when automatic decides "offline"
static _Atomic int notices_told;
static void count_notice(void) {
    atomic_fetch_add(&notices_told, 1);
}

static void test_replacements(void) {
    offline_system_t real = offline_system;
    offline_system = stand_ins;
    offline_mode_set_notice(count_notice);

    // off: everything goes to the system as before, and nothing is said
    capture_start();
    offline_mode_start(Offline_Off, true, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    CHECK(!offline_mode_cut());
    CHECK(outside_calls_let_through() == 7);
    CHECK(local_calls_let_through() == 15);
    const char *lines = capture_end();
    CHECK(strstr(lines, "offline mode") == NULL);
    CHECK(atomic_load(&notices_told) == 0);

    // always: nothing outside the Mac reaches the system; told once per target with the debug lines
    capture_start();
    offline_mode_start(Offline_Always, true, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    CHECK(offline_mode_cut());
    CHECK(outside_calls_let_through() == 0);
    CHECK(outside_calls_let_through() == 0);
    CHECK(local_calls_let_through() == 15);
    lines = capture_end();
    CHECK(count_of(lines, "BO3MacFix: offline mode: always (OfflineMode 1): the game gets no internet") == 1);
    CHECK(count_of(lines, "offline mode: refused a connection to 185.34.107.128:3074 (connect)\n") == 1);
    CHECK(count_of(lines, "offline mode: refused a connection to [2a00:1450::1]:443 (connectx)\n") == 1);
    CHECK(count_of(lines, "offline mode: refused a datagram to [2a00:1450::1]:443 (sendto)\n") == 1);
    CHECK(count_of(lines, "offline mode: refused a datagram to 185.34.107.128:3074 (sendmsg)\n") == 1);
    CHECK(count_of(lines, "offline mode: refused a name lookup of ops3-pc-lobby.prod.demonware.net (getaddrinfo)\n") ==
          1);
    CHECK(count_of(lines, "offline mode: refused a name lookup of ops3-pc-auth3.prod.demonware.net (gethostbyname)\n") ==
          1);
    CHECK(count_of(lines, "offline mode: refused a name lookup of stun.us.demonware.net (gethostbyname2)\n") == 1);
    CHECK(count_of(lines, "refused") == 7);
    CHECK(count_of(lines, "waited") == 0);
    CHECK(atomic_load(&notices_told) == 0);
    struct addrinfo *found = NULL;
    int through = atomic_load(&system_calls);
    capture_start();
    CHECK(offline_getaddrinfo("other-mac", NULL, NULL, &found) == EAI_NONAME);
    CHECK(offline_gethostbyname("test-mac2") == NULL);
    lines = capture_end();
    CHECK(atomic_load(&system_calls) == through);
    CHECK(count_of(lines, "refused a name lookup of other-mac (getaddrinfo)") == 1);

    // more targets than the notes have room for: told once that the others aren't listed
    capture_start();
    offline_mode_start(Offline_Always, true, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    for (int i = 0; i < Offline_Note_Slots + 6; i++) {
        char text[32];
        snprintf(text, sizeof(text), "8.8.%d.%d", i / 200, i % 200 + 1);
        struct sockaddr_in public4 = v4(text, 53);
        CHECK(offline_connect(9, (const struct sockaddr *)&public4, sizeof(public4)) == -1);
    }
    lines = capture_end();
    CHECK(count_of(lines, "refused a connection to") == Offline_Note_Slots);
    CHECK(count_of(lines, "offline mode: more than 64 refusals and waits, the others aren't listed\n") == 1);

    // without the debug lines only the mode's line
    capture_start();
    offline_mode_start(Offline_Always, false, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    CHECK(outside_calls_let_through() == 0);
    lines = capture_end();
    CHECK(count_of(lines, "offline mode") == 1);
    CHECK(count_of(lines, "refused") == 0);

    // a start forgets what was told
    capture_start();
    offline_mode_start(Offline_Always, true, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    CHECK(outside_calls_let_through() == 0);
    lines = capture_end();
    CHECK(count_of(lines, "refused") == 7);

    // automatic, the test answers after 150 ms: online, a call before that waits for it and goes through
    set_fakes(50, 0, 100, 0, false);
    capture_start();
    offline_mode_start(Offline_Automatic, true, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    uint64_t before = offline_now_ms();
    CHECK(outside_calls_let_through() == 7);
    uint64_t waited = offline_now_ms() - before;
    CHECK(waited >= 120 && waited < 1200);
    CHECK(!offline_mode_cut());
    lines = capture_end_after("online as usual");
    CHECK(count_of(lines, "offline mode: automatic (OfflineMode 2): asking lobby.example port 3074") == 1);
    CHECK(count_of(lines, "offline mode: automatic: online as usual, lobby.example (203.0.113.7) port 3074 answered") ==
          1);
    CHECK(count_of(lines, "waited for the automatic test's verdict on a connection to 185.34.107.128:3074 (connect): "
                          "") == 1);
    CHECK(count_of(lines, "then let through") == 1);
    CHECK(count_of(lines, "refused") == 0);
    CHECK(atomic_load(&notices_told) == 0);

    // automatic, the lobby server refuses: offline for the session
    set_fakes(20, 0, 20, ECONNREFUSED, false);
    capture_start();
    offline_mode_start(Offline_Automatic, true, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    CHECK(outside_calls_let_through() == 0);
    CHECK(offline_mode_cut());
    CHECK(local_calls_let_through() == 15);
    lines = capture_end_after("offline for this session, the connection to lobby.example");
    CHECK(count_of(lines, "offline mode: automatic: offline for this session, the connection to lobby.example") == 1);
    CHECK(count_of(lines, "then refused") == 1);
    CHECK(count_of(lines, "refused a connection to 185.34.107.128:3074 (connect)") == 1);
    CHECK(atomic_load(&notices_told) == 1);

    // automatic, the lookup never answers: offline at the limit
    set_fakes(900, 0, 0, 0, false);
    capture_start();
    before = offline_now_ms();
    offline_mode_start(Offline_Automatic, false, "test-mac", "slow.example", 3074, 300, &fake_ops);
    CHECK(outside_calls_let_through() == 0);
    waited = offline_now_ms() - before;
    CHECK(waited >= 280 && waited < 1500);
    lines = capture_end_after("didn't answer within 300 ms");
    CHECK(count_of(lines, "offline mode: automatic: offline for this session, the name lookup of slow.example didn't "
                          "answer within 300 ms") == 1);
    CHECK(count_of(lines, "refused") == 0);
    CHECK(atomic_load(&notices_told) == 2);

    // automatic, the test's own answer comes after the limit and the grace: the waiting call decides "offline" and
    // says so, and the late answer doesn't count
    set_fakes(10, 0, 1200, 0, true);
    capture_start();
    before = offline_now_ms();
    offline_mode_start(Offline_Automatic, true, "test-mac", "late.example", 3074, 200, &fake_ops);
    CHECK(outside_calls_let_through() == 0);
    waited = offline_now_ms() - before;
    CHECK(waited >= 200 + Offline_Verdict_Grace_Ms - 5 && waited < 1100);
    CHECK(offline_mode_cut());
    lines = capture_end_after("came too late to count");
    CHECK(count_of(lines, "offline mode: automatic: offline for this session, the test hadn't answered in time") == 1);
    CHECK(count_of(lines, "offline mode: automatic: the test's answer came too late to count: late.example") == 1);
    CHECK(count_of(lines, "online as usual") == 0);
    wait_for_lookups();
    CHECK(atomic_load(&notices_told) == 3);

    // a verdict before the notices are ready: told as soon as they are, once; a new start forgets it
    offline_mode_set_notice(NULL);
    set_fakes(5, EAI_NONAME, 0, 0, false);
    capture_start();
    offline_mode_start(Offline_Automatic, false, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    CHECK(offline_mode_cut());
    capture_end_after("offline for this session");
    CHECK(atomic_load(&notices_told) == 3);
    offline_mode_set_notice(count_notice);
    CHECK(atomic_load(&notices_told) == 4);
    offline_mode_set_notice(count_notice);
    CHECK(atomic_load(&notices_told) == 4);
    offline_mode_set_notice(NULL);
    capture_start();
    offline_mode_start(Offline_Automatic, false, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    CHECK(offline_mode_cut());
    capture_end_after("offline for this session");
    capture_start();
    offline_mode_start(Offline_Always, false, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    capture_end();
    offline_mode_set_notice(count_notice);
    CHECK(atomic_load(&notices_told) == 4);

    // after all that, off again is off
    capture_start();
    offline_mode_start(Offline_Off, true, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    CHECK(outside_calls_let_through() == 7);
    lines = capture_end();
    CHECK(strstr(lines, "offline mode") == NULL);

    offline_system = real;
}

// Off is the system as it was: every replacement hands each argument through as it came (pointers that point
// nowhere too: none of them is read), leaves errno and h_errno as they were before the call and as the system left
// them after it, and gives back the system's answer.
typedef struct {
    int calls, socket, flags, family, errno_seen, h_errno_seen;
    const void *pointer, *buffer, *data, *length_out, *connection, *service, *hints, *result;
    size_t size;
    socklen_t length;
    sae_associd_t association;
    unsigned int data_count;
} seen_t;
static seen_t seen;

static void saw(void) {
    seen.calls++;
    seen.errno_seen = errno;
    seen.h_errno_seen = h_errno;
    errno = 4321;
    h_errno = 765;
}
static int record_connect(int socket, const struct sockaddr *address, socklen_t length) {
    saw();
    seen.socket = socket, seen.pointer = address, seen.length = length;
    return 1001;
}
static int record_connectx(int socket, const sa_endpoints_t *endpoints, sae_associd_t association, unsigned int flags,
                           const struct iovec *data, unsigned int data_count, size_t *length, sae_connid_t *connection) {
    saw();
    seen.socket = socket, seen.pointer = endpoints, seen.association = association, seen.flags = (int)flags;
    seen.data = data, seen.data_count = data_count, seen.length_out = length, seen.connection = connection;
    return 1002;
}
static ssize_t record_sendto(int socket, const void *buffer, size_t size, int flags, const struct sockaddr *address,
                             socklen_t length) {
    saw();
    seen.socket = socket, seen.buffer = buffer, seen.size = size, seen.flags = flags, seen.pointer = address;
    seen.length = length;
    return 1003;
}
static ssize_t record_sendmsg(int socket, const struct msghdr *message, int flags) {
    saw();
    seen.socket = socket, seen.pointer = message, seen.flags = flags;
    return 1004;
}
static int record_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                              struct addrinfo **result) {
    saw();
    seen.pointer = node, seen.service = service, seen.hints = hints, seen.result = result;
    return 1005;
}
static struct hostent recorded_host;
static struct hostent *record_gethostbyname(const char *name) {
    saw();
    seen.pointer = name;
    return &recorded_host;
}
static struct hostent *record_gethostbyname2(const char *name, int family) {
    saw();
    seen.pointer = name, seen.family = family;
    return &recorded_host + 1;
}

// before a call: what errno and h_errno hold, and nothing seen yet
static void before_call(void) {
    memset(&seen, 0, sizeof(seen));
    errno = 1234;
    h_errno = 567;
}
#define Handed_Through(call_result, expected)                                                                         \
    CHECK((call_result) == (expected) && seen.calls == 1 && seen.errno_seen == 1234 && seen.h_errno_seen == 567 &&  \
          errno == 4321 && h_errno == 765)

static void test_off_hands_through(void) {
    offline_system_t real = offline_system;
    offline_system = (offline_system_t){ record_connect,     record_connectx,       record_sendto,
                                         record_sendmsg,     record_getaddrinfo,    record_gethostbyname,
                                         record_gethostbyname2 };
    capture_start();
    offline_mode_start(Offline_Off, true, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    // addresses that point nowhere: off must not read them
    const void *nowhere = (const void *)(uintptr_t)8, *elsewhere = (const void *)(uintptr_t)24;

    before_call();
    Handed_Through(offline_connect(31, (const struct sockaddr *)nowhere, 16), 1001);
    CHECK(seen.socket == 31 && seen.pointer == nowhere && seen.length == 16);

    size_t written = 0;
    sae_connid_t connection = 0;
    before_call();
    Handed_Through(offline_connectx(32, (const sa_endpoints_t *)nowhere, 77, 0x5a, (const struct iovec *)elsewhere, 3,
                                    &written, &connection),
                   1002);
    CHECK(seen.socket == 32 && seen.pointer == nowhere && seen.association == 77 && seen.flags == 0x5a &&
          seen.data == elsewhere && seen.data_count == 3 && seen.length_out == &written &&
          seen.connection == &connection);

    before_call();
    Handed_Through(offline_sendto(33, elsewhere, 999, 0x80, (const struct sockaddr *)nowhere, 28), 1003);
    CHECK(seen.socket == 33 && seen.buffer == elsewhere && seen.size == 999 && seen.flags == 0x80 &&
          seen.pointer == nowhere && seen.length == 28);

    before_call();
    Handed_Through(offline_sendmsg(34, (const struct msghdr *)nowhere, 0x40), 1004);
    CHECK(seen.socket == 34 && seen.pointer == nowhere && seen.flags == 0x40);

    struct addrinfo *found = (struct addrinfo *)&found;
    before_call();
    Handed_Through(offline_getaddrinfo((const char *)nowhere, (const char *)elsewhere,
                                       (const struct addrinfo *)elsewhere, &found),
                   1005);
    CHECK(seen.pointer == nowhere && seen.service == elsewhere && seen.hints == elsewhere && seen.result == &found);
    CHECK(found == (struct addrinfo *)&found);

    before_call();
    Handed_Through(offline_gethostbyname((const char *)nowhere), &recorded_host);
    CHECK(seen.pointer == nowhere);

    before_call();
    Handed_Through(offline_gethostbyname2((const char *)nowhere, AF_INET6), &recorded_host + 1);
    CHECK(seen.pointer == nowhere && seen.family == AF_INET6);

    CHECK(strstr(capture_end(), "offline mode") == NULL);
    offline_system = real;
}

// the real system calls through the replacements, on this Mac only
static void test_replacements_on_loopback(void) {
    capture_start();
    offline_mode_start(Offline_Always, false, "test-mac", "lobby.example", 3074, 1500, &fake_ops);
    capture_end();
    int port;
    int listener = listen_on_loopback(&port);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = v4("127.0.0.1", port);
    CHECK(offline_connect(client, (const struct sockaddr *)&to, sizeof(to)) == 0);
    close(client);
    close(listener);

    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval patience = { .tv_sec = 1 };
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof(patience));
    struct sockaddr_in at = v4("127.0.0.1", 0);
    socklen_t size = sizeof(at);
    bind(receiver, (struct sockaddr *)&at, sizeof(at));
    getsockname(receiver, (struct sockaddr *)&at, &size);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(offline_sendto(sender, "ping", 4, 0, (const struct sockaddr *)&at, sizeof(at)) == 4);
    char got[8] = { 0 };
    CHECK(recv(receiver, got, sizeof(got), 0) == 4 && memcmp(got, "ping", 4) == 0);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *found = NULL;
    CHECK(offline_getaddrinfo("127.0.0.1", "3074", &hints, &found) == 0 && found != NULL);
    if (found != NULL)
        freeaddrinfo(found);
    close(sender);
    close(receiver);
}

// what the library's constructor does with BO3MACFIX_OFFLINEPROBE, on this Mac only
static void test_init(void) {
    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%d", closed_loopback_port());
    capture_start();
    offline_mode_init(Offline_Automatic, false, target);
    CHECK(offline_mode_cut());
    const char *lines = capture_end_after("offline for this session, the connection to 127.0.0.1");
    char asking[96];
    snprintf(asking, sizeof(asking), "asking 127.0.0.1 port %s", strrchr(target, ':') + 1);
    CHECK(strstr(lines, asking) != NULL);
    CHECK(strstr(lines, "offline for this session, the connection to 127.0.0.1 (127.0.0.1)") != NULL);

    int port;
    int listener = listen_on_loopback(&port);
    snprintf(target, sizeof(target), "127.0.0.1:%d", port);
    capture_start();
    offline_mode_init(Offline_Automatic, false, target);
    CHECK(!offline_mode_cut());
    lines = capture_end_after("online as usual, 127.0.0.1");
    CHECK(strstr(lines, "online as usual, 127.0.0.1 (127.0.0.1)") != NULL);
    close(listener);

    capture_start();
    offline_mode_init(Offline_Off, true, NULL);
    CHECK(!offline_mode_cut());
    offline_mode_init(Offline_Always, false, "nonsense");
    CHECK(offline_mode_cut());
    lines = capture_end();
    CHECK(count_of(lines, "offline mode") == 1);
}

int main(void) {
    test_addresses();
    test_names();
    test_notes();
    test_state();
    test_probe();
    test_probe_on_loopback();
    test_replacements();
    test_off_hands_through();
    test_replacements_on_loopback();
    test_init();
    if (failures == 0)
        printf("test_offline_mode: all passed\n");
    return failures == 0 ? 0 : 1;
}
