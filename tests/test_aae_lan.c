// aae_lan.c: the plugin's packets and rules for joining a game on the local network, and the host's listener talking
// to the test over the loopback (nothing here leaves the machine).
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/aae_plugin.c"
#include "../source/aae_lan.c"

// ---- what the code asks of the rest of BO3MacFix
static char mod_folder[4096];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
#define A_Person 0x0110000100000001ull // 76561197960265729, the first individual account
static uint64_t game_steam_id = A_Person;
CSteamID steam_id(void) {
    return (CSteamID){ game_steam_id };
}
static uint64_t test_steam_id(void) {
    return game_steam_id;
}

// ---- the mod with the plugin
static char loaded_name[Mod_Name_Size + 8], plugin_file[4096 + 64];
static uint64_t plugin_time = 5000;
static uint64_t test_plugin_now(void) {
    return plugin_time;
}
static void set_mod(bool loaded) {
    memset(loaded_name, 0, sizeof(loaded_name));
    if (loaded)
        snprintf(loaded_name, sizeof(loaded_name), "%s", strrchr(mod_folder, '/') + 1);
    plugin_time += Plugin_Look_Ms;
}

// ---- the made-up game
static int32_t game_network_mode = 1;

static uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) {
    return a << 24 | b << 16 | c << 8 | d;
}

static void test_ids(void) {
    CHECK(aae_lan_is_individual(A_Person));
    CHECK(aae_lan_is_individual(0x011000010fffffffull) && aae_lan_is_individual(0x01100001ffffffffull));
    // the whole account number counts, its high half too
    CHECK(aae_lan_is_individual(0x0110000100010000ull) && aae_lan_is_individual(0x0110000180000000ull));
    // the instance (bits 32 to 51) isn't looked at
    CHECK(aae_lan_is_individual(0x011fffff00000001ull) && aae_lan_is_individual(0x0110000000000001ull));
    // no account number
    CHECK(!aae_lan_is_individual(0x0110000100000000ull) && !aae_lan_is_individual(0));
    // other kinds of account: a game server (3), an anonymous game server (4), a clan (7), none (0)
    CHECK(!aae_lan_is_individual(0x0130000100000001ull) && !aae_lan_is_individual(0x0140000100000001ull) &&
          !aae_lan_is_individual(0x0170000100000001ull) && !aae_lan_is_individual(0x0100000100000001ull) &&
          !aae_lan_is_individual(0x0120000100000001ull) && !aae_lan_is_individual(0x01f0000100000001ull));
    // other universes: none (0), beta (2), internal (3), dev (4), and the high bits
    CHECK(!aae_lan_is_individual(0x0010000100000001ull) && !aae_lan_is_individual(0x0210000100000001ull) &&
          !aae_lan_is_individual(0x0410000100000001ull) && !aae_lan_is_individual(0x1110000100000001ull) &&
          !aae_lan_is_individual(0x8110000100000001ull));
}

static void test_private(void) {
    // the five ranges, at their ends and just beyond
    CHECK(aae_lan_is_private(ip(10, 0, 0, 0)) && aae_lan_is_private(ip(10, 255, 255, 255)));
    CHECK(!aae_lan_is_private(ip(9, 255, 255, 255)) && !aae_lan_is_private(ip(11, 0, 0, 0)));
    CHECK(aae_lan_is_private(ip(172, 16, 0, 0)) && aae_lan_is_private(ip(172, 31, 255, 255)));
    CHECK(!aae_lan_is_private(ip(172, 15, 255, 255)) && !aae_lan_is_private(ip(172, 32, 0, 0)));
    CHECK(aae_lan_is_private(ip(192, 168, 0, 0)) && aae_lan_is_private(ip(192, 168, 255, 255)));
    CHECK(!aae_lan_is_private(ip(192, 167, 255, 255)) && !aae_lan_is_private(ip(192, 169, 0, 0)));
    CHECK(aae_lan_is_private(ip(127, 0, 0, 0)) && aae_lan_is_private(ip(127, 255, 255, 255)));
    CHECK(!aae_lan_is_private(ip(126, 255, 255, 255)) && !aae_lan_is_private(ip(128, 0, 0, 0)));
    CHECK(aae_lan_is_private(ip(169, 254, 0, 0)) && aae_lan_is_private(ip(169, 254, 255, 255)));
    CHECK(!aae_lan_is_private(ip(169, 253, 255, 255)) && !aae_lan_is_private(ip(169, 255, 0, 0)));
    // what isn't one of them: nothing, the internet, the carrier's shared range, multicast, broadcast
    CHECK(!aae_lan_is_private(0) && !aae_lan_is_private(ip(8, 8, 8, 8)) && !aae_lan_is_private(ip(100, 64, 0, 1)) &&
          !aae_lan_is_private(ip(224, 0, 0, 1)) && !aae_lan_is_private(0xffffffff) &&
          !aae_lan_is_private(ip(192, 0, 2, 1)) && !aae_lan_is_private(ip(1, 10, 0, 0)) &&
          !aae_lan_is_private(ip(1, 192, 168, 0)));
}

