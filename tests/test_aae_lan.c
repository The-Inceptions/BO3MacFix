// aae_lan.c: the plugin's packets and rules for joining a game on the local network, and the host's listener talking
// to the test over the loopback (nothing here leaves the machine).
#include <fcntl.h>
#include <stdatomic.h>
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

// ---- the hooks, and the game functions behind them
static struct {
    void *address, *replacement, **original;
} hooked[8];
static int hooks;
static struct {
    int connects, parses, pings, joins;
    char parsed[128];
    uint64_t ping_ip, ping_port, controller, xuid, join_type;
} game;
static void game_connect(void) {
    game.connects++;
}
static uint64_t game_parse(const char *text, void *address) {
    game.parses++;
    snprintf(game.parsed, sizeof(game.parsed), "%s", text);
    return address != NULL;
}
static uint64_t game_ping(uint64_t ip, uint64_t port) {
    game.pings++;
    game.ping_ip = ip, game.ping_port = port;
    return 0x77;
}
static uint64_t game_join(uint64_t controller, uint64_t xuid, uint64_t join_type) {
    game.joins++;
    game.controller = controller, game.xuid = xuid, game.join_type = join_type;
    return 0x55;
}
// the two ways a lobby message goes: the game server's networking and the player's own
static struct {
    int calls;
    uint64_t to, length;
    const uint8_t *packet;
    uint64_t answer;
} server_send, user_send;
static uint64_t game_server_send(uint64_t to, const uint8_t *packet, uint64_t length) {
    server_send.calls++;
    server_send.to = to, server_send.packet = packet, server_send.length = length;
    return server_send.answer;
}
static uint64_t game_user_send(uint64_t to, const uint8_t *packet, uint64_t length) {
    user_send.calls++;
    user_send.to = to, user_send.packet = packet, user_send.length = length;
    return user_send.answer;
}
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 8)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    if (out_origin_func == (void **)&orig_LanJoin_Connect)
        *out_origin_func = (void *)game_connect;
    else if (out_origin_func == (void **)&orig_LanJoin_ParseAddress)
        *out_origin_func = (void *)game_parse;
    else if (out_origin_func == (void **)&orig_LanJoin_PingServer)
        *out_origin_func = (void *)game_ping;
    else if (out_origin_func == (void **)&orig_LanJoin_Join)
        *out_origin_func = (void *)game_join;
    else if (out_origin_func == (void **)&orig_LanJoin_SendLobbyPacket)
        *out_origin_func = (void *)game_server_send;
    return 0;
}
static bool was_hooked(uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 8; i++)
        if (hooked[i].address == (void *)(0x100000000 + (uint64_t)place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
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
static int32_t game_network_mode = 1, game_screen = 410;
static int game_playlist_version(void) {
    return 1230005;
}
static int game_ffotd_version(void) {
    return 42;
}
static const uint64_t game_user_xuid = A_Person;
static const uint64_t *game_users[2] = { &game_user_xuid, NULL };
static uint64_t test_now = 1000000;
static uint64_t test_clock(void) {
    return test_now;
}

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

// ---- the joiner's side: the rules
static void test_query(void) {
    uint8_t packet[AAE_LAN_PACKET];
    memset(packet, 0xcc, sizeof(packet));
    aae_lan_query(packet, 0x12345678);
    CHECK(memcmp(packet, a_query, sizeof(a_query)) == 0);
    // the number: the low half of the time in ms, the connect's count in the upper half, the plugin's constant
    CHECK(aae_lan_number(0, 0) == 0xaabbef35u);
    CHECK(aae_lan_number(0x123456789abcdef0ull, 0) == (0x9abcdef0u ^ 0xaabbef35u));
    CHECK(aae_lan_number(0, 1) == (0x10000u ^ 0xaabbef35u));
    CHECK(aae_lan_number(0, 0x12345) == (0x23450000u ^ 0xaabbef35u));
    CHECK(aae_lan_number(0x1000, 3) == (0x31000u ^ 0xaabbef35u));
}

static void test_reply(void) {
    const uint32_t host = ip(192, 168, 1, 20), number = 0x12345678;
    uint8_t reply[AAE_LAN_PACKET];
    uint64_t id = 0;
    aae_lan_reply(reply, number, 0x0110000187654321ull);
    CHECK(aae_lan_read_reply(reply, AAE_LAN_PACKET, host, 27018, host, 27018, number, &id) &&
          id == 0x0110000187654321ull);
    // from another address or port, of another length or number: not the reply, and nothing is given
    id = 7;
    CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET, ip(192, 168, 1, 21), 27018, host, 27018, number, &id));
    CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET, host, 27019, host, 27018, number, &id));
    CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET, host, 27018, host, 27019, number, &id));
    CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET + 1, host, 27018, host, 27018, number, &id));
    CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET - 1, host, 27018, host, 27018, number, &id));
    CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET, host, 27018, host, 27018, number + 1, &id));
    CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET, host, 27018, host, 27018, number ^ 0x10000, &id));
    CHECK(id == 7);
    // each byte of the magic, and the kind: a query sent back isn't a reply
    for (int i = 0; i < 8; i++) {
        aae_lan_reply(reply, number, A_Person);
        reply[i] ^= 0x01;
        CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET, host, 27018, host, 27018, number, &id));
    }
    const uint32_t kinds[] = { 0, 1, 3, 0x102, 0x10002, 0x1000002 };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        aae_lan_reply(reply, number, A_Person);
        write_u32(reply + 12, kinds[i]);
        CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET, host, 27018, host, 27018, number, &id));
    }
    // the Steam ID must be a person's
    aae_lan_reply(reply, number, 0x0140000100000001ull);
    CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET, host, 27018, host, 27018, number, &id));
    aae_lan_reply(reply, number, 0x0110000100000000ull);
    CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET, host, 27018, host, 27018, number, &id));
    aae_lan_reply(reply, number, 0x0210000100000001ull);
    CHECK(!aae_lan_read_reply(reply, AAE_LAN_PACKET, host, 27018, host, 27018, number, &id) && id == 7);
}