// a query as the plugin's joiner sends it
static const uint8_t a_query[AAE_LAN_PACKET] = { 'A', 'A', 'E', 'L', 'A', 'N', '3', '5', 0x78, 0x56, 0x34, 0x12,
                                                 1,   0,   0,   0,   0,   0,   0,   0,   0,    0,    0,    0 };

static void test_packets(void) {
    uint32_t number = 0;
    CHECK(aae_lan_read_query(a_query, sizeof(a_query), &number) && number == 0x12345678);
    // what is behind the kind isn't looked at
    uint8_t packet[AAE_LAN_PACKET + 1];
    memcpy(packet, a_query, sizeof(a_query));
    memset(packet + 16, 0xee, 8);
    number = 0;
    CHECK(aae_lan_read_query(packet, AAE_LAN_PACKET, &number) && number == 0x12345678);
    // exactly 24 bytes
    packet[AAE_LAN_PACKET] = 0;
    number = 7;
    CHECK(!aae_lan_read_query(packet, AAE_LAN_PACKET + 1, &number) && !aae_lan_read_query(packet, 23, &number) &&
          !aae_lan_read_query(packet, 0, &number) && number == 7);
    // each byte of the magic, and the kind
    for (int i = 0; i < 8; i++) {
        memcpy(packet, a_query, sizeof(a_query));
        packet[i] ^= 0x20;
        CHECK(!aae_lan_read_query(packet, AAE_LAN_PACKET, &number));
    }
    const uint32_t kinds[] = { 0, 2, 0x101, 0x10001, 0x1000001, 0xffffffff };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        memcpy(packet, a_query, sizeof(a_query));
        write_u32(packet + 12, kinds[i]);
        CHECK(!aae_lan_read_query(packet, AAE_LAN_PACKET, &number) && number == 7);
    }
    // the reply, byte for byte
    uint8_t reply[AAE_LAN_PACKET];
    memset(reply, 0xcc, sizeof(reply));
    aae_lan_reply(reply, 0x12345678, 0x0110000102030405ull);
    static const uint8_t expected[AAE_LAN_PACKET] = { 'A',  'A',  'E',  'L',  'A',  'N',  '3',  '5',
                                                      0x78, 0x56, 0x34, 0x12, 2,    0,    0,    0,
                                                      0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0x10, 0x01 };
    CHECK(memcmp(reply, expected, sizeof(reply)) == 0);
}

static void test_answers(void) {
    const uint32_t home = ip(192, 168, 1, 20);
    CHECK(aae_lan_answers(home, 1, A_Person) && aae_lan_answers(home, 2, A_Person));
    CHECK(aae_lan_answers(ip(127, 0, 0, 1), 1, A_Person) && aae_lan_answers(ip(10, 1, 2, 3), 2, A_Person));
    // not in LAN or online mode
    CHECK(!aae_lan_answers(home, 0, A_Person) && !aae_lan_answers(home, 3, A_Person) &&
          !aae_lan_answers(home, -1, A_Person));
    // from beyond the local network
    CHECK(!aae_lan_answers(ip(8, 8, 8, 8), 1, A_Person) && !aae_lan_answers(ip(100, 64, 0, 1), 2, A_Person));
    // without a person's Steam ID to give
    CHECK(!aae_lan_answers(home, 1, 0) && !aae_lan_answers(home, 1, 0x0140000100000001ull) &&
          !aae_lan_answers(home, 1, 0x0110000100000000ull));
}

// ---- the listener against the loopback
static uint16_t port_of(int fd) {
    struct sockaddr_in at;
    socklen_t size = sizeof(at);
    getsockname(fd, (struct sockaddr *)&at, &size);
    return ntohs(at.sin_port);
}
// a socket on the loopback that the test sends from and receives on, with a short receive timeout
static int asker(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct timeval wait = { .tv_sec = 0, .tv_usec = 100000 };
    struct sockaddr_in at = { .sin_len = sizeof(at), .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    bind(fd, (struct sockaddr *)&at, sizeof(at));
    return fd;
}
static void send_to(int from, uint16_t port, const void *data, size_t size) {
    struct sockaddr_in to = { .sin_len = sizeof(to), .sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    CHECK(sendto(from, data, size, 0, (struct sockaddr *)&to, sizeof(to)) == (ssize_t)size);
}
// what the asker got back, -1 for nothing; who sent it in *from_port
static ssize_t received(int fd, uint8_t *buffer, size_t size, uint16_t *from_port) {
    struct sockaddr_in from;
    socklen_t from_size = sizeof(from);
    ssize_t length = recvfrom(fd, buffer, size, 0, (struct sockaddr *)&from, &from_size);
    if (from_port != NULL)
        *from_port = length >= 0 ? ntohs(from.sin_port) : 0;
    return length;
}

static void test_listener(void) {
    int host = listener_open(INADDR_LOOPBACK, 0), client = asker();
    CHECK(host >= 0 && client >= 0);
    uint16_t port = port_of(host);
    // the receive timeout is the plugin's
    struct timeval wait;
    socklen_t wait_size = sizeof(wait);
    CHECK(getsockopt(host, SOL_SOCKET, SO_RCVTIMEO, &wait, &wait_size) == 0 && wait.tv_sec == 1 && wait.tv_usec == 0);
    // nobody else can have the port
    errno = 0;
    CHECK(listener_open(INADDR_LOOPBACK, port) == -1 && errno == EADDRINUSE);
    // a query gets the reply, from the port it was sent to
    uint8_t buffer[64];
    uint16_t from_port = 0;
    game_network_mode = 1;
    send_to(client, port, a_query, sizeof(a_query));
    listener_serve(host);
    ssize_t length = received(client, buffer, sizeof(buffer), &from_port);
    uint8_t expected[AAE_LAN_PACKET];
    aae_lan_reply(expected, 0x12345678, A_Person);
    CHECK(length == AAE_LAN_PACKET && memcmp(buffer, expected, sizeof(expected)) == 0 && from_port == port);
    // online mode too, with the Steam ID the game has now
    game_network_mode = 2;
    game_steam_id = 0x0110000155667788ull;
    send_to(client, port, a_query, sizeof(a_query));
    listener_serve(host);
    aae_lan_reply(expected, 0x12345678, 0x0110000155667788ull);
    CHECK(received(client, buffer, sizeof(buffer), NULL) == AAE_LAN_PACKET &&
          memcmp(buffer, expected, sizeof(expected)) == 0);
    game_steam_id = A_Person;
    // nothing in offline mode, without a person's Steam ID, or for what isn't the query
    game_network_mode = 0;
    send_to(client, port, a_query, sizeof(a_query));
    listener_serve(host);
    CHECK(received(client, buffer, sizeof(buffer), NULL) == -1);
    game_network_mode = 1;
    game_steam_id = 0x0140000100000001ull;
    send_to(client, port, a_query, sizeof(a_query));
    listener_serve(host);
    CHECK(received(client, buffer, sizeof(buffer), NULL) == -1);
    game_steam_id = A_Person;
    uint8_t longer[AAE_LAN_PACKET + 1] = { 0 };
    memcpy(longer, a_query, sizeof(a_query));
    send_to(client, port, longer, sizeof(longer));
    listener_serve(host);
    CHECK(received(client, buffer, sizeof(buffer), NULL) == -1);
    send_to(client, port, a_query, AAE_LAN_PACKET - 1);
    listener_serve(host);
    CHECK(received(client, buffer, sizeof(buffer), NULL) == -1);
    uint8_t reply_back[AAE_LAN_PACKET];
    aae_lan_reply(reply_back, 1, A_Person);
    send_to(client, port, reply_back, sizeof(reply_back));
    listener_serve(host);
    CHECK(received(client, buffer, sizeof(buffer), NULL) == -1);
    // and after all that, a query still gets its reply
    send_to(client, port, a_query, sizeof(a_query));
    listener_serve(host);
    CHECK(received(client, buffer, sizeof(buffer), NULL) == AAE_LAN_PACKET);
    // with nothing there, it comes back after the timeout
    listener_serve(host);
    close(host);
    close(client);
}

// the listener's loop: open while the mod is loaded and there is a Steam ID, closed otherwise, giving up on a port
// that can't be had until the mod is loaded anew
static void test_turns(void) {
    int holder = listener_open(INADDR_LOOPBACK, 0);
    uint16_t taken = port_of(holder);
    listen_address = INADDR_LOOPBACK;
    listen_port = 0;
    listener_t listener = { .fd = -1 };
    set_mod(false);
    CHECK(listener_turn(&listener) && listener.fd == -1);
    set_mod(true);
    game_steam_id = 0;
    CHECK(listener_turn(&listener) && listener.fd == -1);
    game_steam_id = A_Person;
    CHECK(!listener_turn(&listener) && listener.fd >= 0 && !listener.gave_up);
    int open = listener.fd;
    CHECK(!listener_turn(&listener) && listener.fd == open);
    // the mod goes: closed
    set_mod(false);
    CHECK(listener_turn(&listener) && listener.fd == -1);
    // loaded again, with the port taken: given up, also on later turns
    listen_port = taken;
    set_mod(true);
    CHECK(listener_turn(&listener) && listener.fd == -1 && listener.gave_up);
    close(holder);
    CHECK(listener_turn(&listener) && listener.fd == -1 && listener.gave_up);
    // until the mod is loaded anew
    set_mod(false);
    CHECK(listener_turn(&listener) && !listener.gave_up);
    set_mod(true);
    CHECK(!listener_turn(&listener) && listener.fd >= 0 && port_of(listener.fd) == taken);
    // the Steam ID gone: closed too
    game_steam_id = 0x0110000100000000ull;
    CHECK(listener_turn(&listener) && listener.fd == -1);
    game_steam_id = A_Person;
    listen_port = AAE_LAN_PORT;
    listen_address = INADDR_ANY;
}

// ---- init
static void *started;
static void *nothing(void *unused) {
    return unused;
}
static int test_start(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument) {
    started = (void *)function;
    return pthread_create(thread, attributes, nothing, argument);
}

static void test_init(void) {
    const uint64_t base = 0x100000000;
    start_thread = test_start;
    aae_lan_init(base, 0);
    CHECK(started == NULL && lobby_network_mode == NULL);
    aae_lan_init(base, 1);
    CHECK(started == (void *)listener_main && lobby_network_mode == (const int32_t *)(base + ADDR_lobbyNetworkMode));
    CHECK(loaded_mod == (const char *)(base + ADDR_Mods_LoadedName));
    CHECK(listen_address == INADDR_ANY && listen_port == 27018 && local_id == steam_local_id);
    // the plugin's numbers
    CHECK(AAE_LAN_PORT == 27018 && AAE_LAN_PACKET == 24 && Listen_Wait_Ms == 1000 && Logged_Most == 32);
}

int main(void) {
    char template[] = "/tmp/bo3macfix-lan-test-XXXXXX";
    if (mkdtemp(template) == NULL)
        return 2;
    snprintf(mod_folder, sizeof(mod_folder), "%s/2631943123", template);
    char command[4096 + 64];
    snprintf(command, sizeof(command), "mkdir -p '%s/T7Overcharged/plugins'", mod_folder);
    if (system(command) != 0)
        return 2;
    snprintf(plugin_file, sizeof(plugin_file), "%s/%s", mod_folder, Plugin_Path);
    close(open(plugin_file, O_CREAT | O_WRONLY, 0644));
    plugin_now_ms = test_plugin_now;

    test_init();
    debug = 1;
    loaded_mod = loaded_name;
    lobby_network_mode = &game_network_mode;
    local_id = test_steam_id;
    test_ids();
    test_private();
    test_packets();
    test_answers();
    test_listener();
    test_turns();

    snprintf(command, sizeof(command), "rm -rf '%s'", template);
    if (system(command) != 0)
        failures++;
    if (failures != 0) {
        printf("test_aae_lan: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_lan: all passed\n");
    return 0;
}