static void test_split(void) {
    char address[AAE_LAN_ADDRESS];
    uint64_t id = 0;
    // decimal
    CHECK(aae_lan_split("192.168.1.20:27017@76561197960265729", address, &id) && id == A_Person &&
          strcmp(address, "192.168.1.20:27017") == 0);
    // hexadecimal after 0x or 0X, with small and capital letters
    id = 0;
    CHECK(aae_lan_split("10.0.0.5@0x0110000100000001", address, &id) && id == A_Person &&
          strcmp(address, "10.0.0.5") == 0);
    CHECK(aae_lan_split("10.0.0.5@0X011000010000aBcD", address, &id) && id == 0x011000010000abcdull);
    CHECK(aae_lan_split("10.0.0.5@0x110000109abcdef", address, &id) && id == 0x0110000109abcdefull);
    // leading zeros, and nothing before the "@"
    CHECK(aae_lan_split("h@076561197960265729", address, &id) && id == A_Person && strcmp(address, "h") == 0);
    CHECK(aae_lan_split("@76561197960265729", address, &id) && address[0] == '\0');
    // what is no person's Steam ID, or no number: nothing is taken off or given
    const char *not_ones[] = {
        "1.2.3.4@1", "1.2.3.4@0x1", "1.2.3.4@0", "1.2.3.4@85568397215006721" /* a game server's */,
        "1.2.3.4@0x0130000100000001", "1.2.3.4@7656119796026572x", "1.2.3.4@0x011000010000000g",
        "1.2.3.4@76561197960265729 ", "1.2.3.4@ 76561197960265729", "1.2.3.4@+76561197960265729", "1.2.3.4@-1",
        "1.2.3.4@", "1.2.3.4@0x", "1.2.3.4@0X", "1.2.3.4", "", "1.2.3.4@76561197960265729@5",
        "1.2.3.4@x76561197960265729", "1.2.3.4@0b1", "1.2.3.4@7656\xb5", "1.2.3.4@0xffffffffffffffff",
        // beyond 64 bits, though what would be left of them is a person's
        "1.2.3.4@18523305271669817345", "1.2.3.4@0x10110000100000001",
        // hexadecimal digits in a decimal number
        "1.2.3.4@7656119796026572a",
    };
    for (size_t i = 0; i < sizeof(not_ones) / sizeof(not_ones[0]); i++) {
        id = 9;
        memset(address, 'z', sizeof(address));
        CHECK(!aae_lan_split(not_ones[i], address, &id) && id == 9 && address[0] == 'z');
        if (id != 9 || address[0] != 'z')
            printf("  (with %s)\n", not_ones[i]);
    }
    // the largest number that fits, and one more digit
    CHECK(!aae_lan_split("a@18446744073709551615", address, &id) && !aae_lan_split("a@184467440737095516150", address, &id));
    // 95 characters before the "@" at most
    char long_text[200];
    memset(long_text, 'a', 95);
    strcpy(long_text + 95, "@76561197960265729");
    CHECK(aae_lan_split(long_text, address, &id) && strlen(address) == 95 && id == A_Person);
    memset(long_text, 'a', 96);
    strcpy(long_text + 96, "@76561197960265729");
    CHECK(!aae_lan_split(long_text, address, &id));
}

static void test_cache(void) {
    aae_lan_join_t join = { 0 };
    uint64_t id = 5;
    CHECK(!aae_lan_cached(&join, ip(10, 0, 0, 1), 1000, &id) && id == 5);
    // a Steam ID is kept for 5 s, no answer for 0.6 s
    aae_lan_cache(&join, ip(10, 0, 0, 1), A_Person, 1000);
    CHECK(join.cached[0].address == ip(10, 0, 0, 1) && join.cached[0].id == A_Person && join.cached[0].expires == 6000);
    CHECK(aae_lan_cached(&join, ip(10, 0, 0, 1), 5999, &id) && id == A_Person);
    CHECK(!aae_lan_cached(&join, ip(10, 0, 0, 1), 6000, &id) && !aae_lan_cached(&join, ip(10, 0, 0, 2), 1000, &id));
    aae_lan_cache(&join, ip(10, 0, 0, 2), 0, 1000);
    CHECK(join.cached[1].address == ip(10, 0, 0, 2) && join.cached[1].id == 0 && join.cached[1].expires == 1600);
    id = 5;
    CHECK(aae_lan_cached(&join, ip(10, 0, 0, 2), 1599, &id) && id == 0);
    CHECK(!aae_lan_cached(&join, ip(10, 0, 0, 2), 1600, &id));
    // an address's own entry is taken again
    aae_lan_cache(&join, ip(10, 0, 0, 2), A_Person, 1100);
    CHECK(join.cached[1].id == A_Person && join.cached[1].expires == 6100 && join.cached[2].address == 0);
    // an expired entry ahead of it is taken first, and found first
    aae_lan_cache(&join, ip(10, 0, 0, 2), 0, 6050);
    CHECK(join.cached[0].address == ip(10, 0, 0, 2) && join.cached[0].expires == 6650 &&
          join.cached[1].address == ip(10, 0, 0, 2) && join.cached[1].id == A_Person);
    id = 5;
    CHECK(aae_lan_cached(&join, ip(10, 0, 0, 2), 6060, &id) && id == 0);
    // one that expires right now counts as expired
    aae_lan_cache(&join, ip(10, 0, 0, 3), A_Person, 6100);
    CHECK(join.cached[1].address == ip(10, 0, 0, 3));
    // all eight in use: the first of those that expire first goes
    aae_lan_join_t full = { 0 };
    for (int i = 0; i < AAE_LAN_CACHED; i++)
        aae_lan_cache(&full, ip(10, 1, 0, i + 1), A_Person, i == 3 || i == 5 ? 2000 : 2100);
    for (int i = 0; i < AAE_LAN_CACHED; i++)
        CHECK(full.cached[i].address == ip(10, 1, 0, i + 1));
    aae_lan_cache(&full, ip(10, 2, 0, 1), A_Person, 2500);
    CHECK(full.cached[3].address == ip(10, 2, 0, 1) && full.cached[5].address == ip(10, 1, 0, 6));
    aae_lan_cache(&full, ip(10, 2, 0, 2), A_Person, 2500);
    CHECK(full.cached[5].address == ip(10, 2, 0, 2) && full.cached[3].address == ip(10, 2, 0, 1));
}

static void test_ping(void) {
    aae_lan_join_t j = { 0 };
    const uint32_t home = ip(192, 168, 1, 20);
    // before any connect: nothing is the target, though the address pinged is kept
    CHECK(!aae_lan_ping(&j, home, 5000) && !j.matched && j.target == 0 && j.pinged == home);
    aae_lan_connect(&j, 10000);
    CHECK(j.attempt == 1 && j.started == 10000 && j.target == 0 && j.pinged == 0 && !j.matched);
    // the first address of a local network pinged in the first 10 s is the target, and its host is asked
    CHECK(!aae_lan_ping(&j, ip(8, 8, 8, 8), 10100) && !j.matched && j.target == 0 && j.pinged == ip(8, 8, 8, 8));
    CHECK(aae_lan_ping(&j, home, 19999) && j.matched && j.target == home && j.requested == home &&
          j.requested_attempt == 1);
    // another address of a local network within the 10 s doesn't become the target instead
    aae_lan_join_t other = j;
    CHECK(!aae_lan_ping(&other, ip(192, 168, 1, 30), 10500) && !other.matched && other.target == home);
    // pinged again: still the target, its host asked already
    CHECK(!aae_lan_ping(&j, home, 20000) && j.matched && j.requested == home);
    // another address isn't the target
    CHECK(!aae_lan_ping(&j, ip(192, 168, 1, 21), 20001) && !j.matched && j.pinged == ip(192, 168, 1, 21));
    // being asked, or asked already: not again; otherwise again
    j.requested = 0;
    j.asking = home;
    CHECK(!aae_lan_ping(&j, home, 99999) && j.matched && j.requested == 0);
    j.asking = 0;
    CHECK(aae_lan_ping(&j, home, 99999) && j.matched && j.requested == home);
    // 90 s after the start it's over
    j.requested = 0;
    CHECK(!aae_lan_ping(&j, home, 100000) && !j.matched && j.requested == 0 && j.pinged == home);
    // no target after 10 s
    aae_lan_connect(&j, 200000);
    CHECK(!aae_lan_ping(&j, home, 210000) && !j.matched && j.target == 0);
    CHECK(aae_lan_ping(&j, ip(10, 0, 0, 1), 209999) && j.target == ip(10, 0, 0, 1));
    // a clock that went back
    aae_lan_connect(&j, 300000);
    j.requested = 0;
    CHECK(!aae_lan_ping(&j, home, 299999) && !j.matched && j.target == 0);
    // what the cache keeps, even that the host didn't answer, isn't asked for again until it expires
    aae_lan_connect(&j, 400000);
    aae_lan_cache(&j, ip(10, 0, 0, 9), 0, 400000);
    CHECK(!aae_lan_ping(&j, ip(10, 0, 0, 9), 400100) && j.matched && j.requested == 0);
    CHECK(aae_lan_ping(&j, ip(10, 0, 0, 9), 400600) && j.requested == ip(10, 0, 0, 9) && j.requested_attempt == 4);
    // a Steam ID given with the address: the host isn't asked; one of no person isn't one
    aae_lan_connect(&j, 500000);
    CHECK(j.given_id == 0);
    j.requested = 0;
    j.given_id = A_Person;
    CHECK(!aae_lan_ping(&j, ip(10, 0, 0, 10), 500100) && j.matched && j.target == ip(10, 0, 0, 10) && j.requested == 0);
    aae_lan_connect(&j, 600000);
    j.given_id = 0x0140000100000001ull;
    CHECK(aae_lan_ping(&j, ip(10, 0, 0, 11), 600100) && j.requested == ip(10, 0, 0, 11));
    // a count of connects gone round to 0 has no window for the ping
    j.attempt = 0xffffffff;
    j.requested = 0;
    aae_lan_connect(&j, 700000);
    CHECK(j.attempt == 0 && !aae_lan_ping(&j, ip(10, 0, 0, 12), 700100) && !j.matched && j.target == 0);
    // the address 0 is nobody's target; a connect begun at time 0 has no window
    aae_lan_connect(&j, 700200);
    CHECK(!aae_lan_ping(&j, 0, 700300) && !j.matched && j.target == 0);
    aae_lan_connect(&j, 0);
    CHECK(!aae_lan_ping(&j, home, 100) && !j.matched && j.target == 0);
    // a connect clears what the last one found
    j.attempt = 5;
    j.matched = true, j.target = home, j.pinged = home, j.given_id = A_Person;
    aae_lan_connect(&j, 800000);
    CHECK(j.attempt == 6 && !j.matched && j.target == 0 && j.pinged == 0 && j.given_id == 0 && j.started == 800000);
    // what asking needs: an address of a local network
    CHECK(aae_lan_ask(&j, 0, 1, 800000) == 0 && aae_lan_ask(&j, ip(8, 8, 8, 8), 1, 800000) == 0);
    CHECK(aae_lan_ask(&j, home, 9, 800000) == 2 && j.requested == home && j.requested_attempt == 9);
    CHECK(aae_lan_ask(&j, home, 10, 800000) == 1 && j.requested_attempt == 9);
}

#define A_Server 0x0140000100001234ull // an anonymous game server's Steam ID, as Steam gives a LAN host's
static aae_lan_plan_t plan_after_ping(aae_lan_join_t *j, uint32_t address, uint64_t now, int mode, uint64_t server,
                                      int join_type) {
    aae_lan_ping(j, address, now);
    return aae_lan_plan(j, now + 100, mode, server, join_type);
}

static void test_plan(void) {
    aae_lan_join_t j = { 0 };
    const uint32_t home = ip(192, 168, 1, 20);
    aae_lan_connect(&j, 10000);
    aae_lan_plan_t plan = plan_after_ping(&j, home, 10100, 1, A_Server, 0);
    CHECK(plan.window && plan.swap && plan.wait && plan.join_type == 4 && plan.id == 0 && plan.attempt == 1 &&
          plan.target == home && !j.matched);
    // the match is used up: the next Join is the game's
    plan = aae_lan_plan(&j, 10300, 1, A_Server, 0);
    CHECK(!plan.window && !plan.swap && !plan.wait && plan.join_type == 0);
    // online mode too; in other modes the Join is the game's, but in the window
    plan = plan_after_ping(&j, home, 10400, 2, A_Server, 0);
    CHECK(plan.swap && plan.wait && plan.join_type == 4);
    const int other_modes[] = { 0, 3, -1 };
    for (int i = 0; i < 3; i++) {
        plan = plan_after_ping(&j, home, 10400, other_modes[i], A_Server, 0);
        CHECK(plan.window && !plan.swap && !plan.wait && plan.join_type == 0 && plan.id == 0);
    }
    // a join type other than 0 stays
    plan = plan_after_ping(&j, home, 10400, 1, A_Server, 1);
    CHECK(plan.swap && plan.join_type == 1);
    // the host is waited for when the server's Steam ID is a game server's (3) or an anonymous one's (4)
    plan = plan_after_ping(&j, home, 10400, 1, 0x0130000100001234ull, 0);
    CHECK(plan.swap && plan.wait);
    const uint64_t others[] = { A_Person, 0x0120000100001234ull, 0x0150000100001234ull, 0x0170000100001234ull, 0 };
    for (int i = 0; i < 5; i++) {
        plan = plan_after_ping(&j, home, 10400, 1, others[i], 0);
        CHECK(plan.swap && !plan.wait && plan.id == 0 && plan.join_type == 4);
    }
    // a Steam ID given with the address is used, not waited for
    j.given_id = A_Person + 5;
    plan = plan_after_ping(&j, home, 10400, 1, A_Server, 0);
    CHECK(plan.swap && !plan.wait && plan.id == A_Person + 5);
    plan = plan_after_ping(&j, home, 10400, 3, A_Server, 0);
    CHECK(!plan.swap && plan.id == 0);
    j.given_id = 0x0110000100000000ull;
    plan = plan_after_ping(&j, home, 10400, 1, A_Server, 0);
    CHECK(plan.swap && plan.wait && plan.id == 0);
    j.given_id = 0;
    // the window: up to 90 s after the start
    aae_lan_ping(&j, home, 10400);
    plan = aae_lan_plan(&j, 99999, 1, A_Server, 0);
    CHECK(plan.window && plan.swap);
    aae_lan_ping(&j, home, 10400);
    plan = aae_lan_plan(&j, 100000, 1, A_Server, 0);
    CHECK(!plan.window && !plan.swap);
    aae_lan_ping(&j, home, 10400);
    plan = aae_lan_plan(&j, 9999, 1, A_Server, 0);
    CHECK(!plan.window);
    // the address last pinged has to be the target, which there has to be (made-up states)
    j.matched = true, j.pinged = ip(192, 168, 1, 21);
    CHECK(!aae_lan_plan(&j, 10500, 1, A_Server, 0).window);
    j.matched = true, j.pinged = 0, j.target = 0;
    CHECK(!aae_lan_plan(&j, 10500, 1, A_Server, 0).window);
    // a target not of a local network gets no swap
    j.matched = true, j.pinged = j.target = ip(8, 8, 8, 8);
    plan = aae_lan_plan(&j, 10500, 1, A_Server, 0);
    CHECK(plan.window && !plan.swap);
    // no start, no window; the count of connects doesn't matter to the Join
    j.matched = true, j.pinged = j.target = home, j.started = 0;
    CHECK(!aae_lan_plan(&j, 10500, 1, A_Server, 0).window);
    j.matched = true, j.started = 10000, j.attempt = 0;
    plan = aae_lan_plan(&j, 10500, 1, A_Server, 0);
    CHECK(plan.window && plan.swap && plan.attempt == 0);
}

static void test_waited(void) {
    aae_lan_join_t j = { 0 };
    const uint32_t home = ip(192, 168, 1, 20);
    aae_lan_connect(&j, 10000);
    aae_lan_plan_t plan = plan_after_ping(&j, home, 10100, 1, A_Server, 0);
    uint64_t id = 9;
    CHECK(!aae_lan_waited(&j, &plan, 10300, &id) && id == 9 && !aae_lan_dropped(&j, &plan));
    aae_lan_cache(&j, home, A_Person, 10300);
    CHECK(aae_lan_waited(&j, &plan, 10301, &id) && id == A_Person);
    CHECK(!aae_lan_waited(&j, &plan, 15300, &id));
    // the host didn't answer: over, with nothing
    aae_lan_cache(&j, home, 0, 15300);
    id = 9;
    CHECK(aae_lan_waited(&j, &plan, 15301, &id) && id == 0);
    // another connect, or another target: over, and the Join dropped
    aae_lan_join_t later = j;
    aae_lan_connect(&later, 16000);
    CHECK(aae_lan_waited(&later, &plan, 16001, &id) && aae_lan_dropped(&later, &plan));
    later = j;
    later.target = ip(192, 168, 1, 21);
    CHECK(aae_lan_waited(&later, &plan, 15400, &id) && aae_lan_dropped(&later, &plan));
    // another connect to the same host is another connect
    later = j;
    aae_lan_connect(&later, 16000);
    aae_lan_ping(&later, home, 16001);
    CHECK(later.target == plan.target && aae_lan_waited(&later, &plan, 16002, &id) && aae_lan_dropped(&later, &plan));
    // a Join out of the window is never dropped
    plan.window = false;
    CHECK(!aae_lan_dropped(&later, &plan));
}

static uint64_t elapsed_since(uint64_t start) {
    return uptime_ms() - start;
}

// ---- when the asking thread can't be started: nothing is left requested, and the Join doesn't wait
static int no_thread(pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *argument) {
    (void)thread, (void)attributes, (void)function, (void)argument;
    return EAGAIN;
}
static void test_no_thread(void) {
    start_thread = no_thread;
    now_ms = uptime_ms;
    set_mod(true);
    game_network_mode = 1;
    memset(&join, 0, sizeof(join));
    memset(&game, 0, sizeof(game));
    fake_LanJoin_Connect();
    fake_LanJoin_PingServer(ip(192, 168, 1, 20), 27017);
    CHECK(join.matched && join.target == ip(192, 168, 1, 20) && join.requested == 0 && !asker_started);
    uint64_t start = uptime_ms();
    CHECK(join_server(0, A_Server, 0, true) == 0x55 && game.xuid == A_Server && game.join_type == 4);
    CHECK(elapsed_since(start) < 50 && join.requested == 0 && !asker_started);
    now_ms = test_clock;
}

// ---- the joiner's side against a host of the test's own on the loopback
static int test_host = -1;
static _Atomic bool host_stops;
static void *host_main(void *unused) {
    while (!atomic_load(&host_stops))
        listener_serve(test_host);
    return unused;
}
// a host that answers one query and keeps it; its reply is longer by this many bytes
static int recording_socket = -1;
static uint8_t recorded[64];
static ssize_t recorded_length;
static size_t reply_extra;
static void *recording_main(void *unused) {
    struct sockaddr_in from;
    socklen_t from_size = sizeof(from);
    recorded_length = recvfrom(recording_socket, recorded, sizeof(recorded), 0, (struct sockaddr *)&from, &from_size);
    uint32_t number;
    uint8_t reply[AAE_LAN_PACKET + 8] = { 0 };
    if (aae_lan_read_query(recorded, (size_t)recorded_length, &number)) {
        aae_lan_reply(reply, number, A_Person);
        sendto(recording_socket, reply, AAE_LAN_PACKET + reply_extra, 0, (struct sockaddr *)&from, from_size);
    }
    return unused;
}
static void *connect_later(void *unused) {
    usleep(100000);
    fake_LanJoin_Connect();
    return unused;
}
static void test_asking(void) {
    now_ms = uptime_ms;
    start_thread = pthread_create;
    game_network_mode = 1;
    game_steam_id = A_Person;
    set_mod(true);
    memset(&join, 0, sizeof(join));
    memset(&game, 0, sizeof(game));
    test_host = listener_open(INADDR_LOOPBACK, 0);
    int silent = listener_open(INADDR_LOOPBACK, 0); // a port where nothing answers
    query_port = port_of(test_host);
    pthread_t host;
    CHECK(pthread_create(&host, NULL, host_main, NULL) == 0);
    const uint32_t loopback = ip(127, 0, 0, 1);
    char netadr[32];

    // connect, its address read, its server pinged: the host is asked
    fake_LanJoin_Connect();
    CHECK(game.connects == 1 && join.attempt == 1);
    CHECK(fake_LanJoin_ParseAddress("127.0.0.1:27017", netadr) == 1 && strcmp(game.parsed, "127.0.0.1:27017") == 0);
    CHECK(fake_LanJoin_PingServer(loopback, 27017) == 0x77 && game.pings == 1 && game.ping_ip == loopback &&
          game.ping_port == 27017);
    // the Join from the ping's answer waits for the host's Steam ID and joins it, directly
    uint64_t start = uptime_ms();
    CHECK(join_server(0, A_Server, 0, true) == 0x55 && game.joins == 1 && game.controller == 0 &&
          game.xuid == A_Person && game.join_type == 4);
    CHECK(elapsed_since(start) < Answer_Wait_Ms);
    // the answer is kept: the next attempt has it at once
    fake_LanJoin_Connect();
    fake_LanJoin_ParseAddress("127.0.0.1:27017", netadr);
    fake_LanJoin_PingServer(loopback, 27017);
    pthread_mutex_lock(&lock);
    CHECK(join.requested == 0 && join.matched && join.attempt == 2);
    pthread_mutex_unlock(&lock);
    CHECK(join_server(1, A_Server, 0, true) == 0x55 && game.joins == 2 && game.controller == 1 &&
          game.xuid == A_Person && game.join_type == 4);
    // a Join from anywhere else is the game's
    CHECK(join_server(0, A_Server, 0, false) == 0x55 && game.xuid == A_Server && game.join_type == 0);
    // also while the ping's own Join is still to come, which then gets the match
    fake_LanJoin_Connect();
    fake_LanJoin_PingServer(loopback, 27017);
    CHECK(join_server(0, A_Server, 0, false) == 0x55 && game.xuid == A_Server && game.join_type == 0 && join.matched);
    CHECK(join_server(0, A_Server, 0, true) == 0x55 && game.xuid == A_Person && game.join_type == 4);
    // and so is one while the game is offline; online it's the plugin's
    game_network_mode = 0;
    fake_LanJoin_Connect();
    fake_LanJoin_PingServer(loopback, 27017);
    CHECK(join_server(0, A_Server, 0, true) == 0x55 && game.xuid == A_Server && game.join_type == 0);
    game_network_mode = 2;
    fake_LanJoin_Connect();
    fake_LanJoin_PingServer(loopback, 27017);
    CHECK(join_server(0, A_Server, 0, true) == 0x55 && game.xuid == A_Person && game.join_type == 4);
    game_network_mode = 1;

    // the Steam ID given with the address: nobody is asked, and it is used once
    fake_LanJoin_Connect();
    CHECK(fake_LanJoin_ParseAddress("127.0.0.1:27017@0x0110000100000099", netadr) == 1 &&
          strcmp(game.parsed, "127.0.0.1:27017") == 0 && join.given_id == 0x0110000100000099ull);
    CHECK(fake_LanJoin_ParseAddress("127.0.0.1:27017@1", netadr) == 1 && strcmp(game.parsed, "127.0.0.1:27017@1") == 0);
    fake_LanJoin_PingServer(loopback, 27017);
    CHECK(join.requested == 0 && join.matched);
    CHECK(join_server(0, A_Server, 0, true) == 0x55 && game.xuid == 0x0110000100000099ull && game.join_type == 4 &&
          join.given_id == 0);

    // a host that doesn't answer: two queries, then the server's Steam ID is joined, directly
    query_port = port_of(silent);
    fake_LanJoin_Connect();
    fake_LanJoin_ParseAddress("127.0.0.1:27017", netadr);
    pthread_mutex_lock(&lock);
    join.cached[0].expires = join.cached[1].expires = 0; // forget the answer of the host before
    pthread_mutex_unlock(&lock);
    start = uptime_ms();
    fake_LanJoin_PingServer(loopback, 27017);
    CHECK(join_server(0, A_Server, 0, true) == 0x55 && game.xuid == A_Server && game.join_type == 4);
    uint64_t took = elapsed_since(start);
    CHECK(took >= 2 * Reply_Wait_Ms - 20 && took < Answer_Wait_Ms + 100);
    uint64_t id = 9;
    pthread_mutex_lock(&lock);
    CHECK(aae_lan_cached(&join, loopback, uptime_ms(), &id) && id == 0);
    pthread_mutex_unlock(&lock);

    // the questioning itself: two queries, each waiting 140 ms, for a host that doesn't answer; one for one that does
    int tries = 0;
    start = uptime_ms();
    CHECK(ask_host(loopback, 1, &tries) == 0 && tries == 2);
    took = elapsed_since(start);
    CHECK(took >= 2 * Reply_Wait_Ms - 10 && took < 2 * Reply_Wait_Ms + 40);
    query_port = port_of(test_host);
    tries = 0;
    CHECK(ask_host(loopback, 1, &tries) == A_Person && tries == 1);
    // the query's number: the time, and the connect it is for
    recording_socket = listener_open(INADDR_LOOPBACK, 0);
    query_port = port_of(recording_socket);
    pthread_t recorder;
    CHECK(pthread_create(&recorder, NULL, recording_main, NULL) == 0);
    now_ms = test_clock;
    tries = 0;
    CHECK(ask_host(loopback, 7, &tries) == A_Person && tries == 1);
    now_ms = uptime_ms;
    pthread_join(recorder, NULL);
    uint8_t expected_query[AAE_LAN_PACKET];
    aae_lan_query(expected_query, aae_lan_number(test_now, 7));
    CHECK(recorded_length == AAE_LAN_PACKET && memcmp(recorded, expected_query, sizeof(expected_query)) == 0);
    close(recording_socket);
    // a reply longer than 24 bytes isn't the plugin's (its Windows fails one), though it begins like one
    recording_socket = listener_open(INADDR_LOOPBACK, 0);
    query_port = port_of(recording_socket);
    reply_extra = 1;
    CHECK(pthread_create(&recorder, NULL, recording_main, NULL) == 0);
    tries = 0;
    CHECK(ask_host(loopback, 7, &tries) == 0 && tries == 2);
    pthread_join(recorder, NULL);
    reply_extra = 0;
    close(recording_socket);
    query_port = port_of(silent);

    // nothing comes: the Join waits 350 ms (a host made up to be asked by nobody)
    fake_LanJoin_Connect();
    fake_LanJoin_ParseAddress("127.0.0.1:27017", netadr);
    pthread_mutex_lock(&lock);
    memset(join.cached, 0, sizeof(join.cached));
    aae_lan_ping(&join, loopback, uptime_ms());
    join.requested = 0, join.asking = loopback;
    pthread_mutex_unlock(&lock);
    start = uptime_ms();
    CHECK(join_server(0, A_Server, 0, true) == 0x55 && game.xuid == A_Server && game.join_type == 4);
    took = elapsed_since(start);
    CHECK(took >= Answer_Wait_Ms && took < Answer_Wait_Ms + 150);
    // and another connect while it waits drops the Join
    pthread_mutex_lock(&lock);
    aae_lan_ping(&join, loopback, uptime_ms());
    join.matched = true;
    pthread_mutex_unlock(&lock);
    fake_LanJoin_Connect();
    fake_LanJoin_ParseAddress("127.0.0.1:27017", netadr);
    pthread_mutex_lock(&lock);
    aae_lan_ping(&join, loopback, uptime_ms());
    join.requested = 0, join.asking = loopback;
    pthread_mutex_unlock(&lock);
    int joins = game.joins;
    pthread_t later;
    CHECK(pthread_create(&later, NULL, connect_later, NULL) == 0);
    start = uptime_ms();
    CHECK(join_server(0, A_Server, 0, true) == 0 && game.joins == joins);
    took = elapsed_since(start);
    pthread_join(later, NULL);
    CHECK(took >= 80 && took < Answer_Wait_Ms);
    pthread_mutex_lock(&lock);
    join.asking = 0;
    pthread_mutex_unlock(&lock);

    // without the mod everything is the game's
    query_port = port_of(test_host);
    set_mod(false);
    uint32_t attempt = join.attempt;
    fake_LanJoin_Connect();
    CHECK(join.attempt == attempt && game.connects == 11);
    CHECK(fake_LanJoin_ParseAddress("127.0.0.1:27017@76561197960265729", netadr) == 1 &&
          strcmp(game.parsed, "127.0.0.1:27017@76561197960265729") == 0 && join.given_id == 0);
    join.matched = false;
    fake_LanJoin_PingServer(loopback, 27017);
    CHECK(!join.matched && join.requested == 0);
    join.matched = true;
    CHECK(join_server(0, A_Server, 0, true) == 0x55 && game.xuid == A_Server && game.join_type == 0 && join.matched);
    join.matched = false;
    set_mod(true);

    atomic_store(&host_stops, true);
    pthread_join(host, NULL);
    close(test_host);
    close(silent);
    query_port = AAE_LAN_PORT;
    now_ms = test_clock;
}

// ---- lobby messages
static void test_lobby_message(void) {
    uint8_t packet[0x408] = { 0x28, 0, 0, 0, '1', 5 };
    CHECK(aae_lan_lobby_message(A_Person, packet, sizeof(packet)) && aae_lan_lobby_message(A_Person, packet, 8));
    // to a game server, or to nobody: the game's way
    CHECK(!aae_lan_lobby_message(A_Server, packet, sizeof(packet)) && !aae_lan_lobby_message(0, packet, 8) &&
          !aae_lan_lobby_message(0x0110000100000000ull, packet, 8));
    // too short, none, or not a lobby message
    CHECK(!aae_lan_lobby_message(A_Person, packet, 7) && !aae_lan_lobby_message(A_Person, NULL, 8));
    packet[4] = '2';
    CHECK(!aae_lan_lobby_message(A_Person, packet, 8));
    packet[4] = '1', packet[0] = 0x29;
    CHECK(!aae_lan_lobby_message(A_Person, packet, 8));
    packet[0] = 0x28, packet[1] = 1;
    CHECK(!aae_lan_lobby_message(A_Person, packet, 8));
    packet[1] = 0, packet[3] = 0x80;
    CHECK(!aae_lan_lobby_message(A_Person, packet, 8));
}

static void test_lobby_send(void) {
    uint8_t packet[0x408] = { 0x28, 0, 0, 0, '1', 5 };
    set_mod(true);
    // to a person: the player's own networking first, and that's all when it worked
    memset(&server_send, 0, sizeof(server_send)), memset(&user_send, 0, sizeof(user_send));
    user_send.answer = 0x101, server_send.answer = 0x77;
    CHECK(fake_LanJoin_SendLobbyPacket(A_Person, packet, 0x408) == 1 && user_send.calls == 1 &&
          server_send.calls == 0 && user_send.to == A_Person && user_send.packet == packet &&
          user_send.length == 0x408);
    // when it didn't (al 0, whatever else rax holds), the game server's as the game does it
    user_send.answer = 0x100;
    CHECK(fake_LanJoin_SendLobbyPacket(A_Person, packet, 0x408) == 0x77 && user_send.calls == 2 &&
          server_send.calls == 1 && server_send.to == A_Person && server_send.packet == packet &&
          server_send.length == 0x408);
    // to a game server, or not a lobby message: the game's way only
    user_send.answer = 1;
    CHECK(fake_LanJoin_SendLobbyPacket(A_Server, packet, 0x408) == 0x77 && user_send.calls == 2 &&
          server_send.calls == 2);
    packet[4] = '0';
    CHECK(fake_LanJoin_SendLobbyPacket(A_Person, packet, 0x408) == 0x77 && user_send.calls == 2 &&
          server_send.calls == 3);
    packet[4] = '1';
    // the length is the game's int: its upper half isn't looked at
    CHECK(fake_LanJoin_SendLobbyPacket(A_Person, packet, 0xffffffff00000408ull) == 1 && user_send.calls == 3 &&
          user_send.length == 0xffffffff00000408ull);
    CHECK(fake_LanJoin_SendLobbyPacket(A_Person, packet, 0x100000007ull) == 0x77 && user_send.calls == 3);
    // without the mod: the game's way
    set_mod(false);
    CHECK(fake_LanJoin_SendLobbyPacket(A_Person, packet, 0x408) == 0x77 && user_send.calls == 3 &&
          server_send.calls == 5);
    set_mod(true);
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
    CHECK(started == NULL && lobby_network_mode == NULL && hooks == 0);
    aae_lan_init(base, 1);
    CHECK(started == (void *)listener_main && lobby_network_mode == (const int32_t *)(base + ADDR_lobbyNetworkMode));
    CHECK(was_hooked(ADDR_LanJoin_SendLobbyPacket, (void *)fake_LanJoin_SendLobbyPacket,
                     (void **)&orig_LanJoin_SendLobbyPacket) &&
          UserNetworking_Send == (void *)(base + ADDR_LanJoin_UserSend));
    CHECK(hooks == 5 && was_hooked(ADDR_LanJoin_Connect, (void *)fake_LanJoin_Connect, (void **)&orig_LanJoin_Connect) &&
          was_hooked(ADDR_LanJoin_ParseAddress, (void *)fake_LanJoin_ParseAddress,
                     (void **)&orig_LanJoin_ParseAddress) &&
          was_hooked(ADDR_LanJoin_PingServer, (void *)fake_LanJoin_PingServer, (void **)&orig_LanJoin_PingServer) &&
          was_hooked(ADDR_LanJoin_Join, (void *)fake_LanJoin_Join, (void **)&orig_LanJoin_Join));
    CHECK(join_from_ping == (const void *)(base + ADDR_LanJoin_JoinFromPing) && now_ms == uptime_ms &&
          query_port == 27018);
    CHECK(lobby_ui_screen == (const int32_t *)(base + ADDR_lobbyUIScreen) &&
          PlaylistVersion_Get == (void *)(base + ADDR_PlaylistVersion_Get) &&
          FFOTDVersion_Get == (void *)(base + ADDR_FFOTDVersion_Get) &&
          local_users == (const uint64_t *const *)(base + ADDR_localUsers));
    CHECK(loaded_mod == (const char *)(base + ADDR_Mods_LoadedName));
    CHECK(listen_address == INADDR_ANY && listen_port == 27018 && local_id == steam_local_id);
    // the plugin's numbers
    CHECK(AAE_LAN_PORT == 27018 && AAE_LAN_PACKET == 24 && Listen_Wait_Ms == 1000 && Logged_Most == 32);
    CHECK(Tries == 2 && Reply_Wait_Ms == 140 && Answer_Wait_Ms == 350 && Window_Ms == 90000 && Target_Ms == 10000 &&
          Answer_Kept_Ms == 5000 && No_Answer_Kept_Ms == 600 && Join_Direct == 4 && Address_Most == 95 &&
          AAE_LAN_CACHED == 8 && AAE_LAN_ADDRESS == 96 && Number_Constant == 0xaabbef35);
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
    lobby_ui_screen = &game_screen;
    PlaylistVersion_Get = game_playlist_version;
    FFOTDVersion_Get = game_ffotd_version;
    local_users = game_users;
    UserNetworking_Send = game_user_send;
    local_id = test_steam_id;
    now_ms = test_clock;
    test_ids();
    test_private();
    test_packets();
    test_answers();
    test_listener();
    test_turns();
    test_query();
    test_reply();
    test_split();
    test_cache();
    test_ping();
    test_plan();
    test_waited();
    test_no_thread();
    test_asking();
    test_lobby_message();
    test_lobby_send();

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
