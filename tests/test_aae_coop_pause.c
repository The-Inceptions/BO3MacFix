// aae_coop_pause.c: the plugin's packets byte for byte, its host's and its client's rules, its reading of the game,
// the server command reader, four machines playing together over a made-up network that loses and reorders
// packets, and the hooks against a made-up game.
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
#include "../source/aae_coop_pause.c"

// ---- what the code asks of the rest of BO3MacFix
static char mod_folder[4096];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
static bool have_stubs = true;
static int stub_missing_for = -1;
bool prepatch_has_hook(uint32_t offset) {
    return have_stubs && (int)offset != stub_missing_for;
}

// ---- a made-up machine: its game as the rules see it
typedef struct {
    aae_coop_game_t game; // its context is the machine
    aae_coop_t coop;
    uint64_t *clock;
    char map[100];
    aae_coop_value_t value[AAE_Coop_Pause_Remote + 1];
    char remote_text[16];
    bool table;
    aae_coop_client_t clients[AAE_Coop_Slots + 2]; // two more than the plugin reads, to see whether it does
    int connection;
    bool connection_data;
    aae_coop_adr_t server;
    uint64_t server_id;
    int local_sock;
    uint64_t nonces;      // the next one it draws is this, then one more
    int lock_depth, lock_errors;
    int cl_paused_sets, coop_paused, coop_paused_sets, other_sets;
} machine_t;

static machine_t *of(const aae_coop_game_t *game) {
    return (machine_t *)game->context;
}
static uint64_t fake_now(const aae_coop_game_t *game) {
    return *of(game)->clock;
}
static void fake_value(const aae_coop_game_t *game, int which, aae_coop_value_t *value) {
    *value = of(game)->value[which];
}
static void fake_set_dvar(const aae_coop_game_t *game, const char *name, const char *value) {
    machine_t *m = of(game);
    if (m->lock_depth != 0)
        m->lock_errors++; // the plugin sets dvars only outside its lock
    if (strcmp(name, "cl_paused") == 0) {
        m->cl_paused_sets++;
        m->value[AAE_Coop_Cl_Paused] = (aae_coop_value_t){ .kind = AAE_Coop_Number, .number = atoi(value) };
    } else if (strcmp(name, "aae_coop_paused") == 0) {
        m->coop_paused_sets++;
        m->coop_paused = atoi(value);
    } else {
        m->other_sets++;
    }
}
static bool fake_client(const aae_coop_game_t *game, int index, aae_coop_client_t *client) {
    machine_t *m = of(game);
    if (!m->table || index < 0 || index >= AAE_Coop_Slots + 2)
        return false;
    *client = m->clients[index];
    return true;
}
static int fake_connection(const aae_coop_game_t *game) {
    return of(game)->connection;
}
static bool fake_server(const aae_coop_game_t *game, aae_coop_adr_t *adr, uint64_t *id) {
    machine_t *m = of(game);
    if (!m->connection_data)
        return false;
    *adr = m->server;
    *id = m->server_id;
    return true;
}
static int fake_local_sock(const aae_coop_game_t *game) {
    return of(game)->local_sock;
}
static uint64_t fake_nonce(const aae_coop_game_t *game) {
    return of(game)->nonces++;
}
static void fake_lock(const aae_coop_game_t *game) {
    if (of(game)->lock_depth++ != 0)
        of(game)->lock_errors++;
}
static void fake_unlock(const aae_coop_game_t *game) {
    if (--of(game)->lock_depth != 0)
        of(game)->lock_errors++;
}

static aae_coop_value_t whole(int64_t number) {
    return (aae_coop_value_t){ .kind = AAE_Coop_Number, .number = number };
}

// an address: kind 4 (another machine over Steam) unless said otherwise
static aae_coop_adr_t adr(uint8_t ip, uint16_t port, uint32_t kind, uint32_t net_id) {
    aae_coop_adr_t a = { { 0 } };
    a.bytes[0] = 10, a.bytes[1] = 0, a.bytes[2] = 0, a.bytes[3] = ip;
    memcpy(a.bytes + 4, &port, sizeof(port));
    memcpy(a.bytes + 8, &kind, sizeof(kind));
    memcpy(a.bytes + 12, &net_id, sizeof(net_id));
    return a;
}

// a machine in a map with sv_running as given (1 the host, 0 a client), the mod allowing the pause
static void machine_init(machine_t *m, uint64_t *clock, int running, uint64_t nonces) {
    memset(m, 0, sizeof(*m));
    m->game = (aae_coop_game_t){ .context = m, .now = fake_now, .value = fake_value, .set_dvar = fake_set_dvar,
                                 .client = fake_client, .connection = fake_connection, .server = fake_server,
                                 .local_sock = fake_local_sock, .nonce = fake_nonce, .lock = fake_lock,
                                 .unlock = fake_unlock };
    m->clock = clock;
    aae_coop_reset(&m->coop);
    snprintf(m->map, sizeof(m->map), "zm_theater");
    m->value[AAE_Coop_Map] = (aae_coop_value_t){ .kind = AAE_Coop_Text, .text = m->map };
    m->value[AAE_Coop_Sv_Running] = whole(running);
    m->value[AAE_Coop_Cl_Paused] = whole(0);
    m->value[AAE_Coop_Sv_Paused] = whole(0);
    m->value[AAE_Coop_Max_Clients] = whole(4);
    snprintf(m->remote_text, sizeof(m->remote_text), "1");
    m->value[AAE_Coop_Pause_Remote] = (aae_coop_value_t){ .kind = AAE_Coop_Text, .text = m->remote_text };
    m->table = running == 1;
    m->connection = running == 1 ? 9 : 9;
    m->local_sock = 0;
    m->nonces = nonces;
}
static void set_pause_remote(machine_t *m, const char *text) {
    snprintf(m->remote_text, sizeof(m->remote_text), "%s", text);
    m->value[AAE_Coop_Pause_Remote] = (aae_coop_value_t){ .kind = AAE_Coop_Text, .text = m->remote_text };
}
static void set_cl_paused(machine_t *m, int value) {
    m->value[AAE_Coop_Cl_Paused] = whole(value);
}
static int cl_paused_of(const machine_t *m) {
    return (int)m->value[AAE_Coop_Cl_Paused].number;
}

// a client of the host's server: state, address, id, bot
static void seat(machine_t *host, int slot, uint32_t state, aae_coop_adr_t where, uint64_t id, int bot) {
    host->clients[slot] = (aae_coop_client_t){ .state = state, .adr = where, .id = id, .bot = bot };
}

// the frames the rules run in, as the game's frame calls them; what they let the game run with
static int run_server_frame(machine_t *m, int msec) {
    return aae_coop_server_frame(&m->coop, &m->game, msec);
}
static int run_client_frame(machine_t *m, int local_client, int msec, float *frac) {
    aae_coop_frame_t frame;
    aae_coop_client_frame(&m->coop, &m->game, local_client, &msec, frac, &frame);
    aae_coop_client_frame_done(&m->coop, &m->game, &frame);
    return msec;
}
// a whole frame of the game: the server's (on the host), then the client's; returns the client's msec
static int game_frame(machine_t *m, int *server_msec) {
    float frac = 0.5f;
    int msec = 16;
    if (server_msec != NULL)
        *server_msec = run_server_frame(m, 16);
    return run_client_frame(m, 0, msec, &frac);
}

// ---- the packets, byte for byte (the plugin's own stores: 0x31504345414100e1, version 1, ...)

static void test_packets(void) {
    uint8_t out[AAE_Coop_Packet_Size];
    aae_coop_packet(out, 2, 0x0102030405060708ull, 0x1112131415161718ull, 0x2122232425262728ull, 1);
    static const uint8_t state[AAE_Coop_Packet_Size] = {
        0xe1, 0x00, 0x41, 0x41, 0x45, 0x43, 0x50, 0x31, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    CHECK(memcmp(out, state, sizeof(state)) == 0);
    aae_coop_packet(out, 1, 0, 0xffeeddccbbaa9988ull, 0, 0);
    static const uint8_t hello[AAE_Coop_Packet_Size] = {
        0xe1, 0x00, 0x41, 0x41, 0x45, 0x43, 0x50, 0x31, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    CHECK(memcmp(out, hello, sizeof(hello)) == 0);
    // a packet is one of these by its first 8 bytes alone, whatever follows and however long it is
    CHECK(aae_coop_ours(out, AAE_Coop_Packet_Size) && aae_coop_ours(out, 8) && !aae_coop_ours(out, 7));
    CHECK(!aae_coop_ours(NULL, AAE_Coop_Packet_Size) && aae_coop_ours(out, 0xffffffffu));
    for (int i = 0; i < 8; i++) {
        uint8_t other[AAE_Coop_Packet_Size];
        memcpy(other, out, sizeof(other));
        other[i] ^= 0x20;
        CHECK(!aae_coop_ours(other, sizeof(other)));
    }
}

// ---- what the plugin starts with (its data's initial values)

static void test_start(void) {
    aae_coop_t c;
    memset(&c, 0x5a, sizeof(c));
    aae_coop_reset(&c);
    CHECK(c.role == -1 && c.sock == -1 && c.sequence == 1 && c.coop_paused_written == -1 && c.last_cl_paused == -999 &&
          c.total == -1 && c.remote == -1 && c.remote_active == -1);
    CHECK(c.host_nonce == 0 && c.own_nonce == 0 && c.confirmed == 0 && c.state == 0 && c.mask == 0 && !c.timed_out &&
          c.frame_at == 0 && c.counted_at == 0 && !c.server_paused && !c.busy && !c.applied && c.news == 0 &&
          c.slots[17].nonce == 0 && !c.server_valid && c.paused_at == 0 && !c.remote_known);
}

// ---- reading the game the plugin's way

static void test_reading(void) {
    CHECK(aae_coop_in_map("zm_theater") && aae_coop_in_map("a") && aae_coop_in_map("Z9_-x"));
    CHECK(!aae_coop_in_map(NULL) && !aae_coop_in_map("") && !aae_coop_in_map("core_frontend"));
    CHECK(aae_coop_in_map("core_frontendx") && aae_coop_in_map("core_frontenD") && aae_coop_in_map("core"));
    CHECK(aae_coop_in_map("core_fronten") && aae_coop_in_map("xcore_frontend"));
    char name[80];
    memset(name, 'a', sizeof(name));
    name[64] = '\0';
    CHECK(aae_coop_in_map(name));
    name[64] = 'a', name[65] = '\0';
    CHECK(!aae_coop_in_map(name));
    // only letters, digits, _ and -
    const char *refused = " .!/:;<=>?@[\\]^`{|}~\x7f\x80\xff\t";
    for (const char *c = refused; *c != '\0'; c++) {
        snprintf(name, sizeof(name), "zm%cx", *c);
        CHECK(!aae_coop_in_map(name));
    }
    for (int c = 1; c < 256; c++) {
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                       c == '-';
        snprintf(name, sizeof(name), "%c", c);
        CHECK(aae_coop_in_map(name) == allowed);
    }
    aae_coop_value_t value = whole(1);
    CHECK(aae_coop_switch_on(&value));
    value = whole(2);
    CHECK(aae_coop_switch_on(&value));
    value = whole(0);
    CHECK(!aae_coop_switch_on(&value));
    value = whole(-1);
    CHECK(!aae_coop_switch_on(&value));
    value = (aae_coop_value_t){ .kind = AAE_Coop_Text, .text = "1" };
    CHECK(aae_coop_switch_on(&value));
    const char *texts[] = { "0", "2", "11", "1.0", " 1", "1 ", "", "true", NULL };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        value = (aae_coop_value_t){ .kind = AAE_Coop_Text, .text = texts[i] };
        CHECK(!aae_coop_switch_on(&value));
    }
    value = (aae_coop_value_t){ .kind = AAE_Coop_Other, .number = 1, .text = "1" };
    CHECK(!aae_coop_switch_on(&value));
    value = (aae_coop_value_t){ .kind = AAE_Coop_None, .number = 1, .text = "1" };
    CHECK(!aae_coop_switch_on(&value));

    // a dvar the plugin reads as a whole number: -1 none, -2 another kind; sv_running only 0 or 1
    uint64_t clock = 1000;
    machine_t m;
    machine_init(&m, &clock, 1, 1);
    CHECK(number(&m.game, AAE_Coop_Cl_Paused) == 0);
    m.value[AAE_Coop_Cl_Paused] = (aae_coop_value_t){ .kind = AAE_Coop_None };
    CHECK(number(&m.game, AAE_Coop_Cl_Paused) == -1);
    m.value[AAE_Coop_Cl_Paused] = (aae_coop_value_t){ .kind = AAE_Coop_Text, .text = "1" };
    CHECK(number(&m.game, AAE_Coop_Cl_Paused) == -2);
    m.value[AAE_Coop_Cl_Paused] = (aae_coop_value_t){ .kind = AAE_Coop_Other, .number = 1 };
    CHECK(number(&m.game, AAE_Coop_Cl_Paused) == -2);
    m.value[AAE_Coop_Cl_Paused] = whole(7);
    CHECK(number(&m.game, AAE_Coop_Cl_Paused) == 7);
    CHECK(sv_running(&m.game) == 1);
    m.value[AAE_Coop_Sv_Running] = whole(0);
    CHECK(sv_running(&m.game) == 0);
    m.value[AAE_Coop_Sv_Running] = whole(2);
    CHECK(sv_running(&m.game) == -1);
    m.value[AAE_Coop_Sv_Running] = whole(-1);
    CHECK(sv_running(&m.game) == -1);
    m.value[AAE_Coop_Sv_Running] = whole(-2);
    CHECK(sv_running(&m.game) == -1);
    m.value[AAE_Coop_Sv_Running] = (aae_coop_value_t){ .kind = AAE_Coop_None };
    CHECK(sv_running(&m.game) == -1);
    m.value[AAE_Coop_Sv_Running] = (aae_coop_value_t){ .kind = AAE_Coop_Text, .text = "1" };
    CHECK(sv_running(&m.game) == -1);
    // the map: only a text dvar counts
    CHECK(in_map(&m.game));
    m.value[AAE_Coop_Map].kind = AAE_Coop_Other;
    CHECK(!in_map(&m.game));
    m.value[AAE_Coop_Map].kind = AAE_Coop_None;
    CHECK(!in_map(&m.game));
}

// ---- the counts of the server's clients, and who may pause

static void test_counts(void) {
    uint64_t clock = 50000;
    machine_t m;
    machine_init(&m, &clock, 1, 1);
    // this machine's player, two of other machines (one connecting), a bot, one of another machine flagged as a bot
    seat(&m, 0, 5, adr(1, 100, 2, 0), 11, 0);
    seat(&m, 1, 5, adr(2, 200, 4, 0), 12, 0);
    seat(&m, 2, 3, adr(3, 300, 3, 0), 13, 0);
    seat(&m, 3, 5, adr(0, 0, 0, 0), 0, 1);
    m.value[AAE_Coop_Max_Clients] = whole(6);
    seat(&m, 4, 5, adr(5, 500, 4, 0), 15, 1);
    seat(&m, 5, 2, adr(6, 600, 4, 0), 16, 0);
    CHECK(counts(&m.coop, &m.game) == 5);
    CHECK(m.coop.remote == 2 && m.coop.remote_active == 1 && m.coop.local_active && m.coop.local_human);
    // kept for 16 ms
    seat(&m, 5, 5, adr(6, 600, 4, 0), 16, 0);
    clock += 15;
    CHECK(counts(&m.coop, &m.game) == 5 && m.coop.remote_active == 1);
    clock += 1;
    CHECK(counts(&m.coop, &m.game) == 6 && m.coop.remote == 3 && m.coop.remote_active == 2);
    // only as many as the count dvar says, 1 to 18
    m.value[AAE_Coop_Max_Clients] = whole(1);
    clock += 16;
    CHECK(counts(&m.coop, &m.game) == 1 && m.coop.remote == 0 && m.coop.local_human);
    m.value[AAE_Coop_Max_Clients] = whole(0);
    clock += 16;
    CHECK(counts(&m.coop, &m.game) == -1 && m.coop.remote == -1 && m.coop.remote_active == -1 &&
          !m.coop.local_active && !m.coop.local_human);
    m.value[AAE_Coop_Max_Clients] = whole(19);
    seat(&m, 18, 5, adr(19, 1900, 4, 0), 19, 0);
    clock += 16;
    CHECK(counts(&m.coop, &m.game) == -1 && m.coop.remote == -1);
    memset(&m.clients[18], 0, sizeof(m.clients[18]));
    m.value[AAE_Coop_Max_Clients] = whole(18);
    clock += 16;
    CHECK(counts(&m.coop, &m.game) == 6);
    // what -1 gives is kept too
    m.table = false;
    clock += 16;
    CHECK(counts(&m.coop, &m.game) == -1 && m.coop.remote == -1);
    m.table = true;
    clock += 15;
    CHECK(counts(&m.coop, &m.game) == -1);
    // kinds 0, 1, 2 and 5 aren't another machine
    memset(m.clients, 0, sizeof(m.clients));
    m.value[AAE_Coop_Max_Clients] = whole(4);
    for (uint32_t kind = 0; kind <= 5; kind++) {
        seat(&m, 0, 5, adr(1, 100, kind, 0), 11, 0);
        clock += 16;
        CHECK(counts(&m.coop, &m.game) == 1 && m.coop.remote == (kind == 3 || kind == 4));
    }
    // counted the first time even with the clock near 0 (it never is: it counts from the machine's start)
    machine_t early;
    uint64_t early_clock = 3;
    machine_init(&early, &early_clock, 1, 1);
    seat(&early, 0, 5, adr(1, 100, 2, 0), 11, 0);
    CHECK(counts(&early.coop, &early.game) == 1 && early.coop.counted_at == 3);
    // a human of this machine: active, not a bot, not of another machine's kind
    memset(m.clients, 0, sizeof(m.clients));
    seat(&m, 0, 5, adr(1, 100, 2, 0), 11, 1);
    clock += 16;
    CHECK(counts(&m.coop, &m.game) == 1 && m.coop.local_active && !m.coop.local_human);
    seat(&m, 0, 4, adr(1, 100, 2, 0), 11, 0);
    clock += 16;
    CHECK(counts(&m.coop, &m.game) == 1 && !m.coop.local_active && !m.coop.local_human);

    // alone: players of this machine only
    memset(m.clients, 0, sizeof(m.clients));
    m.value[AAE_Coop_Max_Clients] = whole(4);
    seat(&m, 0, 5, adr(1, 100, 2, 0), 11, 0);
    clock += 16;
    CHECK(alone(&m.coop, &m.game));
    seat(&m, 1, 5, adr(1, 101, 2, 1), 21, 0); // split screen: two of this machine
    clock += 16;
    CHECK(alone(&m.coop, &m.game));
    seat(&m, 2, 3, adr(2, 200, 4, 0), 12, 0); // one of another machine connecting
    clock += 16;
    CHECK(!alone(&m.coop, &m.game));
    memset(&m.clients[2], 0, sizeof(m.clients[2]));
    clock += 16;
    m.value[AAE_Coop_Sv_Running] = whole(0);
    CHECK(!alone(&m.coop, &m.game));
    // sv_running unreadable: alone when in a map at the last server frame and with a human here
    m.value[AAE_Coop_Sv_Running] = (aae_coop_value_t){ .kind = AAE_Coop_None };
    m.coop.sv_in_map = false;
    CHECK(!alone(&m.coop, &m.game));
    m.coop.sv_in_map = true;
    CHECK(alone(&m.coop, &m.game));
    seat(&m, 0, 5, adr(1, 100, 2, 0), 11, 1);
    seat(&m, 1, 5, adr(1, 101, 2, 1), 21, 1);
    clock += 16;
    CHECK(!alone(&m.coop, &m.game));
    m.value[AAE_Coop_Sv_Running] = whole(1);
    CHECK(alone(&m.coop, &m.game));
    seat(&m, 0, 5, adr(1, 100, 2, 0), 11, 0);
    seat(&m, 1, 5, adr(1, 101, 2, 1), 21, 0);
    clock += 16;
    m.coop.sv_in_map = true;
    m.value[AAE_Coop_Sv_Running] = whole(0);
    CHECK(!alone(&m.coop, &m.game)); // a client: never alone
    m.value[AAE_Coop_Sv_Running] = whole(1);
    memset(m.clients, 0, sizeof(m.clients));
    clock += 16;
    CHECK(!alone(&m.coop, &m.game)); // nobody at all
    seat(&m, 0, 5, adr(1, 100, 2, 0), 11, 0);
    clock += 16;
    snprintf(m.map, sizeof(m.map), "core_frontend");
    CHECK(!alone(&m.coop, &m.game));
    snprintf(m.map, sizeof(m.map), "zm_theater");

    // the host may pause: the mod allows it, this machine's server runs a map, somebody of another machine is in it
    seat(&m, 1, 5, adr(2, 200, 4, 0), 12, 0);
    clock += 16;
    CHECK(requested_host(&m.coop, &m.game));
    set_pause_remote(&m, "0");
    CHECK(!requested_host(&m.coop, &m.game));
    set_pause_remote(&m, "1");
    m.value[AAE_Coop_Sv_Running] = whole(0);
    CHECK(!requested_host(&m.coop, &m.game));
    m.value[AAE_Coop_Sv_Running] = (aae_coop_value_t){ .kind = AAE_Coop_None };
    CHECK(!requested_host(&m.coop, &m.game));
    m.value[AAE_Coop_Sv_Running] = whole(1);
    seat(&m, 1, 4, adr(2, 200, 4, 0), 12, 0);
    clock += 16;
    CHECK(!requested_host(&m.coop, &m.game));
    seat(&m, 1, 5, adr(2, 200, 4, 0), 12, 0);
    clock += 16;
    snprintf(m.map, sizeof(m.map), "core_frontend");
    CHECK(!requested_host(&m.coop, &m.game));
    snprintf(m.map, sizeof(m.map), "zm_theater");
    m.value[AAE_Coop_Max_Clients] = whole(0);
    clock += 16;
    CHECK(!requested_host(&m.coop, &m.game));
    CHECK(m.lock_errors == 0 && m.lock_depth == 0);
}

// ---- the host's rules

// a host with its own player in slot 0 and three clients of other machines; its frame ran
static void host_setup(machine_t *h, uint64_t *clock) {
    machine_init(h, clock, 1, 0x1000);
    seat(h, 0, 5, adr(1, 100, 2, 0), 11, 0);
    seat(h, 1, 5, adr(2, 200, 4, 0), 12, 0);
    seat(h, 2, 5, adr(3, 300, 4, 0), 13, 0);
    seat(h, 3, 5, adr(4, 400, 3, 0), 14, 0);
    game_frame(h, NULL);
}
// a hello or an acknowledgement from slot i's machine
static void from_slot(machine_t *h, int slot, uint32_t kind, uint64_t a, uint64_t nonce, uint64_t c, uint32_t d) {
    uint8_t packet[AAE_Coop_Packet_Size];
    aae_coop_packet(packet, kind, a, nonce, c, d);
    aae_coop_receive(&h->coop, &h->game, &h->clients[slot].adr, Host_Sock, packet, sizeof(packet));
}
static void hellos(machine_t *h) {
    for (int i = 1; i <= 3; i++)
        from_slot(h, i, Kind_Hello, 0, 0x500 + (uint64_t)i, 0, 0);
}
static void acks(machine_t *h) {
    for (int i = 1; i <= 3; i++)
        from_slot(h, i, Kind_Ack, h->coop.host_nonce, 0x500 + (uint64_t)i, h->coop.sequence, h->coop.state);
}

static void test_host_rules(void) {
    uint64_t clock = 100000;
    machine_t h;
    host_setup(&h, &clock);
    aae_coop_t *c = &h.coop;
    CHECK(c->role == 1 && c->sock == 4 && c->host_nonce == 0x1000 && c->own_nonce == 0);
    // the three of other machines take part (kind 3 too); this machine's player doesn't
    CHECK(c->mask == 0xe && c->slots[1].ok && c->slots[2].ok && c->slots[3].ok && !c->slots[0].ok);
    CHECK(c->sequence == 2 && c->changed_at == clock && c->state == 0);
    CHECK(memcmp(&c->slots[2].adr, &h.clients[2].adr, sizeof(aae_coop_adr_t)) == 0 && c->slots[2].id == 13);
    hellos(&h);
    CHECK(c->slots[1].nonce == 0x501 && c->slots[3].nonce == 0x503 && c->slots[1].heard == clock);
    // nothing asked for: nobody holds anything
    CHECK(!c->server_paused && !c->host_view_frozen && !c->waiting && !c->host_timed_out && c->paused_at == 0);

    // the host asks: its view is held at once, its server when everybody acknowledged
    set_cl_paused(&h, 1);
    clock += 16;
    int server;
    CHECK(game_frame(&h, &server) == 0 && server == 16);
    CHECK(c->state == 1 && c->sequence == 3 && c->host_view_frozen && c->waiting && !c->server_paused &&
          !c->all_confirmed);
    acks(&h);
    clock += 16;
    CHECK(game_frame(&h, &server) == 0 && server == 0 && c->confirmed == 3);
    CHECK(c->server_paused && c->all_confirmed && c->paused_at == clock && !c->host_timed_out);
    // once confirmed it stays so, however long the clients are silent (the plugin never takes it back)
    clock += 60000;
    host_rules(c, &h.game);
    CHECK(c->server_paused && c->confirmed == 3 && c->sequence == 3);
    // an acknowledgement counts while its client was heard within 4 s: 3999 ms after it, yes
    set_cl_paused(&h, 0);
    host_rules(c, &h.game);
    set_cl_paused(&h, 1);
    host_rules(c, &h.game);
    CHECK(c->sequence == 5 && c->confirmed == 3 && !c->server_paused);
    acks(&h);
    CHECK(c->slots[1].acked == 3); // the network side acts only while local client 0's frame ran within 5 s
    c->frame_at = clock;
    acks(&h);
    clock += 3999;
    host_rules(c, &h.game);
    CHECK(c->server_paused && c->confirmed == 5);
    // 4000 ms after it, no. (The 3.5 s give-up would come first here, as the change is as old as the
    // acknowledgement: the window only matters when the frames stall, so the change is put 100 ms back by hand.)
    set_cl_paused(&h, 0);
    host_rules(c, &h.game);
    set_cl_paused(&h, 1);
    host_rules(c, &h.game);
    c->frame_at = clock;
    acks(&h);
    clock += 4000;
    c->changed_at = clock - 100;
    host_rules(c, &h.game);
    CHECK(!c->server_paused && c->confirmed == 5 && c->sequence == 7 && !c->timed_out);
    // a hello keeps the client heard (the same nonce: nothing else changes)
    c->frame_at = clock;
    for (int i = 1; i <= 3; i++)
        from_slot(&h, i, Kind_Hello, 0, 0x500 + (uint64_t)i, 0, 0);
    host_rules(c, &h.game);
    CHECK(c->server_paused && c->slots[2].acked == 7 && c->confirmed == 7);
    // the host lets go: everybody runs again in the same frame
    set_cl_paused(&h, 0);
    host_rules(c, &h.game);
    CHECK(!c->server_paused && c->paused_at == 0);
    clock += 16;
    CHECK(game_frame(&h, &server) == 16 && server == 16);
    CHECK(c->state == 0 && c->sequence == 8 && !c->server_paused && !c->host_view_frozen && !c->waiting);

    // somebody comes or goes: the state goes anew and the server runs until everybody acknowledged it again
    set_cl_paused(&h, 1);
    clock += 16;
    game_frame(&h, NULL);
    acks(&h);
    clock += 16;
    CHECK(game_frame(&h, &server) == 0 && server == 0 && c->sequence == 9);
    seat(&h, 3, 0, adr(0, 0, 0, 0), 0, 0);
    clock += 16;
    CHECK(game_frame(&h, &server) == 0 && server == 16 && c->sequence == 10 && c->mask == 0x6);
    CHECK(c->slots[3].nonce == 0 && !c->slots[3].ok && c->changed_at == clock);
    acks(&h); // the one gone can't acknowledge any more; the two others do
    clock += 16;
    CHECK(game_frame(&h, &server) == 0 && server == 0);

    // a client that doesn't acknowledge within 3.5 s cancels the pause for everybody
    seat(&h, 3, 5, adr(4, 400, 3, 0), 14, 0);
    clock += 16;
    game_frame(&h, NULL);
    uint64_t changed = c->changed_at;
    CHECK(c->sequence == 11 && c->state == 1 && c->host_view_frozen);
    for (int i = 1; i <= 2; i++)
        from_slot(&h, i, Kind_Ack, c->host_nonce, 0x500 + (uint64_t)i, c->sequence, c->state);
    clock = changed + 3499;
    CHECK(game_frame(&h, &server) == 0 && server == 16 && c->state == 1 && !c->timed_out);
    clock = changed + 3500;
    CHECK(game_frame(&h, &server) == 16 && server == 16);
    CHECK(c->state == 0 && c->sequence == 12 && c->timed_out && c->host_timed_out && !c->host_view_frozen);
    CHECK(c->changed_at == clock && !c->waiting && !c->all_confirmed);
    CHECK(!aae_coop_can_pause(c, &h.game)); // until cl_paused or the mod's switch drops
    c->frame_at = clock;
    hellos(&h);
    acks(&h);
    host_rules(c, &h.game);
    CHECK(c->confirmed == c->sequence && !c->server_paused && c->state == 0 && !c->all_confirmed && !c->waiting);
    clock += 5000;
    game_frame(&h, NULL);
    CHECK(c->state == 0 && c->sequence == 12 && c->timed_out);
    set_cl_paused(&h, 0);
    clock += 16;
    game_frame(&h, NULL);
    CHECK(!c->timed_out && c->state == 0 && c->sequence == 12 && aae_coop_can_pause(c, &h.game));
    set_cl_paused(&h, 1);
    clock += 16;
    game_frame(&h, NULL);
    CHECK(c->state == 1 && c->sequence == 13 && !c->timed_out);
    // somebody comes or goes after a time out: it is forgotten, and the pause asked for again
    seat(&h, 3, 0, adr(0, 0, 0, 0), 0, 0);
    clock += 16;
    game_frame(&h, NULL);
    CHECK(!c->timed_out && c->state == 1 && c->mask == 0x6);
    clock += 3500;
    game_frame(&h, NULL);
    CHECK(c->timed_out && c->state == 0);
    seat(&h, 3, 5, adr(4, 400, 3, 0), 14, 0);
    clock += 16;
    game_frame(&h, NULL);
    CHECK(c->state == 1 && c->mask == 0xe);
    clock += 3500;
    game_frame(&h, NULL);
    CHECK(c->timed_out && c->state == 0);
    // the same with the switch: it drops, the time out is forgotten
    clock += 3500;
    game_frame(&h, NULL);
    CHECK(c->timed_out && c->state == 0);
    set_pause_remote(&h, "0");
    clock += 16;
    game_frame(&h, NULL);
    CHECK(!c->timed_out);
    set_pause_remote(&h, "1");
    clock += 16;
    game_frame(&h, NULL);
    CHECK(c->state == 1 && !c->timed_out);
    // confirmed in time: no time out, however long it stays
    hellos(&h);
    acks(&h);
    clock += 10;
    game_frame(&h, NULL);
    CHECK(c->confirmed == c->sequence);
    for (int i = 0; i < 4; i++) {
        clock += 3000;
        hellos(&h);
        host_rules(c, &h.game);
        CHECK(!c->timed_out && c->server_paused);
    }
    CHECK(h.lock_errors == 0 && h.lock_depth == 0);
}

static void test_host_slots(void) {
    uint64_t clock = 200000;
    machine_t h;
    host_setup(&h, &clock);
    aae_coop_t *c = &h.coop;
    hellos(&h);
    uint64_t sequence = c->sequence;
    // the same client again: its place is kept, with its nonce
    host_rules(c, &h.game);
    CHECK(c->sequence == sequence && c->slots[2].nonce == 0x502);
    // two bytes of the address the plugin doesn't compare
    h.clients[2].adr.bytes[6] = 0x77;
    host_rules(c, &h.game);
    CHECK(c->slots[2].nonce == 0x502 && c->slots[2].adr.bytes[6] == 0 && c->sequence == sequence);
    // another port, kind, net id, IP or id: another client in that place, which starts anew
    struct { int offset; uint8_t flip; } changes[] = { { 0, 1 }, { 3, 0x10 }, { 4, 1 }, { 5, 0x80 }, { 8, 7 },
                                                       { 12, 1 } };
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        aae_coop_adr_t kept = h.clients[2].adr;
        h.clients[2].adr.bytes[changes[i].offset] ^= changes[i].flip;
        host_rules(c, &h.game);
        CHECK(c->slots[2].nonce == 0 && c->slots[2].heard == 0 && c->slots[2].acked == 0 &&
              memcmp(&c->slots[2].adr, &h.clients[2].adr, sizeof(aae_coop_adr_t)) == 0);
        h.clients[2].adr = kept;
        host_rules(c, &h.game);
        from_slot(&h, 2, Kind_Hello, 0, 0x502, 0, 0);
        CHECK(c->slots[2].nonce == 0x502);
    }
    h.clients[2].id = 99;
    host_rules(c, &h.game);
    CHECK(c->slots[2].nonce == 0 && c->slots[2].id == 99 && c->slots[2].ok);
    // what can't take part: no id, a local net id above 4, this machine's kind, a bot; connecting is "not ready"
    sequence = c->sequence;
    h.clients[2].id = 0;
    host_rules(c, &h.game);
    CHECK(!c->slots[2].ok && c->slots[2].id == 0 && c->mask == 0xa && c->sequence == sequence + 1);
    CHECK(memcmp(&c->slots[2].adr, &h.clients[2].adr, sizeof(aae_coop_adr_t)) == 0);
    set_cl_paused(&h, 1);
    host_rules(c, &h.game);
    c->frame_at = clock;
    acks(&h);
    host_rules(c, &h.game);
    CHECK(!c->server_paused && c->confirmed != c->sequence); // the one without an id isn't ready
    set_cl_paused(&h, 0);
    host_rules(c, &h.game);
    h.clients[2].id = 13;
    h.clients[2].adr = adr(3, 300, 4, 5);
    host_rules(c, &h.game);
    CHECK(!c->slots[2].ok && c->slots[2].id == 13 && c->mask == 0xa);
    h.clients[2].adr = adr(3, 300, 4, 4);
    host_rules(c, &h.game);
    CHECK(c->slots[2].ok && c->mask == 0xe);
    h.clients[2].adr = adr(3, 300, 2, 0);
    host_rules(c, &h.game);
    CHECK(!c->slots[2].ok && c->slots[2].id == 0 && c->mask == 0xa); // not of another machine: no place at all
    h.clients[2].adr = adr(3, 300, 4, 0);
    h.clients[2].bot = 1;
    host_rules(c, &h.game);
    CHECK(!c->slots[2].ok && c->mask == 0xa);
    h.clients[2].bot = 0;
    h.clients[2].state = 4;
    host_rules(c, &h.game);
    CHECK(!c->slots[2].ok && c->slots[2].id == 0 && c->mask == 0xa);
    // a client connecting holds the server back, and the pause gives up after 3.5 s
    set_cl_paused(&h, 1);
    hellos(&h);
    host_rules(c, &h.game);
    acks(&h);
    host_rules(c, &h.game);
    CHECK(c->confirmed != c->sequence && !c->server_paused && c->host_view_frozen);
    h.clients[2].state = 5;
    host_rules(c, &h.game);
    from_slot(&h, 2, Kind_Hello, 0, 0x502, 0, 0);
    acks(&h);
    host_rules(c, &h.game);
    CHECK(c->server_paused);
    // nobody of another machine at all: asked for, the host's own rules hold the server (the frame's rules then
    // don't, as the host may not pause for anybody then)
    for (int i = 1; i <= 3; i++)
        h.clients[i].state = 0;
    host_rules(c, &h.game);
    CHECK(c->mask == 0 && c->server_paused && !c->host_view_frozen && !c->waiting);
    // only one connecting: no pause, and after 3.5 s it gives up
    h.clients[1].state = 3;
    host_rules(c, &h.game);
    CHECK(c->mask == 0 && !c->server_paused && c->state == 1);
    clock += 3500;
    host_rules(c, &h.game);
    CHECK(c->timed_out && c->state == 0 && !c->server_paused);
    // the count dvar: the table is read for 2 to 18
    machine_t g;
    host_setup(&g, &clock);
    g.value[AAE_Coop_Max_Clients] = whole(1);
    aae_coop_client_t own = g.clients[0];
    seat(&g, 0, 5, adr(9, 900, 4, 0), 99, 0);
    host_rules(&g.coop, &g.game);
    CHECK(g.coop.mask == 0);
    g.value[AAE_Coop_Max_Clients] = whole(2);
    host_rules(&g.coop, &g.game);
    CHECK(g.coop.mask == 0x3);
    g.clients[0] = own;
    g.value[AAE_Coop_Max_Clients] = whole(2);
    host_rules(&g.coop, &g.game);
    CHECK(g.coop.mask == 0x2);
    g.value[AAE_Coop_Max_Clients] = whole(19);
    host_rules(&g.coop, &g.game);
    CHECK(g.coop.mask == 0);
    g.value[AAE_Coop_Max_Clients] = whole(18);
    seat(&g, 17, 5, adr(18, 1800, 4, 0), 18, 0);
    host_rules(&g.coop, &g.game);
    CHECK(g.coop.mask == (0xeu | 1u << 17));
    g.table = false;
    host_rules(&g.coop, &g.game);
    CHECK(g.coop.mask == 0);
    // the host's nonce and socket: without them nothing is ever confirmed
    machine_t k;
    host_setup(&k, &clock);
    set_cl_paused(&k, 1);
    hellos(&k);
    host_rules(&k.coop, &k.game);
    acks(&k);
    k.coop.host_nonce = 0;
    host_rules(&k.coop, &k.game);
    CHECK(k.coop.confirmed != k.coop.sequence);
    k.coop.host_nonce = 0x1000;
    k.coop.sock = 5;
    host_rules(&k.coop, &k.game);
    CHECK(k.coop.confirmed != k.coop.sequence);
    k.coop.sock = -1;
    host_rules(&k.coop, &k.game);
    CHECK(k.coop.confirmed != k.coop.sequence);
    k.coop.sock = 4;
    host_rules(&k.coop, &k.game);
    CHECK(k.coop.confirmed == k.coop.sequence && k.coop.server_paused);
    // never heard: a new sequence isn't confirmed
    set_cl_paused(&k, 0);
    host_rules(&k.coop, &k.game);
    set_cl_paused(&k, 1);
    host_rules(&k.coop, &k.game);
    acks(&k);
    k.coop.slots[1].heard = 0;
    host_rules(&k.coop, &k.game);
    CHECK(!k.coop.server_paused && k.coop.confirmed != k.coop.sequence);
    // not the host any more, or out of the map: the rules' answers are all off, the rest is kept
    k.coop.slots[1].heard = clock;
    host_rules(&k.coop, &k.game);
    CHECK(k.coop.server_paused && k.coop.host_view_frozen);
    snprintf(k.map, sizeof(k.map), "core_frontend");
    host_rules(&k.coop, &k.game);
    CHECK(!k.coop.server_paused && !k.coop.host_view_frozen && !k.coop.waiting && !k.coop.all_confirmed);
    CHECK(k.coop.state == 1 && k.coop.mask == 0xe);
    k.coop.host_timed_out = true;
    k.coop.role = 0;
    snprintf(k.map, sizeof(k.map), "zm_theater");
    host_rules(&k.coop, &k.game);
    CHECK(!k.coop.host_timed_out);
    CHECK(h.lock_errors == 0 && g.lock_errors == 0 && k.lock_errors == 0);
}

// ---- the network side

static void test_receive(void) {
    uint64_t clock = 300000;
    machine_t h;
    host_setup(&h, &clock);
    aae_coop_t *c = &h.coop;
    uint8_t packet[AAE_Coop_Packet_Size];
    aae_coop_packet(packet, Kind_Hello, 0, 0x501, 0, 0);
    const aae_coop_adr_t *from = &h.clients[1].adr;
    // what makes a packet unusable
    uint8_t bad[AAE_Coop_Packet_Size + 1];
    struct { int offset; uint32_t value; } breaks[] = { { Packet_Version, 2 }, { Packet_Version, 0 },
        { Packet_Tail, 1 }, { Packet_D, 2 }, { Packet_Kind, 0 }, { Packet_Kind, 4 }, { Packet_B, 0 },
        { Packet_B + 4, 0 } };
    for (size_t i = 0; i < sizeof(breaks) / sizeof(breaks[0]); i++) {
        memcpy(bad, packet, sizeof(packet));
        if (breaks[i].offset == Packet_B)
            memset(bad + Packet_B, 0, 8);
        else
            put32(bad + breaks[i].offset, breaks[i].value);
        aae_coop_receive(c, &h.game, from, Host_Sock, bad, sizeof(packet));
        CHECK(c->slots[1].nonce == (breaks[i].offset == Packet_B + 4 ? 0x501 : 0));
        CHECK(c->slots[1].heard == (breaks[i].offset == Packet_B + 4 ? clock : 0));
        memset(&c->slots[1].nonce, 0, sizeof(c->slots[1].nonce));
        c->slots[1].heard = 0;
    }
    memcpy(bad, packet, sizeof(packet));
    aae_coop_receive(c, &h.game, from, Host_Sock, bad, sizeof(packet) - 1);
    aae_coop_receive(c, &h.game, from, Host_Sock, bad, sizeof(packet) + 1);
    CHECK(c->slots[1].nonce == 0);
    // a hello without a nonce, to a slot that has one: nothing
    from_slot(&h, 2, Kind_Hello, 0, 0x502, 0, 0);
    c->slots[2].acked = 3;
    clock += 1;
    from_slot(&h, 2, Kind_Hello, 0, 0, 0, 0);
    CHECK(c->slots[2].nonce == 0x502 && c->slots[2].acked == 3 && c->slots[2].heard == clock - 1);
    // a hello: from a slot's address, with A, C and D 0
    uint64_t a_c_d[][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 1ull << 32, 0 } };
    for (size_t i = 0; i < sizeof(a_c_d) / sizeof(a_c_d[0]); i++) {
        from_slot(&h, 1, Kind_Hello, a_c_d[i][0], 0x501, a_c_d[i][1], (uint32_t)a_c_d[i][2]);
        CHECK(c->slots[1].nonce == 0 && c->slots[1].heard == 0);
    }
    // the socket it came for has to be the host's
    aae_coop_receive(c, &h.game, from, 0, packet, sizeof(packet));
    aae_coop_receive(c, &h.game, from, 3, packet, sizeof(packet));
    CHECK(c->slots[1].nonce == 0);
    // from an address of no slot, or one that differs in what the plugin compares
    aae_coop_adr_t other = *from;
    other.bytes[4] ^= 1;
    aae_coop_receive(c, &h.game, &other, Host_Sock, packet, sizeof(packet));
    other = *from;
    other.bytes[12] = 1;
    aae_coop_receive(c, &h.game, &other, Host_Sock, packet, sizeof(packet));
    other = *from;
    other.bytes[8] = 3;
    aae_coop_receive(c, &h.game, &other, Host_Sock, packet, sizeof(packet));
    other = *from;
    other.bytes[1] = 1;
    aae_coop_receive(c, &h.game, &other, Host_Sock, packet, sizeof(packet));
    CHECK(c->slots[1].nonce == 0);
    other = *from;
    other.bytes[7] = 0x55; // the two unused bytes aren't compared
    aae_coop_receive(c, &h.game, &other, Host_Sock, packet, sizeof(packet));
    CHECK(c->slots[1].nonce == 0x501 && c->slots[1].heard == clock);
    // only while local client 0's frame ran within 5 s
    c->slots[2].nonce = 0, c->slots[3].nonce = 0;
    clock = c->frame_at + 5000;
    from_slot(&h, 2, Kind_Hello, 0, 0x502, 0, 0);
    CHECK(c->slots[2].nonce == 0x502);
    clock = c->frame_at + 5001;
    from_slot(&h, 3, Kind_Hello, 0, 0x503, 0, 0);
    CHECK(c->slots[3].nonce == 0);
    game_frame(&h, NULL);
    from_slot(&h, 3, Kind_Hello, 0, 0x503, 0, 0);
    CHECK(c->slots[3].nonce == 0x503);
    // a new nonce in a slot: its acknowledgement and sending start anew
    c->slots[3].acked = 5, c->slots[3].sent = 7, c->slots[3].sends = 9;
    from_slot(&h, 3, Kind_Hello, 0, 0x503, 0, 0);
    CHECK(c->slots[3].acked == 5 && c->slots[3].sent == 7 && c->slots[3].sends == 9);
    from_slot(&h, 3, Kind_Hello, 0, 0x603, 0, 0);
    CHECK(c->slots[3].nonce == 0x603 && c->slots[3].acked == 0 && c->slots[3].sent == 0 && c->slots[3].sends == 0);
    // a slot not taking part hears nothing
    c->slots[2].ok = false;
    clock += 1;
    from_slot(&h, 2, Kind_Hello, 0, 0x702, 0, 0);
    CHECK(c->slots[2].nonce == 0x502 && c->slots[2].heard != clock);
    c->slots[2].ok = true;
    // an acknowledgement: the slot's nonce, the host's nonce, sequence and state as they are now
    uint64_t sequence = c->sequence;
    uint32_t state = c->state;
    clock += 1;
    from_slot(&h, 1, Kind_Ack, c->host_nonce, 0x502, sequence, state);
    from_slot(&h, 1, Kind_Ack, c->host_nonce + 1, 0x501, sequence, state);
    from_slot(&h, 1, Kind_Ack, c->host_nonce, 0x501, sequence - 1, state);
    from_slot(&h, 1, Kind_Ack, c->host_nonce, 0x501, sequence + 1, state);
    from_slot(&h, 1, Kind_Ack, c->host_nonce, 0x501, sequence, state ^ 1);
    CHECK(c->slots[1].acked == 0 && c->slots[1].heard != clock);
    from_slot(&h, 1, Kind_Ack, c->host_nonce, 0x501, sequence, state);
    CHECK(c->slots[1].acked == sequence && c->slots[1].heard == clock);
    // a state packet means nothing to a host
    from_slot(&h, 1, Kind_State, c->host_nonce, 0x501, sequence + 5, 1);
    CHECK(c->slots[1].acked == sequence && c->sequence == sequence);
    // neither host nor client: nothing
    h.coop.role = -1;
    h.coop.sock = 4;
    from_slot(&h, 1, Kind_Hello, 0, 0x801, 0, 0);
    CHECK(c->slots[1].nonce == 0x501);
    CHECK(h.lock_errors == 0 && h.lock_depth == 0);
}

// a client of the host at 10.0.0.1:100, its own socket 0
static void client_setup(machine_t *m, uint64_t *clock, uint64_t nonces) {
    machine_init(m, clock, 0, nonces);
    m->connection = 9;
    m->connection_data = true;
    m->server = adr(1, 100, 4, 4);
    m->server_id = 0xabc;
    game_frame(m, NULL);
}
static void to_client(machine_t *m, uint64_t host_nonce, uint64_t nonce, uint64_t sequence, uint32_t state) {
    uint8_t packet[AAE_Coop_Packet_Size];
    aae_coop_packet(packet, Kind_State, host_nonce, nonce, sequence, state);
    aae_coop_receive(&m->coop, &m->game, &m->server, m->local_sock, packet, sizeof(packet));
}

static void test_client(void) {
    uint64_t clock = 400000;
    machine_t m;
    client_setup(&m, &clock, 0x2000);
    aae_coop_t *c = &m.coop;
    // a client: its nonce, drawn when it became one, and drawn again for the server it found
    CHECK(c->role == 0 && c->sock == 0 && c->server_valid && c->own_nonce == 0x2001 && c->server_id == 0xabc);
    CHECK(m.coop_paused == 0 && m.coop_paused_sets == 1 && cl_paused_of(&m) == 0);
    uint64_t own = c->own_nonce;
    // number 0 isn't a state, even "running" to a client that knows none yet
    to_client(&m, 0x77, own, 0, 0);
    CHECK(c->known_nonce == 0 && c->heard == 0 && c->news == 0);
    // the host's state: taken, then put into effect at the next frame, then acknowledged
    clock += 5;
    to_client(&m, 0x77, own, 3, 1);
    CHECK(c->known_nonce == 0x77 && c->known_sequence == 3 && c->known_state == 1 && c->news == 1 &&
          c->heard == clock && c->acked_news == 0);
    clock += 11;
    CHECK(game_frame(&m, NULL) == 0);
    CHECK(cl_paused_of(&m) == 1 && m.cl_paused_sets == 1 && c->applied && m.coop_paused == 1 && c->paused_by_host);
    CHECK(c->acked_news == 1 && c->ack_at == 0 && c->remote_state == 1 && c->remote_known &&
          c->remote_at == clock - 11);
    float frac = 0.25f;
    CHECK(run_client_frame(&m, 0, 16, &frac) == 0 && frac == 0.0f);
    CHECK(m.cl_paused_sets == 1 && m.coop_paused_sets == 2);
    // the same state again: heard, acknowledged again at once
    c->ack_at = 5;
    clock += 100;
    to_client(&m, 0x77, own, 3, 1);
    CHECK(c->heard == clock && c->ack_at == 0 && c->news == 1);
    // another state under the same number, an older number, no nonce, no number, another client's nonce: not taken
    c->ack_at = 5;
    clock += 100;
    to_client(&m, 0x77, own, 3, 0);
    to_client(&m, 0x77, own, 2, 1);
    to_client(&m, 0, own, 4, 1);
    to_client(&m, 0x77, own, 0, 1);
    to_client(&m, 0x77, own + 1, 4, 1);
    CHECK(c->known_sequence == 3 && c->known_state == 1 && c->heard == clock - 100 && c->ack_at == 5);
    // a state that is neither 0 nor 1: not taken
    to_client(&m, 0x77, own, 4, 2);
    CHECK(c->known_sequence == 3 && c->news == 1);
    // not from the server, or for another socket: not taken
    uint8_t packet[AAE_Coop_Packet_Size];
    aae_coop_packet(packet, Kind_State, 0x77, own, 4, 0);
    aae_coop_adr_t elsewhere = m.server;
    elsewhere.bytes[3] = 9;
    aae_coop_receive(c, &m.game, &elsewhere, 0, packet, sizeof(packet));
    aae_coop_receive(c, &m.game, &m.server, 1, packet, sizeof(packet));
    aae_coop_receive(c, &m.game, &m.server, 4, packet, sizeof(packet));
    aae_coop_packet(packet, Kind_Hello, 0, own, 0, 0);
    aae_coop_receive(c, &m.game, &m.server, 0, packet, sizeof(packet));
    aae_coop_packet(packet, Kind_Ack, 0x77, own, 4, 0);
    aae_coop_receive(c, &m.game, &m.server, 0, packet, sizeof(packet));
    CHECK(c->known_sequence == 3);
    // the host lets go: a newer state, cl_paused 0 at the next frame
    clock += 100;
    to_client(&m, 0x77, own, 9, 0);
    CHECK(c->news == 2 && c->known_sequence == 9 && c->known_state == 0 && c->acked_news == 0);
    clock += 16;
    CHECK(game_frame(&m, NULL) == 16);
    CHECK(cl_paused_of(&m) == 0 && m.cl_paused_sets == 2 && !c->applied && m.coop_paused == 0 &&
          c->acked_news == 2 && !c->paused_by_host);
    // another host (its nonce): the client starts anew with its own nonce, as if never heard
    clock += 100;
    c->hello_at = clock;
    to_client(&m, 0x99, own, 10, 1);
    CHECK(c->own_nonce == 0x2002 && c->known_sequence == 0 && c->known_nonce == 0 && c->hello_at == 0);
    CHECK(c->news == 2 && c->known_state == 0);
    to_client(&m, 0x99, own, 10, 1);
    CHECK(c->known_nonce == 0);
    own = c->own_nonce;
    to_client(&m, 0x99, own, 10, 1);
    CHECK(c->known_nonce == 0x99 && c->known_sequence == 10 && c->news == 3);
    clock += 16;
    game_frame(&m, NULL);
    CHECK(cl_paused_of(&m) == 1 && m.coop_paused == 1);
    // paused and not heard from for 10 s: the client unpauses itself
    c->hello_at = 5, c->ack_at = 6;
    uint64_t heard = c->heard;
    clock = heard + 9999;
    game_frame(&m, NULL);
    CHECK(cl_paused_of(&m) == 1 && c->own_nonce == own);
    clock = heard + 10000;
    CHECK(game_frame(&m, NULL) == 16);
    CHECK(cl_paused_of(&m) == 0 && c->own_nonce == 0x2003 && c->news == 0 && c->heard == 0 && !c->applied &&
          m.coop_paused == 0 && !c->remote_known);
    CHECK(c->known_state == 0 && c->known_sequence == 0 && c->known_nonce == 0 && c->hello_at == 0 && c->ack_at == 0 &&
          c->acked_news == 0 && c->remote_state == 0 && c->remote_at == 0);
    // ... but not while the known state is "running"
    own = c->own_nonce;
    to_client(&m, 0x99, own, 11, 0);
    clock += 16;
    game_frame(&m, NULL);
    clock += 20000;
    game_frame(&m, NULL);
    CHECK(c->own_nonce == own && c->news == 1);
    CHECK(m.lock_errors == 0 && m.lock_depth == 0);
}

static void test_client_server_info(void) {
    uint64_t clock = 500000;
    machine_t m;
    client_setup(&m, &clock, 0x3000);
    aae_coop_t *c = &m.coop;
    CHECK(c->server_valid && c->own_nonce == 0x3001);
    to_client(&m, 0x77, c->own_nonce, 3, 1);
    clock += 16;
    game_frame(&m, NULL);
    CHECK(cl_paused_of(&m) == 1);
    // the server's info: when it changes, everything from the old one is forgotten and cl_paused goes back to 0
    struct { int connection; bool data; aae_coop_adr_t server; uint64_t id; bool valid, changed; } cases[] = {
        { 3, true, adr(1, 100, 4, 4), 0xabc, false, true },   // not connected far enough: nothing read
        { 9, true, adr(1, 100, 4, 4), 0xabc, true, false },   // as it was
        { 9, false, adr(1, 100, 4, 4), 0xabc, false, true },  // no connection data
        { 9, true, adr(1, 100, 4, 4), 0, false, true },       // no id
        { 9, true, adr(1, 100, 2, 4), 0xabc, false, true },   // not another machine
        { 9, true, adr(1, 100, 3, 4), 0xabc, true, true },    // a plain IP is another machine too
        { 9, true, adr(1, 100, 4, 5), 0xabc, false, true },   // net id above 4
        { 4, true, adr(1, 100, 4, 4), 0xabc, true, false },   // connected far enough already
        { -1, true, adr(1, 100, 4, 4), 0xabc, false, true },  // the state outside 0..11
        { 9, true, adr(1, 101, 4, 4), 0xabc, true, true },    // another server
        { 9, true, adr(1, 100, 4, 4), 0xabd, true, true },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint64_t own = c->own_nonce;
        int sets = m.cl_paused_sets;
        to_client(&m, 0x77, own, 4, 1);
        clock += 16;
        game_frame(&m, NULL);
        bool was_paused = cl_paused_of(&m) == 1;
        m.connection = cases[i].connection;
        m.connection_data = cases[i].data;
        m.server = cases[i].server;
        m.server_id = cases[i].id;
        clock += 16;
        game_frame(&m, NULL);
        CHECK(c->server_valid == cases[i].valid);
        if (cases[i].changed) {
            CHECK(c->own_nonce != own && c->news == 0 && c->known_nonce == 0);
            CHECK(cl_paused_of(&m) == 0 && (!was_paused || m.cl_paused_sets > sets) && m.coop_paused == 0);
        } else {
            CHECK(c->own_nonce == own && c->news != 0 && cl_paused_of(&m) == 1 && m.coop_paused == 1);
        }
        // what was read is kept, also when not valid, so that it is noticed when it changes
        if (cases[i].connection >= 4 && cases[i].data)
            CHECK(memcmp(&c->server, &cases[i].server, sizeof(c->server)) == 0 && c->server_id == cases[i].id);
        else
            CHECK(c->server_id == 0 && adr_kind(&c->server) == 0);
        // the same info again: nothing is forgotten
        own = c->own_nonce;
        clock += 16;
        game_frame(&m, NULL);
        CHECK(c->own_nonce == own);
        if (!cases[i].valid) {
            to_client(&m, 0x77, own, 5, 1);
            CHECK(c->news == 0); // nothing from a server that isn't valid
        }
        m.connection = 9, m.connection_data = true, m.server = adr(1, 100, 4, 4), m.server_id = 0xabc;
        clock += 16;
        game_frame(&m, NULL);
    }
    // the two unused bytes of the server's address: not noticed
    uint64_t own = c->own_nonce;
    m.server.bytes[6] = 9;
    clock += 16;
    game_frame(&m, NULL);
    CHECK(c->own_nonce == own);
    // the client's socket is the game's for local client 0
    m.local_sock = 1;
    clock += 16;
    game_frame(&m, NULL);
    CHECK(c->sock == 1);
    CHECK(m.lock_errors == 0 && m.lock_depth == 0);
}

static void test_forget(void) {
    uint64_t clock = 1300000;
    machine_t h;
    host_setup(&h, &clock);
    aae_coop_t *c = &h.coop;
    hellos(&h);
    set_cl_paused(&h, 1);
    clock += 16;
    game_frame(&h, NULL);
    acks(&h);
    clock += 16;
    game_frame(&h, NULL);
    CHECK(c->sequence > 1 && c->confirmed == c->sequence && c->mask == 0xe && c->state == 1 && c->slots[1].nonce != 0 &&
          c->changed_at != 0);
    c->server = adr(9, 900, 4, 4), c->server_id = 5, c->heard = 7, c->server_valid = true, c->known_sequence = 3;
    c->known_nonce = 4, c->known_state = 1, c->ack_at = 8, c->hello_at = 9, c->acked_news = 2, c->news = 2;
    c->timed_out = true, c->own_nonce = 11;
    snprintf(h.map, sizeof(h.map), "core_frontend");
    clock += 16;
    game_frame(&h, NULL);
    aae_coop_slot_t none = { 0 };
    for (int i = 0; i < AAE_Coop_Slots; i++)
        CHECK(memcmp(&c->slots[i], &none, sizeof(none)) == 0);
    aae_coop_adr_t nowhere = { { 0 } };
    CHECK(c->role == -1 && c->sock == -1 && c->host_nonce == 0 && c->sequence == 1 && c->confirmed == 0 &&
          c->mask == 0 && c->state == 0 && c->changed_at == 0 && !c->timed_out);
    CHECK(memcmp(&c->server, &nowhere, sizeof(nowhere)) == 0 && c->server_id == 0 && c->heard == 0 &&
          !c->server_valid && c->own_nonce == 0 && c->known_sequence == 0 && c->known_nonce == 0 &&
          c->known_state == 0 && c->ack_at == 0 && c->hello_at == 0 && c->acked_news == 0 && c->news == 0);
    CHECK(c->frame_at == clock); // not the network's: kept
    // a client becoming the host: the same
    machine_t m;
    client_setup(&m, &clock, 0xc000);
    to_client(&m, 0x77, m.coop.own_nonce, 3, 0);
    clock += 16;
    game_frame(&m, NULL);
    CHECK(m.coop.news == 1 && m.coop.server_valid);
    m.value[AAE_Coop_Sv_Running] = whole(1);
    clock += 16;
    game_frame(&m, NULL);
    CHECK(m.coop.role == 1 && m.coop.news == 0 && !m.coop.server_valid && m.coop.known_nonce == 0 &&
          m.coop.own_nonce == 0 && m.coop.host_nonce == 0xc002 && m.coop.server_id == 0);
}

static void test_roles(void) {
    uint64_t clock = 600000;
    machine_t m;
    client_setup(&m, &clock, 0x4000);
    aae_coop_t *c = &m.coop;
    to_client(&m, 0x77, c->own_nonce, 3, 1);
    clock += 16;
    game_frame(&m, NULL);
    CHECK(cl_paused_of(&m) == 1 && c->applied);
    // out of the map: neither host nor client, everything of the network forgotten, cl_paused back to 0
    snprintf(m.map, sizeof(m.map), "core_frontend");
    clock += 16;
    game_frame(&m, NULL);
    CHECK(c->role == -1 && c->sock == -1 && cl_paused_of(&m) == 0 && !c->applied && c->own_nonce == 0 &&
          c->sequence == 1 && !c->server_valid && m.coop_paused == 0);
    int sets = m.cl_paused_sets;
    clock += 16;
    game_frame(&m, NULL);
    CHECK(m.cl_paused_sets == sets); // only once
    // the host: a nonce of its own, socket 4
    snprintf(m.map, sizeof(m.map), "zm_theater");
    m.value[AAE_Coop_Sv_Running] = whole(1);
    clock += 16;
    game_frame(&m, NULL);
    CHECK(c->role == 1 && c->sock == 4 && c->host_nonce == 0x4002 && c->own_nonce == 0);
    CHECK(!c->client_in_map && c->client_frame_at == 0); // the host isn't a client in a map
    CHECK(m.cl_paused_sets == sets); // not paused by anybody: nothing to put back
    // sv_running unreadable in a map: neither
    m.value[AAE_Coop_Sv_Running] = (aae_coop_value_t){ .kind = AAE_Coop_None };
    clock += 16;
    game_frame(&m, NULL);
    CHECK(c->role == -1 && c->host_nonce == 0);
    // only local client 0 keeps the network's books
    m.value[AAE_Coop_Sv_Running] = whole(0);
    clock += 16;
    float frac = 1.0f;
    run_client_frame(&m, 1, 16, &frac);
    CHECK(c->role == -1 && c->frame_at != clock);
    run_client_frame(&m, 0, 16, &frac);
    CHECK(c->role == 0 && c->frame_at == clock);
    CHECK(m.lock_errors == 0 && m.lock_depth == 0);
}

static int sent(machine_t *m, aae_coop_outgoing_t out[AAE_Coop_Slots + 2]) {
    return aae_coop_send(&m->coop, &m->game, out);
}

// The plugin's times are milliseconds since the machine started, so a 0 means "never": a test of what it does with
// the clock near 0, where "never" and "long ago" differ.
static void test_early_clock(void) {
    uint64_t clock = 10;
    machine_t h;
    machine_init(&h, &clock, 1, 0x1000);
    seat(&h, 0, 5, adr(1, 100, 2, 0), 11, 0);
    seat(&h, 1, 5, adr(2, 200, 4, 0), 12, 0);
    aae_coop_outgoing_t out[AAE_Coop_Slots + 2];
    // no frame yet (the host's rules ran, so its slots are there): nothing taken in, nothing sent
    h.coop.role = 1, h.coop.sock = 4, h.coop.host_nonce = 0x1000;
    host_rules(&h.coop, &h.game);
    CHECK(h.coop.mask == 0x2 && h.coop.frame_at == 0);
    from_slot(&h, 1, Kind_Hello, 0, 0x501, 0, 0);
    CHECK(h.coop.slots[1].nonce == 0);
    h.coop.slots[1].nonce = 0x501;
    CHECK(aae_coop_send(&h.coop, &h.game, out) == 0);
    h.coop.role = -1, h.coop.sock = -1;
    game_frame(&h, NULL);
    from_slot(&h, 1, Kind_Hello, 0, 0x501, 0, 0);
    CHECK(h.coop.slots[1].nonce == 0x501);
    // never sent: at once
    CHECK(aae_coop_send(&h.coop, &h.game, out) == 1);
    machine_t m;
    clock = 20;
    machine_init(&m, &clock, 0, 0x2000);
    m.connection_data = true, m.server = adr(1, 100, 4, 4), m.server_id = 0xabc;
    game_frame(&m, NULL);
    CHECK(aae_coop_send(&m.coop, &m.game, out) == 1 && out[0].data[Packet_Kind] == Kind_Hello);
    to_client(&m, 0x77, m.coop.own_nonce, 3, 1);
    game_frame(&m, NULL);
    CHECK(aae_coop_send(&m.coop, &m.game, out) == 1 && out[0].data[Packet_Kind] == Kind_Ack);
    CHECK(h.lock_errors == 0 && m.lock_errors == 0);
}

static void test_sending(void) {
    uint64_t clock = 700000;
    machine_t h;
    host_setup(&h, &clock);
    aae_coop_t *c = &h.coop;
    aae_coop_outgoing_t out[AAE_Coop_Slots + 2];
    // nothing to anybody who hasn't said hello
    CHECK(sent(&h, out) == 0);
    from_slot(&h, 2, Kind_Hello, 0, 0x502, 0, 0);
    CHECK(sent(&h, out) == 1 && out[0].sock == 4 && memcmp(&out[0].to, &h.clients[2].adr, 16) == 0);
    uint8_t expect[AAE_Coop_Packet_Size];
    aae_coop_packet(expect, Kind_State, 0x1000, 0x502, c->sequence, 0);
    CHECK(memcmp(out[0].data, expect, sizeof(expect)) == 0 && c->slots[2].sent == clock && c->slots[2].sends == 1);
    // again every 70 ms until acknowledged, then every 800 ms
    clock += 69;
    CHECK(sent(&h, out) == 0);
    clock += 1;
    CHECK(sent(&h, out) == 1 && c->slots[2].sends == 2);
    from_slot(&h, 2, Kind_Ack, 0x1000, 0x502, c->sequence, 0);
    clock += 70;
    CHECK(sent(&h, out) == 0);
    clock += 729;
    CHECK(sent(&h, out) == 0);
    clock += 1;
    CHECK(sent(&h, out) == 1);
    // the slots in order; a change goes out at once to everybody
    from_slot(&h, 1, Kind_Hello, 0, 0x501, 0, 0);
    from_slot(&h, 3, Kind_Hello, 0, 0x503, 0, 0);
    set_cl_paused(&h, 1);
    clock += 1;
    game_frame(&h, NULL);
    CHECK(sent(&h, out) == 3 && memcmp(&out[0].to, &h.clients[1].adr, 16) == 0 &&
          memcmp(&out[2].to, &h.clients[3].adr, 16) == 0);
    aae_coop_packet(expect, Kind_State, 0x1000, 0x503, c->sequence, 1);
    CHECK(memcmp(out[2].data, expect, sizeof(expect)) == 0);
    // the give-up sends "running" at once
    clock += 10;
    CHECK(sent(&h, out) == 0);
    c->changed_at = clock - 3500;
    host_rules(c, &h.game);
    CHECK(c->timed_out && c->state == 0 && sent(&h, out) == 3 && get32(out[0].data + Packet_D) == 0);
    set_cl_paused(&h, 0);
    clock += 1;
    game_frame(&h, NULL);
    set_cl_paused(&h, 1);
    game_frame(&h, NULL);
    sent(&h, out);
    // somebody comes: the new sequence goes out at once to the others too
    clock += 1;
    CHECK(sent(&h, out) == 0);
    seat(&h, 3, 0, adr(0, 0, 0, 0), 0, 0);
    game_frame(&h, NULL);
    CHECK(sent(&h, out) == 2 && get64(out[0].data + Packet_C) == c->sequence);
    seat(&h, 3, 5, adr(4, 400, 4, 0), 14, 0);
    game_frame(&h, NULL);
    from_slot(&h, 3, Kind_Hello, 0, 0x503, 0, 0);
    CHECK(sent(&h, out) == 3);
    // nothing without the host's nonce, with a socket above 4, or when the frame hasn't run for 5 s
    clock += 70;
    c->host_nonce = 0;
    CHECK(sent(&h, out) == 0);
    c->host_nonce = 0x1000;
    c->sock = 5;
    CHECK(sent(&h, out) == 0);
    c->sock = -1;
    CHECK(sent(&h, out) == 0);
    c->sock = 4;
    clock = c->frame_at + 5001;
    CHECK(sent(&h, out) == 0);
    clock = c->frame_at + 5000;
    CHECK(sent(&h, out) == 3);
    c->frame_at = 0;
    clock += 1000;
    CHECK(sent(&h, out) == 0);
    c->frame_at = clock;
    // a slot not taking part: nothing
    c->slots[2].ok = false;
    clock += 1000;
    CHECK(sent(&h, out) == 2);
    c->slots[2].ok = true;

    // the client: a hello at once and every 800 ms while it has a valid server
    machine_t m;
    client_setup(&m, &clock, 0x5000);
    aae_coop_t *k = &m.coop;
    CHECK(sent(&m, out) == 1 && out[0].sock == 0 && memcmp(&out[0].to, &m.server, 16) == 0);
    aae_coop_packet(expect, Kind_Hello, 0, k->own_nonce, 0, 0);
    CHECK(memcmp(out[0].data, expect, sizeof(expect)) == 0 && k->hello_at == clock);
    clock += 799;
    CHECK(sent(&m, out) == 0);
    clock += 1;
    CHECK(sent(&m, out) == 1);
    // an acknowledgement only once the state was put into effect
    to_client(&m, 0x77, k->own_nonce, 3, 1);
    clock += 1;
    CHECK(sent(&m, out) == 0);
    game_frame(&m, NULL);
    CHECK(sent(&m, out) == 1);
    aae_coop_packet(expect, Kind_Ack, 0x77, k->own_nonce, 3, 1);
    CHECK(memcmp(out[0].data, expect, sizeof(expect)) == 0 && k->ack_at == clock);
    clock += 799;
    CHECK(sent(&m, out) == 1 && out[0].data[Packet_Kind] == Kind_Hello);
    clock += 1;
    CHECK(sent(&m, out) == 1 && out[0].data[Packet_Kind] == Kind_Ack);
    // the state again: acknowledged at once
    clock += 10;
    to_client(&m, 0x77, k->own_nonce, 3, 1);
    CHECK(sent(&m, out) == 1 && out[0].data[Packet_Kind] == Kind_Ack);
    // hello and acknowledgement in one go
    clock += 800;
    CHECK(sent(&m, out) == 2 && out[0].data[Packet_Kind] == Kind_Hello && out[1].data[Packet_Kind] == Kind_Ack);
    // a newer state not yet in effect: no acknowledgement of anything
    to_client(&m, 0x77, k->own_nonce, 4, 0);
    clock += 900;
    CHECK(sent(&m, out) == 1 && out[0].data[Packet_Kind] == Kind_Hello);
    // nothing without a valid server or a nonce
    k->server_valid = false;
    clock += 900;
    CHECK(sent(&m, out) == 0);
    k->server_valid = true;
    k->own_nonce = 0;
    CHECK(sent(&m, out) == 0);
    CHECK(h.lock_errors == 0 && m.lock_errors == 0 && h.lock_depth == 0 && m.lock_depth == 0);
}

// ---- the server's pause test, and what the frames run with

static void test_gate(void) {
    uint64_t clock = 800000;
    machine_t h;
    host_setup(&h, &clock);
    aae_coop_t *c = &h.coop;
    // out of a map: the game's own test
    snprintf(h.map, sizeof(h.map), "core_frontend");
    c->server_paused = true;
    CHECK(aae_coop_check_paused(c, &h.game) && c->server_paused);
    snprintf(h.map, sizeof(h.map), "zm_theater");
    // the host that may pause, cl_paused 0: the game's own test, and the server isn't held
    CHECK(aae_coop_check_paused(c, &h.game) && !c->server_paused && c->last_cl_paused == 0 && !c->busy);
    // cl_paused 1: the game's test, which would put it back to 0 with more than one client, isn't asked
    set_cl_paused(&h, 1);
    CHECK(!aae_coop_check_paused(c, &h.game) && !c->server_paused && c->last_cl_paused == 1 && !c->busy);
    // the server is held there once the host's rules said so: in time, everybody confirmed, not timed out
    c->waiting = c->all_confirmed = true;
    c->host_timed_out = false;
    c->paused_at = clock;
    CHECK(!aae_coop_check_paused(c, &h.game) && c->server_paused);
    struct { bool waiting, confirmed, timed_out; uint64_t at; int running, sv_paused; } nots[] = {
        { false, true, false, 1, 1, 0 }, { true, false, false, 1, 1, 0 }, { true, true, true, 1, 1, 0 },
        { true, true, false, 0, 1, 0 }, { true, true, false, 2, 1, 0 }, { true, true, false, 1, 1, -1 },
    };
    for (size_t i = 0; i < sizeof(nots) / sizeof(nots[0]); i++) {
        c->server_paused = false;
        c->waiting = nots[i].waiting, c->all_confirmed = nots[i].confirmed, c->host_timed_out = nots[i].timed_out;
        c->paused_at = nots[i].at == 0 ? 0 : clock + nots[i].at - 1; // 2: later than now
        h.value[AAE_Coop_Sv_Paused] = nots[i].sv_paused < 0 ? (aae_coop_value_t){ .kind = AAE_Coop_None }
                                                            : whole(nots[i].sv_paused);
        CHECK(!aae_coop_check_paused(c, &h.game) && !c->server_paused);
    }
    h.value[AAE_Coop_Sv_Paused] = whole(1);
    c->waiting = c->all_confirmed = true, c->host_timed_out = false, c->paused_at = clock;
    CHECK(!aae_coop_check_paused(c, &h.game) && c->server_paused);
    h.value[AAE_Coop_Sv_Paused] = (aae_coop_value_t){ .kind = AAE_Coop_Text, .text = "0" };
    c->server_paused = false;
    CHECK(!aae_coop_check_paused(c, &h.game) && !c->server_paused); // -2: below 0
    h.value[AAE_Coop_Sv_Paused] = whole(0);
    // while the host's rules run, the answer is 0 and nothing else happens
    c->busy = true;
    set_cl_paused(&h, 0);
    c->server_paused = true;
    CHECK(!aae_coop_check_paused(c, &h.game) && c->server_paused && c->last_cl_paused == 1);
    c->busy = false;
    // cl_paused turning 1 takes the counts anew: a client gone within 16 ms is seen
    CHECK(aae_coop_check_paused(c, &h.game) && !c->server_paused && c->last_cl_paused == 0);
    clock += 16;
    counts(c, &h.game);
    seat(&h, 3, 0, adr(0, 0, 0, 0), 0, 0);
    set_cl_paused(&h, 1);
    aae_coop_check_paused(c, &h.game);
    CHECK(c->remote == 2 && c->counted_at == clock);
    seat(&h, 2, 0, adr(0, 0, 0, 0), 0, 0);
    aae_coop_check_paused(c, &h.game);
    CHECK(c->remote == 2); // no change of cl_paused: kept for 16 ms
    // ... and when the last one of another machine went in those 16 ms, the plugin holds the server and resets
    seat(&h, 1, 0, adr(0, 0, 0, 0), 0, 0);
    set_cl_paused(&h, 0);
    aae_coop_check_paused(c, &h.game);
    clock += 16;
    seat(&h, 1, 5, adr(2, 200, 4, 0), 12, 0);
    counts(c, &h.game);
    seat(&h, 1, 0, adr(0, 0, 0, 0), 0, 0);
    set_cl_paused(&h, 1);
    c->host_view_frozen = c->waiting = c->all_confirmed = c->host_timed_out = true;
    c->paused_at = 5;
    c->server_paused = false;
    CHECK(!aae_coop_check_paused(c, &h.game) && c->server_paused && !c->host_view_frozen && !c->waiting &&
          !c->all_confirmed && !c->host_timed_out && c->paused_at == 0 && c->remote == 0 && c->total == 1);
    // only one client, of another machine: the game's own test, the server not held
    memset(h.clients, 0, sizeof(h.clients));
    seat(&h, 1, 5, adr(2, 200, 4, 0), 12, 0);
    clock += 16;
    c->server_paused = true;
    CHECK(aae_coop_check_paused(c, &h.game) && !c->server_paused && c->total == 1);
    // nobody at all within the 16 ms: the same
    clock += 16;
    counts(c, &h.game);
    memset(h.clients, 0, sizeof(h.clients));
    set_cl_paused(&h, 0);
    aae_coop_check_paused(c, &h.game);
    set_cl_paused(&h, 1);
    c->server_paused = true;
    CHECK(aae_coop_check_paused(c, &h.game) && !c->server_paused && c->total == 0);
    // not allowed by the mod: the game's own test
    seat(&h, 0, 5, adr(1, 100, 2, 0), 11, 0);
    seat(&h, 1, 5, adr(2, 200, 4, 0), 12, 0);
    clock += 16;
    set_pause_remote(&h, "0");
    CHECK(aae_coop_check_paused(c, &h.game));
    set_pause_remote(&h, "1");
    // players of this machine only: more than one and cl_paused 1 is "not paused" without the game's test
    memset(h.clients, 0, sizeof(h.clients));
    seat(&h, 0, 5, adr(1, 100, 2, 0), 11, 0);
    clock += 16;
    CHECK(aae_coop_check_paused(c, &h.game)); // alone, one player: the game's own pause
    seat(&h, 1, 5, adr(1, 101, 2, 1), 21, 0);
    clock += 16;
    CHECK(!aae_coop_check_paused(c, &h.game));
    set_cl_paused(&h, 0);
    CHECK(aae_coop_check_paused(c, &h.game));
    set_cl_paused(&h, 2);
    CHECK(aae_coop_check_paused(c, &h.game));
    // a client: "not paused" while the host paused it, else the game's own test
    machine_t m;
    client_setup(&m, &clock, 0x6000);
    CHECK(aae_coop_check_paused(&m.coop, &m.game));
    m.coop.remote_known = true, m.coop.remote_state = 1;
    CHECK(!aae_coop_check_paused(&m.coop, &m.game));
    m.coop.remote_state = 0;
    CHECK(aae_coop_check_paused(&m.coop, &m.game));
    CHECK(h.lock_errors == 0 && m.lock_errors == 0);
}

static void test_frames(void) {
    uint64_t clock = 900000;
    machine_t h;
    host_setup(&h, &clock);
    aae_coop_t *c = &h.coop;
    // the server's frame: held while the host's server is held and cl_paused is 1
    hellos(&h);
    set_cl_paused(&h, 1);
    clock += 16;
    game_frame(&h, NULL);
    acks(&h);
    CHECK(run_server_frame(&h, 33) == 0 && c->server_paused && c->sv_in_map);
    CHECK(run_server_frame(&h, -1) == 0);
    set_cl_paused(&h, 0);
    c->server_paused = true;
    host_rules(c, &h.game);
    c->server_paused = true;
    c->busy = true; // the rules don't run again here
    CHECK(run_server_frame(&h, 33) == 33 && !c->server_paused);
    uint64_t sequence = c->sequence;
    aae_coop_client_t gone = h.clients[3];
    h.clients[3].state = 0;
    run_server_frame(&h, 33);
    CHECK(c->sequence == sequence && c->mask == 0xe && c->busy);
    c->busy = false;
    run_server_frame(&h, 33);
    CHECK(c->sequence == sequence + 1 && c->mask == 0x6 && !c->busy);
    h.clients[3] = gone;
    run_server_frame(&h, 33);
    hellos(&h); // the one back in its place starts anew
    // not allowed: never held
    set_pause_remote(&h, "0");
    c->server_paused = true;
    c->busy = true;
    CHECK(run_server_frame(&h, 33) == 33 && !c->server_paused);
    c->busy = false;
    set_pause_remote(&h, "1");
    // out of a map, the server's frame records it
    snprintf(h.map, sizeof(h.map), "core_frontend");
    run_server_frame(&h, 33);
    CHECK(!c->sv_in_map);
    snprintf(h.map, sizeof(h.map), "zm_theater");
    // players of this machine only, cl_paused 1: both frames held, and the fraction is 0
    machine_t s;
    host_setup(&s, &clock);
    for (int i = 1; i <= 3; i++)
        s.clients[i].state = 0;
    seat(&s, 1, 5, adr(1, 101, 2, 1), 21, 0);
    clock += 16;
    float frac = 0.75f;
    CHECK(run_server_frame(&s, 16) == 16 && run_client_frame(&s, 0, 16, &frac) == 16 && frac == 0.75f);
    set_cl_paused(&s, 1);
    CHECK(aae_coop_can_pause(&s.coop, &s.game));
    CHECK(run_server_frame(&s, 16) == 0 && run_client_frame(&s, 0, 16, &frac) == 0 && frac == 0.0f);
    CHECK(!aae_coop_check_paused(&s.coop, &s.game)); // the game's refusal of two players is skipped
    // the host's own view is held as soon as it asks, before anybody acknowledged
    clock += 16;
    set_cl_paused(&h, 1);
    c->timed_out = false;
    frac = 0.5f;
    run_server_frame(&h, 16);
    CHECK(c->host_view_frozen && !c->server_paused && run_client_frame(&h, 0, 16, &frac) == 0 && frac == 0.0f);
    // its view held needs cl_paused 1 at the frame itself
    c->host_view_frozen = true;
    set_cl_paused(&h, 0);
    c->busy = true;
    frac = 0.5f;
    CHECK(run_client_frame(&h, 0, 16, &frac) == 16 && frac == 0.5f);
    set_cl_paused(&h, 1);
    CHECK(run_client_frame(&h, 0, 16, &frac) == 0);
    h.value[AAE_Coop_Sv_Running] = whole(0);
    c->host_view_frozen = true;
    frac = 0.5f;
    CHECK(run_client_frame(&h, 1, 16, &frac) == 16);
    h.value[AAE_Coop_Sv_Running] = whole(1);
    c->busy = false;
    // the host's server held: its client frame too; once cl_paused is 0, it isn't held any more
    clock += 16;
    acks(&h);
    run_server_frame(&h, 16);
    CHECK(c->server_paused);
    c->host_view_frozen = false;
    c->busy = true;
    CHECK(run_client_frame(&h, 1, 16, &frac) == 0);
    set_cl_paused(&h, 0);
    CHECK(run_client_frame(&h, 1, 16, &frac) == 16 && !c->server_paused);
    c->busy = false;

    // a client paused by its host: held, while still connected to it and hearing from it
    machine_t m;
    client_setup(&m, &clock, 0x7000);
    aae_coop_t *k = &m.coop;
    to_client(&m, 0x77, k->own_nonce, 3, 1);
    clock += 16;
    frac = 0.5f;
    CHECK(run_client_frame(&m, 0, 16, &frac) == 0 && frac == 0.0f && aae_coop_can_pause(k, &m.game));
    m.connection = 3; // disconnecting
    CHECK(run_client_frame(&m, 1, 16, &frac) == 16 && !k->remote_known && k->remote_state == 0 && k->remote_at == 0 &&
          m.coop_paused == 0);
    m.connection = 9;
    k->remote_known = true, k->remote_state = 1, k->remote_at = clock - 9999;
    CHECK(run_client_frame(&m, 1, 16, &frac) == 0);
    k->remote_at = clock - 10000;
    CHECK(run_client_frame(&m, 1, 16, &frac) == 16 && !k->remote_known);
    k->remote_known = true, k->remote_state = 1, k->remote_at = clock + 1; // a time ahead: kept
    CHECK(run_client_frame(&m, 1, 16, &frac) == 0);
    k->remote_at = 0; // from a server command "0": no time, kept
    CHECK(run_client_frame(&m, 1, 16, &frac) == 0);
    m.connection = -1; // the state can't be read: kept
    CHECK(run_client_frame(&m, 1, 16, &frac) == 0);
    m.connection = 4;
    CHECK(run_client_frame(&m, 1, 16, &frac) == 0);
    m.connection = 9;
    // out of the map, or the host: forgotten
    snprintf(m.map, sizeof(m.map), "core_frontend");
    CHECK(run_client_frame(&m, 1, 16, &frac) == 16 && !k->remote_known && k->remote_state == 0);
    snprintf(m.map, sizeof(m.map), "zm_theater");
    k->remote_known = true, k->remote_state = 1, k->remote_at = 0;
    m.value[AAE_Coop_Sv_Running] = whole(1);
    CHECK(run_client_frame(&m, 1, 16, &frac) == 16 && !k->remote_known && !k->client_in_map);
    m.value[AAE_Coop_Sv_Running] = whole(0);
    // aae_coop_paused is set when it changes, the first time to 0
    machine_t f;
    client_setup(&f, &clock, 0x8000);
    CHECK(f.coop_paused_sets == 1 && f.coop_paused == 0 && f.coop.coop_paused_written == 0);
    game_frame(&f, NULL);
    CHECK(f.coop_paused_sets == 1);
    // the acknowledgement waits for the frame that put the state into effect: not while cl_paused was other
    to_client(&f, 0x77, f.coop.own_nonce, 3, 1);
    aae_coop_frame_t frame;
    int msec = 16;
    frac = 0.5f;
    aae_coop_client_frame(&f.coop, &f.game, 0, &msec, &frac, &frame);
    CHECK(frame.news == 1 && frame.by_host && f.coop.acked_news == 0);
    f.coop.known_state = 0; // as if another state had come meanwhile
    aae_coop_client_frame_done(&f.coop, &f.game, &frame);
    CHECK(f.coop.acked_news == 0);
    f.coop.known_state = 1;
    f.coop.news = 2;
    aae_coop_client_frame_done(&f.coop, &f.game, &frame);
    CHECK(f.coop.acked_news == 0);
    f.coop.news = 1;
    f.coop.role = 1;
    aae_coop_client_frame_done(&f.coop, &f.game, &frame);
    CHECK(f.coop.acked_news == 0);
    f.coop.role = 0;
    f.coop.ack_at = 5;
    aae_coop_client_frame_done(&f.coop, &f.game, &frame);
    CHECK(f.coop.acked_news == 1 && f.coop.ack_at == 0);
    f.coop.ack_at = 5;
    aae_coop_client_frame_done(&f.coop, &f.game, &frame);
    CHECK(f.coop.ack_at == 5); // only once
    frame.news = 0;
    f.coop.acked_news = 0;
    aae_coop_client_frame_done(&f.coop, &f.game, &frame);
    CHECK(f.coop.acked_news == 0);
    CHECK(h.lock_errors == 0 && s.lock_errors == 0 && m.lock_errors == 0 && f.lock_errors == 0);
}

static void test_can_pause(void) {
    uint64_t clock = 1000000;
    machine_t h;
    host_setup(&h, &clock);
    CHECK(aae_coop_can_pause(&h.coop, &h.game));
    h.coop.host_timed_out = true;
    CHECK(!aae_coop_can_pause(&h.coop, &h.game));
    h.coop.host_timed_out = false;
    set_pause_remote(&h, "0");
    CHECK(!aae_coop_can_pause(&h.coop, &h.game));
    set_pause_remote(&h, "1");
    snprintf(h.map, sizeof(h.map), "core_frontend");
    CHECK(!aae_coop_can_pause(&h.coop, &h.game));
    snprintf(h.map, sizeof(h.map), "zm_theater");
    // alone: yes, timed out or not, allowed or not
    for (int i = 1; i <= 3; i++)
        h.clients[i].state = 0;
    clock += 16;
    set_pause_remote(&h, "0");
    h.coop.host_timed_out = true;
    CHECK(aae_coop_can_pause(&h.coop, &h.game));
    // sv_running unreadable and not alone: the game's own answer
    h.value[AAE_Coop_Sv_Running] = (aae_coop_value_t){ .kind = AAE_Coop_None };
    h.coop.sv_in_map = false;
    CHECK(!aae_coop_can_pause(&h.coop, &h.game));
    h.coop.remote_known = true, h.coop.remote_state = 1;
    CHECK(!aae_coop_can_pause(&h.coop, &h.game));
    h.value[AAE_Coop_Sv_Running] = whole(0);
    CHECK(aae_coop_can_pause(&h.coop, &h.game));
    h.coop.remote_state = 0;
    CHECK(!aae_coop_can_pause(&h.coop, &h.game));
    // out of a map, what came from the host is forgotten when it is asked
    h.coop.remote_state = 1;
    snprintf(h.map, sizeof(h.map), "core_frontend");
    CHECK(!aae_coop_can_pause(&h.coop, &h.game) && h.coop.remote_known);
    snprintf(h.map, sizeof(h.map), "zm_theater");
    h.coop.paused_by_host = true;
    snprintf(h.map, sizeof(h.map), "frontend!"); // not a map name the plugin takes: the game's own test, nothing else
    CHECK(aae_coop_check_paused(&h.coop, &h.game) && h.coop.remote_known);
    snprintf(h.map, sizeof(h.map), "zm_theater");
    CHECK(paused_by_host(&h.coop, &h.game));
    snprintf(h.map, sizeof(h.map), "-");
    CHECK(paused_by_host(&h.coop, &h.game)); // "-" is a map name the plugin takes
    snprintf(h.map, sizeof(h.map), "core_frontend");
    h.coop.remote_at = 77;
    CHECK(!paused_by_host(&h.coop, &h.game) && !h.coop.remote_known && h.coop.remote_state == 0 &&
          !h.coop.paused_by_host && h.coop.remote_at == 0);
}

// On a client the game's frame calls SV_Frame too: the host's part of the state stays untouched there.
static void test_client_server_frame(void) {
    uint64_t clock = 1150000;
    machine_t m;
    client_setup(&m, &clock, 0xb000);
    to_client(&m, 0x77, m.coop.own_nonce, 3, 1);
    int server;
    for (int i = 0; i < 5; i++) {
        clock += 16;
        CHECK(game_frame(&m, &server) == 0 && server == 16);
    }
    CHECK(cl_paused_of(&m) == 1 && m.coop.sequence == 1 && m.coop.state == 0 && m.coop.changed_at == 0 &&
          !m.coop.server_paused && !m.coop.host_view_frozen);
}

// ---- the server commands the client takes apart

static void test_commands(void) {
    uint64_t clock = 1100000;
    machine_t m;
    client_setup(&m, &clock, 0x9000);
    aae_coop_t *c = &m.coop;
    CHECK(c->client_in_map && c->client_frame_at == clock);
    const char *given = aae_coop_server_command(c, &m.game, "q @ 1");
    CHECK(strcmp(given, "q cl_paused \"1\"") == 0 && c->remote_known && c->remote_state == 1 && c->remote_at == clock);
    given = aae_coop_server_command(c, &m.game, "q @ 0 00af");
    CHECK(strcmp(given, "q cl_paused \"0\"") == 0 && c->remote_state == 0 && c->remote_at == 0 && c->remote_known);
    given = aae_coop_server_command(c, &m.game, "q @ 1 9fa0");
    CHECK(strcmp(given, "q cl_paused \"1\"") == 0 && c->remote_state == 1);
    // not one of them: the text as it came, nothing taken
    const char *others[] = { "q @ 0 00AF", "q @ 0 00a", "q @ 0 00af ", "q @ 0 00ag", "q @ 2", "q @ 0x", "q @ 0  00af",
                             "q @  0", "q @0", "q  @ 0", "Q @ 0", "q @ /", "q @ ", "q @", "q", "", "q cl_paused 2",
                             "q cl_paused \"\"", "q cl_paused 0 ", "q cl_paused \"0\"\"", "q cl_paused", "q cl_paused ",
                             "q cl_paused  0", "q cl_pausedx 0", "q cl_paused 01", "x cl_paused 0", "q @ 1 g000",
                             "q @ 1 0g00", "q @ 1 00g0", "q @ 1 000g", "q @ 1 00af0", "q @ 1 :000", "q @ 1 0`00" };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        c->remote_state = 1, c->remote_at = 77;
        given = aae_coop_server_command(c, &m.game, others[i]);
        CHECK(given == others[i] && c->remote_state == 1 && c->remote_at == 77);
    }
    // the game's own "set cl_paused": taken, the text stays; each quote may be there or not
    const char *sets[][2] = { { "q cl_paused 0", "0" }, { "q cl_paused \"0\"", "0" }, { "q cl_paused \"1", "1" },
                              { "q cl_paused 1\"", "1" }, { "q cl_paused \"1\"", "1" }, { "q cl_paused 1", "1" } };
    for (size_t i = 0; i < sizeof(sets) / sizeof(sets[0]); i++) {
        c->remote_state = 7;
        given = aae_coop_server_command(c, &m.game, sets[i][0]);
        CHECK(given == sets[i][0] && c->remote_state == sets[i][1][0] - '0');
    }
    CHECK(aae_coop_server_command(c, &m.game, NULL) == NULL);
    // out of a map: the text as it came, nothing taken
    snprintf(m.map, sizeof(m.map), "core_frontend");
    c->remote_state = 5;
    given = aae_coop_server_command(c, &m.game, "q @ 1");
    CHECK(strcmp(given, "q @ 1") == 0 && c->remote_state == 5);
    given = aae_coop_server_command(c, &m.game, "q cl_paused 1");
    CHECK(c->remote_state == 5);
    snprintf(m.map, sizeof(m.map), "zm_theater");
    // a pause counts within 2 s of the client's last frame in a map; a 0 whenever it is a client in a map
    c->remote_state = 5;
    clock = c->client_frame_at + 2000;
    aae_coop_server_command(c, &m.game, "q @ 1");
    CHECK(c->remote_state == 1 && c->remote_at == clock);
    c->remote_state = 5;
    clock = c->client_frame_at + 2001;
    given = aae_coop_server_command(c, &m.game, "q @ 1");
    CHECK(strcmp(given, "q cl_paused \"1\"") == 0 && c->remote_state == 5);
    aae_coop_server_command(c, &m.game, "q @ 0");
    CHECK(c->remote_state == 0);
    c->remote_state = 5;
    c->client_frame_at = clock + 1; // a time ahead
    aae_coop_server_command(c, &m.game, "q @ 1");
    CHECK(c->remote_state == 5);
    c->client_frame_at = 0;
    aae_coop_server_command(c, &m.game, "q @ 1");
    CHECK(c->remote_state == 5);
    // not a client in a map (the host): nothing, not even a 0
    c->client_in_map = false;
    c->client_frame_at = clock;
    aae_coop_server_command(c, &m.game, "q @ 1");
    aae_coop_server_command(c, &m.game, "q @ 0");
    CHECK(c->remote_state == 5);
}

// ---- four machines over a made-up network

typedef struct {
    machine_t m;
    uint8_t ip;
    uint16_t port;
    bool mute, deaf;      // what it sends is lost; what is sent to it is lost
    int server_msec, client_msec;
} node_t;

#define Nodes 5
static node_t nodes[Nodes];
static int node_count;
static uint64_t world_clock;
static struct {
    uint64_t at;
    int to;
    aae_coop_adr_t from;
    int msg_sock;
    uint8_t data[AAE_Coop_Packet_Size];
} flights[8192];
static int flying, lost_count, delivered_count;
static uint64_t random_state = 0x9e3779b97f4a7c15ull;
static uint32_t loss_percent, latency_least = 5, latency_spread = 150;

static uint64_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static int node_at(const aae_coop_adr_t *to) {
    for (int i = 0; i < node_count; i++)
        if (to->bytes[3] == nodes[i].ip && memcmp(to->bytes + 4, &nodes[i].port, 2) == 0)
            return i;
    return -1;
}

// what a machine's network thread sends goes out with its address and socket, and arrives (or not) later
static void network_send(int from) {
    aae_coop_outgoing_t out[AAE_Coop_Slots + 2];
    int count = aae_coop_send(&nodes[from].m.coop, &nodes[from].m.game, out);
    for (int i = 0; i < count; i++) {
        int to = node_at(&out[i].to);
        if (to < 0 || nodes[from].mute || nodes[to].deaf || next_random() % 100 < loss_percent ||
            flying == (int)(sizeof(flights) / sizeof(flights[0]))) {
            lost_count++;
            continue;
        }
        flights[flying].at = world_clock + latency_least + next_random() % latency_spread;
        flights[flying].to = to;
        flights[flying].from = adr(nodes[from].ip, nodes[from].port, 4, (uint32_t)out[i].sock);
        flights[flying].msg_sock = (int)adr_net_id(&out[i].to);
        memcpy(flights[flying].data, out[i].data, AAE_Coop_Packet_Size);
        flying++;
    }
}

static void network_deliver(void) {
    for (int i = 0; i < flying;) {
        if (flights[i].at > world_clock) {
            i++;
            continue;
        }
        node_t *to = &nodes[flights[i].to];
        aae_coop_receive(&to->m.coop, &to->m.game, &flights[i].from, flights[i].msg_sock, flights[i].data,
                         AAE_Coop_Packet_Size);
        delivered_count++;
        flights[i] = flights[--flying];
    }
}

// the world runs for this many ms: the network threads every ms, the frames every 16
static void run_world(int ms) {
    for (int t = 0; t < ms; t++) {
        world_clock++;
        network_deliver();
        for (int i = 0; i < node_count; i++)
            network_send(i);
        if (world_clock % 16 == 0) {
            for (int i = 0; i < node_count; i++) {
                node_t *n = &nodes[i];
                n->client_msec = game_frame(&n->m, n->m.value[AAE_Coop_Sv_Running].number == 1 ? &n->server_msec
                                                                                                  : NULL);
            }
        }
    }
}

// run until a condition holds (checked every frame), at most this long; how long it took, -1 for never
#define run_until(condition, most)                                                                                     \
    ({                                                                                                                 \
        int took = -1;                                                                                                 \
        for (int waited = 0; waited <= (most); waited += 16) {                                                         \
            if (condition) {                                                                                           \
                took = waited;                                                                                         \
                break;                                                                                                 \
            }                                                                                                          \
            run_world(16);                                                                                             \
        }                                                                                                              \
        took;                                                                                                          \
    })

// the host (10.0.0.1, its own player in slot 0) and three clients in slots 1 to 3
static void world_setup(uint32_t loss) {
    memset(nodes, 0, sizeof(nodes));
    flying = lost_count = delivered_count = 0;
    loss_percent = loss;
    world_clock = 10000000;
    node_count = 4;
    for (int i = 0; i < node_count; i++) {
        nodes[i].ip = (uint8_t)(1 + i);
        nodes[i].port = (uint16_t)(100 * (1 + i));
        machine_init(&nodes[i].m, &world_clock, i == 0 ? 1 : 0, 0x10000 * (uint64_t)(i + 1));
    }
    machine_t *host = &nodes[0].m;
    host->value[AAE_Coop_Max_Clients] = whole(8);
    seat(host, 0, 5, adr(1, 100, 2, 0), 11, 0);
    for (int i = 1; i < node_count; i++) {
        seat(host, i, 5, adr(nodes[i].ip, nodes[i].port, 4, 0), 100 + (uint64_t)i, 0);
        nodes[i].m.connection = 9;
        nodes[i].m.connection_data = true;
        nodes[i].m.server = adr(1, 100, 4, 4);
        nodes[i].m.server_id = 0x5e;
    }
    run_world(2000);
}

static bool clients_paused(int from, int to, bool paused) {
    for (int i = from; i <= to; i++) {
        machine_t *m = &nodes[i].m;
        if ((cl_paused_of(m) == 1) != paused || (m->coop_paused == 1) != paused ||
            (nodes[i].client_msec == 0) != paused)
            return false;
    }
    return true;
}

static void test_world(void) {
    for (uint32_t loss = 0; loss <= 30; loss += 30) {
        world_setup(loss);
        machine_t *host = &nodes[0].m;
        aae_coop_t *h = &host->coop;
        // everybody said hello; nothing is held
        CHECK(h->slots[1].nonce == nodes[1].m.coop.own_nonce && h->slots[3].nonce == nodes[3].m.coop.own_nonce);
        CHECK(nodes[0].server_msec == 16 && nodes[0].client_msec == 16 && clients_paused(1, 3, false));
        // the host's menu key: the game sets cl_paused 1 once CanPauseGame said yes
        CHECK(aae_coop_can_pause(h, &host->game));
        for (int i = 1; i <= 3; i++)
            CHECK(!aae_coop_can_pause(&nodes[i].m.coop, &nodes[i].m.game)); // a client can't
        set_cl_paused(host, 1);
        run_world(16);
        CHECK(nodes[0].client_msec == 0 && nodes[0].server_msec == 16 && h->state == 1);
        int took = run_until(clients_paused(1, 3, true) && nodes[0].server_msec == 0, 3000);
        CHECK(took >= 0 && took < 3500);
        // the server held only once every client acknowledged
        for (int i = 1; i <= 3; i++)
            CHECK(h->slots[i].acked == h->sequence && aae_coop_can_pause(&nodes[i].m.coop, &nodes[i].m.game));
        // it stays so
        run_world(8000);
        CHECK(clients_paused(1, 3, true) && nodes[0].server_msec == 0 && nodes[0].client_msec == 0 && !h->timed_out);
        // the host closes its menu: its server and view run in the same frame, the clients follow
        set_cl_paused(host, 0);
        run_world(16);
        CHECK(nodes[0].client_msec == 16 && nodes[0].server_msec == 16 && h->state == 0);
        took = run_until(clients_paused(1, 3, false), 3000);
        CHECK(took >= 0 && took < (loss == 0 ? 200 : 1000));

        // somebody joins while paused: the server runs until the newcomer acknowledged too
        set_cl_paused(host, 1);
        CHECK(run_until(nodes[0].server_msec == 0 && clients_paused(1, 3, true), 3000) >= 0);
        node_count = 5;
        nodes[4].ip = 5, nodes[4].port = 500;
        machine_init(&nodes[4].m, &world_clock, 0, 0x50000);
        nodes[4].m.connection = 9, nodes[4].m.connection_data = true;
        nodes[4].m.server = adr(1, 100, 4, 4), nodes[4].m.server_id = 0x5e;
        seat(host, 4, 3, adr(5, 500, 4, 0), 104, 0); // connecting
        run_world(32);
        CHECK(nodes[0].server_msec == 16 && nodes[0].client_msec == 0);
        seat(host, 4, 5, adr(5, 500, 4, 0), 104, 0);
        took = run_until(nodes[0].server_msec == 0 && clients_paused(1, 4, true), 3500);
        CHECK(took >= 0);
        set_cl_paused(host, 0);
        CHECK(run_until(clients_paused(1, 4, false), 3000) >= 0);

        // a client that never answers (no plugin, or its packets lost): given up after 3.5 s, for everybody
        nodes[2].mute = true;
        run_world(5000); // its last hello is older than 4 s now
        set_cl_paused(host, 1);
        uint64_t asked = world_clock;
        run_world(16);
        CHECK(nodes[0].server_msec == 16 && nodes[0].client_msec == 0);
        CHECK(run_until(h->timed_out, 4000) >= 0);
        CHECK(world_clock - asked >= 3500 && world_clock - asked < 3500 + 32);
        CHECK(h->state == 0 && nodes[0].client_msec == 16 && nodes[0].server_msec == 16);
        CHECK(run_until(clients_paused(1, 4, false), 3000) >= 0);
        CHECK(!aae_coop_can_pause(h, &host->game)); // until the host lets go of its menu
        set_cl_paused(host, 0);
        run_world(16);
        CHECK(aae_coop_can_pause(h, &host->game) && !h->timed_out);
        nodes[2].mute = false;

        // the host gone while paused: the clients unpause by themselves after 10 s
        set_cl_paused(host, 1);
        CHECK(run_until(nodes[0].server_msec == 0 && clients_paused(1, 4, true), 4000) >= 0);
        nodes[0].mute = true;
        uint64_t gone = world_clock;
        CHECK(run_until(clients_paused(1, 4, false), 12000) >= 0);
        CHECK(world_clock - gone > 9000 && world_clock - gone < 10000 + 200);
        for (int i = 0; i < node_count; i++)
            CHECK(nodes[i].m.lock_errors == 0 && nodes[i].m.lock_depth == 0);
        CHECK(delivered_count > 0 && (loss == 0 || lost_count > 0));
    }
}

// one machine, two players: the game's own refusal skipped, both frames held; one player: the game's own pause
static void test_split_screen(void) {
    uint64_t clock = 1200000;
    machine_t m;
    machine_init(&m, &clock, 1, 0xa000);
    seat(&m, 0, 5, adr(1, 100, 2, 0), 11, 0);
    seat(&m, 1, 5, adr(1, 100, 2, 1), 12, 0);
    m.table = true;
    int server;
    CHECK(game_frame(&m, &server) == 16 && server == 16 && aae_coop_can_pause(&m.coop, &m.game));
    set_cl_paused(&m, 1);
    clock += 16;
    CHECK(!aae_coop_check_paused(&m.coop, &m.game));
    CHECK(game_frame(&m, &server) == 0 && server == 0 && m.cl_paused_sets == 0 && m.coop_paused == 0);
    aae_coop_outgoing_t out[AAE_Coop_Slots + 2];
    CHECK(aae_coop_send(&m.coop, &m.game, out) == 0); // nothing over the network
    set_cl_paused(&m, 0);
    clock += 16;
    CHECK(game_frame(&m, &server) == 16 && server == 16);
    m.clients[1].state = 0;
    set_cl_paused(&m, 1);
    clock += 16;
    CHECK(aae_coop_check_paused(&m.coop, &m.game) && game_frame(&m, &server) == 0 && server == 0);
}

// ---- the hooks against a made-up game

static struct {
    void *address, *replacement, **original;
} hooked[16];
static int hooks;

static int game_server_frames, game_server_msec;
static int fake_game_SV_Frame(int msec) {
    game_server_frames++;
    game_server_msec = msec;
    return msec + 1000;
}
static int game_checks;
static int fake_game_SV_CheckPaused(void) {
    game_checks++;
    return 7;
}
static int game_client_frames, game_client_msec, game_client_local;
static float game_client_frac;
static void fake_game_CL_Frame(int local_client, int msec, float frac) {
    game_client_frames++;
    game_client_local = local_client, game_client_msec = msec, game_client_frac = frac;
}
static const char *tokenized;
static void fake_game_tokenize(const char *text) {
    tokenized = text;
}
// what NET_GetPacket has to give: packets with the address and socket they came from
static struct {
    uint8_t data[64];
    uint32_t size;
    aae_coop_adr_t from;
    int sock;      // the sender's
    int for_sock;  // the one it was sent to (msg + 0x34)
} arriving[40];
static int arrived, handed_out, unrestored;
static uint8_t *msg_buffer;
static int fake_game_NET_GetPacket(aae_coop_adr_t *from, uint8_t *msg) {
    uint8_t *data;
    memcpy(&data, msg + Msg_Data, sizeof(data));
    if (data != msg_buffer)
        unrestored++;
    if (handed_out == arrived)
        return 0;
    // as the game: the header byte, then the packet; the data pointer moves past the header byte
    typeof(arriving[0]) *packet = &arriving[handed_out++];
    data[0] = (uint8_t)(packet->for_sock << 4 | packet->sock);
    memcpy(data + 1, packet->data, packet->size);
    data++;
    memcpy(msg + Msg_Data, &data, sizeof(data));
    memcpy(msg + Msg_Size, &packet->size, sizeof(packet->size));
    int32_t for_sock = packet->for_sock;
    memcpy(msg + Msg_Sock, &for_sock, sizeof(for_sock));
    *from = packet->from;
    uint32_t net_id = (uint32_t)packet->sock;
    memcpy(from->bytes + Adr_Net_Id, &net_id, sizeof(net_id));
    return 1;
}
static int game_sends, game_sent_local;
static bool fake_game_send_local(int local_client) {
    game_sends++;
    game_sent_local = local_client;
    return local_client == 0;
}
static struct {
    int sock, length;
    uint8_t data[AAE_Coop_Packet_Size];
    uint64_t low, high;
} packets_out[8];
static int packets_sent;
static bool fake_NET_SendPacket(int sock, int length, const void *data, uint64_t to_low, uint64_t to_high) {
    if (packets_sent < 8) {
        packets_out[packets_sent].sock = sock, packets_out[packets_sent].length = length;
        memcpy(packets_out[packets_sent].data, data, AAE_Coop_Packet_Size);
        packets_out[packets_sent].low = to_low, packets_out[packets_sent].high = to_high;
    }
    packets_sent++;
    return true;
}
static int fake_local_socket(int local_client) {
    return local_client == 0 ? 0 : -1;
}

int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 16)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    void *game_side = NULL;
    if (out_origin_func == (void **)&orig_SV_Frame)
        game_side = (void *)fake_game_SV_Frame;
    else if (out_origin_func == (void **)&orig_SV_CheckPaused)
        game_side = (void *)fake_game_SV_CheckPaused;
    else if (out_origin_func == (void **)&orig_CL_Frame)
        game_side = (void *)fake_game_CL_Frame;
    else if (out_origin_func == (void **)&orig_Cmd_TokenizeString)
        game_side = (void *)fake_game_tokenize;
    else if (out_origin_func == (void **)&orig_NET_GetPacket)
        game_side = (void *)fake_game_NET_GetPacket;
    else if (out_origin_func == (void **)&orig_Netchan_SendLocalClient)
        game_side = (void *)fake_game_send_local;
    *out_origin_func = game_side;
    return 0;
}
static bool was_hooked(uint32_t place, void *replacement, void **original) {
    for (int i = 0; i < hooks && i < 16; i++)
        if (hooked[i].address == (void *)(0x100000000 + (uint64_t)place))
            return hooked[i].replacement == replacement && hooked[i].original == original;
    return false;
}

// the game's dvars: the kind at +0x1c, the value at +0x28
typedef struct {
    uint8_t bytes[0x40];
} fake_dvar_t;
static fake_dvar_t d_cl_paused, d_sv_paused, d_max_clients, d_sv_running, d_mapname, d_pause_remote;
static uint8_t *p_cl_paused = d_cl_paused.bytes, *p_sv_paused = d_sv_paused.bytes,
               *p_max_clients = d_max_clients.bytes, *p_sv_running = d_sv_running.bytes;
static bool pause_remote_there;
static char pause_remote_text[16], set_names[8][64], set_values[8][16];
static int sets;
static void dvar_number(fake_dvar_t *dvar, uint32_t type, int32_t value) {
    memset(dvar->bytes, 0, sizeof(dvar->bytes));
    memcpy(dvar->bytes + Dvar_Type, &type, sizeof(type));
    if (type == Dvar_Bool)
        dvar->bytes[Dvar_Value] = (uint8_t)value;
    else
        memcpy(dvar->bytes + Dvar_Value, &value, sizeof(value));
}
static void dvar_text(fake_dvar_t *dvar, const char *text) {
    uint32_t type = Dvar_String;
    memset(dvar->bytes, 0, sizeof(dvar->bytes));
    memcpy(dvar->bytes + Dvar_Type, &type, sizeof(type));
    memcpy(dvar->bytes + Dvar_Value, &text, sizeof(text));
}
static void *fake_find(uint32_t hash) {
    if (hash == gsc_hash("mapname"))
        return d_mapname.bytes;
    if (hash == gsc_hash("aae_coop_pause_remote") && pause_remote_there)
        return d_pause_remote.bytes;
    return NULL;
}
static const char *fake_get_string(void *dvar) {
    const char *text;
    memcpy(&text, (uint8_t *)dvar + Dvar_Value, sizeof(text));
    return text;
}
static void fake_set_from_string(const char *name, const char *value, bool create) {
    if (sets < 8) {
        snprintf(set_names[sets], sizeof(set_names[0]), "%s", name);
        snprintf(set_values[sets], sizeof(set_values[0]), "%s%s", value, create ? "" : " (not made)");
    }
    sets++;
    if (strcmp(name, "aae_coop_pause_remote") == 0) {
        pause_remote_there = true;
        snprintf(pause_remote_text, sizeof(pause_remote_text), "%s", value);
        dvar_text(&d_pause_remote, pause_remote_text);
    } else if (strcmp(name, "cl_paused") == 0) {
        dvar_number(&d_cl_paused, Dvar_Int, atoi(value));
    }
}

static uint8_t *table, *table_pointer, *connection_data, *connection_pointer;
static int32_t connections[2 * 0x1078 / 4];
static uint64_t test_clock = 50000000, plugin_clock = 5000;
static uint64_t test_now(void) {
    return test_clock;
}
static uint64_t test_plugin_now(void) {
    return plugin_clock;
}
static char loaded_name[Mod_Name_Size + 8];
static void set_mod(bool loaded) {
    memset(loaded_name, 0, sizeof(loaded_name));
    if (loaded)
        snprintf(loaded_name, sizeof(loaded_name), "%s", strrchr(mod_folder, '/') + 1);
    plugin_clock += Plugin_Look_Ms;
}
static void put_client(int index, uint32_t state, aae_coop_adr_t where, uint64_t id, int32_t bot) {
    uint8_t *entry = table + (size_t)index * Client_Size;
    memcpy(entry + Client_State, &state, sizeof(state));
    memcpy(entry + Client_Adr, where.bytes, sizeof(where.bytes));
    memcpy(entry + Client_Id, &id, sizeof(id));
    memcpy(entry + Client_Bot, &bot, sizeof(bot));
}

static void test_init(void) {
    const uint64_t base = 0x100000000;
    aae_coop_pause_init(base, 0);
    CHECK(hooks == 0 && !installed);
    const uint32_t places[] = { ADDR_SV_Frame, ADDR_SV_CheckPaused, ADDR_CL_Frame, ADDR_CanPauseGame_Test,
                                ADDR_Cmd_TokenizeString, ADDR_NET_GetPacket, ADDR_Netchan_SendLocalClient };
    for (size_t i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
        stub_missing_for = (int)places[i];
        aae_coop_pause_init(base, 1);
        CHECK(hooks == 0 && !installed);
    }
    have_stubs = false, stub_missing_for = -1;
    aae_coop_pause_init(base, 1);
    CHECK(hooks == 0 && !installed);
    have_stubs = true;
    aae_coop_pause_init(base, 1);
    CHECK(hooks == 7 && installed);
    CHECK(was_hooked(ADDR_SV_Frame, (void *)fake_SV_Frame, (void **)&orig_SV_Frame));
    CHECK(was_hooked(ADDR_SV_CheckPaused, (void *)fake_SV_CheckPaused, (void **)&orig_SV_CheckPaused));
    CHECK(was_hooked(ADDR_CL_Frame, (void *)fake_CL_Frame, (void **)&orig_CL_Frame));
    CHECK(was_hooked(ADDR_CanPauseGame_Test, (void *)handler_Can_Pause, &orig_Can_Pause));
    CHECK(was_hooked(ADDR_Cmd_TokenizeString, (void *)fake_Cmd_TokenizeString, (void **)&orig_Cmd_TokenizeString));
    CHECK(was_hooked(ADDR_NET_GetPacket, (void *)fake_NET_GetPacket, (void **)&orig_NET_GetPacket));
    CHECK(was_hooked(ADDR_Netchan_SendLocalClient, (void *)fake_Netchan_SendLocalClient,
                     (void **)&orig_Netchan_SendLocalClient));
    CHECK(Dvar_FindByHash == (void *)(base + ADDR_Dvar_FindByHash) &&
          Dvar_GetString == (void *)(base + ADDR_Dvar_GetString) &&
          Dvar_SetFromStringByName == (void *)(base + ADDR_Dvar_SetFromStringByName) &&
          NET_SendPacket == (void *)(base + ADDR_NET_SendPacket) &&
          CL_LocalClientSocket == (void *)(base + ADDR_Com_LocalClient_GetControllerIndex));
    CHECK(dvar_cl_paused == (void *)(base + ADDR_dvar_cl_paused) &&
          dvar_sv_paused == (void *)(base + ADDR_dvar_sv_paused) &&
          dvar_max_clients == (void *)(base + ADDR_dvar_sv_maxclients) &&
          dvar_sv_running == (void *)(base + ADDR_sv_running));
    CHECK(server_clients == (void *)(base + ADDR_svs_clients) &&
          client_connections == (void *)(base + ADDR_CLIENT_GLOBALS) &&
          connection_state == (void *)(base + ADDR_CLIENT_CONNECTIONS + 8));
    CHECK(com_frame_runs_server == (void *)(base + ADDR_Com_Frame_SV_FrameReturn) &&
          com_frame_runs_server_late == (void *)(base + ADDR_Com_Frame_SV_FrameReturn2) &&
          netchan_receives == (void *)(base + ADDR_Netchan_GetPacketReturn) &&
          client_takes_command == (void *)(base + ADDR_CL_ServerCommand_TokenizeReturn) && now_ms == uptime_ms);
    // the places and the game's structures as they were read from its code
    CHECK(ADDR_SV_CheckPaused == 0x11d5ba4 && ADDR_SV_Frame == 0x11d64a6 && ADDR_Com_Frame_SV_FrameReturn == 0xe75a3e &&
          ADDR_Com_Frame_SV_FrameReturn2 == 0xe75a54 && ADDR_CL_Frame == 0xe1a4c3 &&
          ADDR_CanPauseGame_Test == 0x136b736 &&
          ADDR_Cmd_TokenizeString == 0xf76456 && ADDR_CL_ServerCommand_TokenizeReturn == 0xe2dfc7 &&
          ADDR_NET_GetPacket == 0xd6dacf && ADDR_Netchan_GetPacketReturn == 0x14012ca &&
          ADDR_Netchan_SendLocalClient == 0x13ff83e && ADDR_NET_SendPacket == 0xd6dc30 &&
          ADDR_Com_LocalClient_GetControllerIndex == 0xeae63e && ADDR_dvar_cl_paused == 0x118df830 &&
          ADDR_dvar_sv_paused == 0x118dfa78 && ADDR_dvar_sv_maxclients == 0x118df620 && ADDR_svs_clients == 0x144e7c18);
    CHECK(Client_Size == 0xe5270 && Client_State == 0 && Client_Adr == 0x2c && Client_Id == 0x55c8 &&
          Client_Bot == 0xbb460 && Clc_Server == 0x2455c && Clc_Server_Id == 0x24b68 && Msg_Data == 8 &&
          Msg_Size == 0x1c && Msg_Sock == 0x34 && Msg_Kept == 0x38 && Dvar_Type == 0x1c && Dvar_Value == 0x28 &&
          Can_Pause_Test == 0x1790);
}

// the made-up game behind the hooks: this machine hosts zm_theater with its own player and one of another machine
static void game_setup(void) {
    Dvar_FindByHash = fake_find;
    Dvar_GetString = fake_get_string;
    Dvar_SetFromStringByName = fake_set_from_string;
    NET_SendPacket = fake_NET_SendPacket;
    CL_LocalClientSocket = fake_local_socket;
    dvar_cl_paused = &p_cl_paused, dvar_sv_paused = &p_sv_paused, dvar_max_clients = &p_max_clients;
    dvar_sv_running = &p_sv_running;
    table = calloc(4, Client_Size);
    table_pointer = table;
    server_clients = &table_pointer;
    connection_data = calloc(1, 0x25780);
    connection_pointer = connection_data;
    client_connections = &connection_pointer;
    connection_state = (const int32_t *)((uint8_t *)connections + 8);
    msg_buffer = calloc(1, 0x1000);
    static const int dummy = 0;
    com_frame_runs_server = &dummy, com_frame_runs_server_late = &dummy + 1;
    now_ms = test_now;
    dvar_number(&d_cl_paused, Dvar_Int, 0);
    dvar_number(&d_sv_paused, Dvar_Int, 0);
    dvar_number(&d_max_clients, Dvar_Int, 4);
    dvar_number(&d_sv_running, Dvar_Bool, 1);
    dvar_text(&d_mapname, "zm_theater");
    put_client(0, 5, adr(1, 100, 2, 0), 11, 0);
    put_client(1, 5, adr(2, 200, 4, 0), 12, 0);
    connections[2] = 9;
}

static void test_game_side(void) {
    aae_coop_client_t client;
    CHECK(game_client(&game, 1, &client) && client.state == 5 && client.id == 12 && client.bot == 0 &&
          memcmp(&client.adr, adr(2, 200, 4, 0).bytes, 16) == 0);
    put_client(3, 4, adr(7, 700, 3, 2), 0x1122334455667788ull, -5);
    CHECK(game_client(&game, 3, &client) && client.state == 4 && client.id == 0x1122334455667788ull &&
          client.bot == -5 && memcmp(&client.adr, adr(7, 700, 3, 2).bytes, 16) == 0);
    put_client(3, 0, adr(0, 0, 0, 0), 0, 0);
    table_pointer = NULL;
    CHECK(!game_client(&game, 0, &client));
    table_pointer = table;
    int states[][2] = { { -1, -1 }, { -5, -1 }, { 0, 0 }, { 4, 4 }, { 11, 11 }, { 12, -1 }, { 9, 9 } };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        connections[2] = states[i][0];
        CHECK(game_connection(&game) == states[i][1]);
    }
    aae_coop_adr_t where = adr(1, 100, 4, 4), got;
    uint64_t id = 0x5e, got_id;
    memcpy(connection_data + Clc_Server, where.bytes, 16);
    memcpy(connection_data + Clc_Server_Id, &id, sizeof(id));
    CHECK(game_server(&game, &got, &got_id) && memcmp(&got, &where, 16) == 0 && got_id == 0x5e);
    connection_pointer = NULL;
    CHECK(!game_server(&game, &got, &got_id));
    connection_pointer = connection_data;
    CHECK(game_local_sock(&game) == 0);
    // the dvars: a bool's value is a byte, an int's and an enum's 4 bytes, a text's a pointer; others aren't numbers
    aae_coop_value_t value;
    fake_dvar_t probe;
    dvar_number(&probe, Dvar_Bool, 1);
    probe.bytes[Dvar_Value + 1] = 7;
    read_dvar(probe.bytes, &value);
    CHECK(value.kind == AAE_Coop_Number && value.number == 1);
    dvar_number(&probe, Dvar_Int, -3);
    read_dvar(probe.bytes, &value);
    CHECK(value.kind == AAE_Coop_Number && value.number == -3);
    dvar_number(&probe, Dvar_Enum, 2);
    read_dvar(probe.bytes, &value);
    CHECK(value.kind == AAE_Coop_Number && value.number == 2);
    dvar_text(&probe, "1");
    read_dvar(probe.bytes, &value);
    CHECK(value.kind == AAE_Coop_Text && strcmp(value.text, "1") == 0);
    uint32_t kinds[] = { 0, 2, 3, 4, 5, 9, 0xf };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        dvar_number(&probe, kinds[i], 1);
        read_dvar(probe.bytes, &value);
        CHECK(value.kind == AAE_Coop_Other);
    }
    read_dvar(NULL, &value);
    CHECK(value.kind == AAE_Coop_None);
    game_value(&game, AAE_Coop_Map, &value);
    CHECK(value.kind == AAE_Coop_Text && strcmp(value.text, "zm_theater") == 0);
    game_value(&game, AAE_Coop_Max_Clients, &value);
    CHECK(value.number == 4);
    game_value(&game, AAE_Coop_Sv_Running, &value);
    CHECK(value.number == 1);
    dvar_number(&d_sv_paused, Dvar_Int, 3);
    game_value(&game, AAE_Coop_Sv_Paused, &value);
    CHECK(value.number == 3);
    dvar_number(&d_sv_paused, Dvar_Int, 0);
    game_value(&game, AAE_Coop_Cl_Paused, &value);
    CHECK(value.kind == AAE_Coop_Number && value.number == 0);
    game_value(&game, AAE_Coop_Pause_Remote, &value);
    CHECK(value.kind == AAE_Coop_None);
    game_set_dvar(&game, "cl_paused", "1");
    CHECK(sets == 1 && strcmp(set_names[0], "cl_paused") == 0 && strcmp(set_values[0], "1") == 0 &&
          number(&game, AAE_Coop_Cl_Paused) == 1);
    game_set_dvar(&game, "cl_paused", "0");
    sets = 0;
    CHECK(game_now(&game) == test_clock);
}

static int call_server_frame(int msec, bool from_com_frame) {
    game_server_frames = 0;
    return server_frame(msec, from_com_frame);
}

static void test_hooks(void) {
    // without the mod nothing is done, and nothing is made
    set_mod(false);
    CHECK(call_server_frame(16, true) == 1016 && game_server_frames == 1 && game_server_msec == 16);
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(game_client_frames == 1 && game_client_msec == 16 && sets == 0 && coop.role == -1);
    // with it: its start makes aae_coop_pause_remote with "0", and the frames go by the rules
    set_mod(true);
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(sets == 2 && strcmp(set_names[0], "aae_coop_pause_remote") == 0 && strcmp(set_values[0], "0") == 0);
    CHECK(strcmp(set_names[1], "aae_coop_paused") == 0 && strcmp(set_values[1], "0") == 0);
    CHECK(coop.role == 1 && coop.sock == 4 && coop.host_nonce != 0 && coop.mask == 0x2 && game_client_msec == 16);
    // the host may pause (the mod's switch on): its view is held at once
    dvar_text(&d_pause_remote, "1");
    dvar_number(&d_cl_paused, Dvar_Int, 1);
    test_clock += 16;
    CHECK(call_server_frame(16, true) == 1016 && game_server_msec == 16);
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(game_client_msec == 0 && game_client_frac == 0.0f && coop.state == 1 && coop.host_view_frozen);
    // SV_Frame's third caller: the game's own, nothing of the rules
    uint64_t sequence = coop.sequence;
    put_client(2, 5, adr(3, 300, 4, 0), 13, 0);
    test_clock += 16;
    CHECK(call_server_frame(0, false) == 1000 && game_server_frames == 1 && coop.sequence == sequence);
    call_server_frame(16, true);
    CHECK(coop.sequence == sequence + 1 && coop.mask == 0x6);
    put_client(2, 0, adr(0, 0, 0, 0), 0, 0);
    test_clock += 16;
    call_server_frame(16, true);
    // the server's pause test: the game's own one is not asked while the host's pause is asked for
    game_checks = 0;
    CHECK(fake_SV_CheckPaused() == 0 && game_checks == 0);
    dvar_number(&d_cl_paused, Dvar_Int, 0);
    CHECK(fake_SV_CheckPaused() == 7 && game_checks == 1);
    // CanPauseGame: rcx pointed at a 1 of ours for a yes, left alone otherwise
    detour_registers_t registers = { .rcx = 0x1113b7880 };
    handler_Can_Pause(&registers);
    CHECK(registers.rcx == (uint64_t)(uintptr_t)&say_yes - Can_Pause_Test && say_yes == 1);
    dvar_text(&d_pause_remote, "0");
    registers.rcx = 0x1113b7880;
    handler_Can_Pause(&registers);
    CHECK(registers.rcx == 0x1113b7880);
    dvar_text(&d_pause_remote, "1");
    // the network thread: a hello from the client of slot 1 is taken out; the game's own packet comes through
    test_clock += 16;
    fake_CL_Frame(0, 16, 0.5f);
    arrived = handed_out = unrestored = 0;
    memset(msg_buffer, 0, 0x1000);
    uint8_t msg[0x40] = { 0 };
    memcpy(msg + Msg_Data, &msg_buffer, sizeof(msg_buffer));
    uint32_t most = 0x4000;
    memcpy(msg + 0x18, &most, sizeof(most));
    aae_coop_packet(arriving[0].data, Kind_Hello, 0, 0xc11e, 0, 0);
    arriving[0].size = AAE_Coop_Packet_Size, arriving[0].from = adr(2, 200, 4, 0), arriving[0].sock = 0,
    arriving[0].for_sock = 4;
    memcpy(arriving[1].data, "\xff\xff\xff\xffgame", 8);
    arriving[1].size = 8, arriving[1].from = adr(2, 200, 4, 0), arriving[1].for_sock = 4;
    arrived = 2;
    aae_coop_adr_t from;
    CHECK(net_get_packet(&from, msg, true) == 1 && handed_out == 2 && unrestored == 0);
    const uint8_t *data;
    memcpy(&data, msg + Msg_Data, sizeof(data));
    CHECK(memcmp(data, "\xff\xff\xff\xffgame", 8) == 0 && coop.slots[1].nonce == 0xc11e);
    // not the network thread's call: as the game has it
    memcpy(msg + Msg_Data, &msg_buffer, sizeof(msg_buffer));
    aae_coop_packet(arriving[2].data, Kind_Hello, 0, 0xd11e, 0, 0);
    arriving[2].size = AAE_Coop_Packet_Size, arriving[2].from = adr(2, 200, 4, 0), arriving[2].for_sock = 4;
    arrived = 3;
    CHECK(net_get_packet(&from, msg, false) == 1 && handed_out == 3 && coop.slots[1].nonce == 0xc11e);
    // packets of these that aren't usable are taken out all the same; 32 in a row, then 0
    memcpy(msg + Msg_Data, &msg_buffer, sizeof(msg_buffer));
    arrived = handed_out = unrestored = 0;
    for (int i = 0; i < 34; i++) {
        aae_coop_packet(arriving[i].data, Kind_Hello, 0, 0xe000 + (uint64_t)i, 0, 0);
        arriving[i].size = i == 0 ? 8 : AAE_Coop_Packet_Size, arriving[i].from = adr(2, 200, 4, 0);
        arriving[i].for_sock = 4;
    }
    arrived = 34;
    CHECK(net_get_packet(&from, msg, true) == 0 && handed_out == 32 && unrestored == 0);
    CHECK(coop.slots[1].nonce == 0xe000 + 31);
    memcpy(&data, msg + Msg_Data, sizeof(data));
    CHECK(data == msg_buffer); // as it was before the call
    CHECK(net_get_packet(&from, msg, true) == 0 && handed_out == 34);
    // nothing there: 0 at once
    CHECK(net_get_packet(&from, msg, true) == 0 && handed_out == 34);
    // the sending: behind local client 0's own, the host's state to the client that said hello
    packets_sent = 0;
    CHECK(fake_Netchan_SendLocalClient(1) == false && game_sends > 0 && game_sent_local == 1 && packets_sent == 0);
    CHECK(fake_Netchan_SendLocalClient(0) == true && game_sent_local == 0 && packets_sent == 1);
    aae_coop_adr_t to = adr(2, 200, 4, 0);
    uint64_t low, high;
    memcpy(&low, to.bytes, 8);
    memcpy(&high, to.bytes + 8, 8);
    uint8_t expect[AAE_Coop_Packet_Size];
    aae_coop_packet(expect, Kind_State, coop.host_nonce, 0xe000 + 33, coop.sequence, coop.state);
    CHECK(packets_out[0].sock == 4 && packets_out[0].length == AAE_Coop_Packet_Size && packets_out[0].low == low &&
          packets_out[0].high == high && memcmp(packets_out[0].data, expect, sizeof(expect)) == 0);
    // server commands: only where the client takes them apart
    tokenized = NULL;
    const char *command = "q @ 1";
    CHECK(server_command(command, false) == command);
    coop.remote_state = 0;
    CHECK(strcmp(server_command(command, true), "q cl_paused \"1\"") == 0 && coop.remote_state == 0); // the host
    dvar_number(&d_sv_running, Dvar_Bool, 0);
    test_clock += 16;
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(coop.role == 0 && coop.client_in_map);
    CHECK(strcmp(server_command(command, true), "q cl_paused \"1\"") == 0 && coop.remote_state == 1);
    fake_Cmd_TokenizeString(command);
    CHECK(tokenized == command); // not where the client takes a command apart
    tokenize(command, true);
    CHECK(tokenized != command && strcmp(tokenized, "q cl_paused \"1\"") == 0);
    // the client paused by the host through it: aae_coop_paused 1, held, CanPauseGame yes, the server's test answered
    sets = 0;
    test_clock += 16;
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(game_client_msec == 0 && sets == 1 && strcmp(set_names[0], "aae_coop_paused") == 0 &&
          strcmp(set_values[0], "1") == 0);
    registers.rcx = 1;
    handler_Can_Pause(&registers);
    CHECK(registers.rcx == (uint64_t)(uintptr_t)&say_yes - Can_Pause_Test);
    game_checks = 0;
    CHECK(fake_SV_CheckPaused() == 0 && game_checks == 0);
    // a state from the host through NET_GetPacket: taken, put into effect at the next frame, then acknowledged
    memcpy(msg + Msg_Data, &msg_buffer, sizeof(msg_buffer));
    arrived = handed_out = 0;
    aae_coop_packet(arriving[0].data, Kind_State, 0x77, coop.own_nonce, 5, 1);
    arriving[0].size = AAE_Coop_Packet_Size, arriving[0].from = adr(1, 100, 4, 4), arriving[0].sock = 4;
    arriving[0].for_sock = 0;
    arrived = 1;
    CHECK(net_get_packet(&from, msg, true) == 0 && coop.news == 1 && coop.known_state == 1);
    test_clock += 16;
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(coop.acked_news == 1 && coop.applied && number(&game, AAE_Coop_Cl_Paused) == 1 && game_client_msec == 0);
    packets_sent = 0;
    fake_Netchan_SendLocalClient(0);
    CHECK(packets_sent >= 1 && packets_out[packets_sent - 1].data[Packet_Kind] == Kind_Ack);
    // the mod gone: nothing sent any more, even before the next frame forgets everything
    set_mod(false);
    test_clock += 800;
    packets_sent = 0;
    fake_Netchan_SendLocalClient(0);
    CHECK(packets_sent == 0);
    set_mod(true);
    fake_Netchan_SendLocalClient(0);
    CHECK(packets_sent >= 1 && packets_out[0].data[Packet_Kind] == Kind_Hello);
    // the mod gone: everything forgotten at the next frame, the game's own behaviour everywhere
    set_mod(false);
    test_clock += 16;
    game_checks = 0;
    CHECK(fake_SV_CheckPaused() == 7 && game_checks == 1);
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(game_client_msec == 16 && coop.role == -1 && coop.remote_state == 0 && !coop.remote_known);
    registers.rcx = 1;
    handler_Can_Pause(&registers);
    CHECK(registers.rcx == 1);
    CHECK(server_command(command, true) == command);
    packets_sent = 0;
    fake_Netchan_SendLocalClient(0);
    CHECK(packets_sent == 0);
    memcpy(msg + Msg_Data, &msg_buffer, sizeof(msg_buffer));
    arrived = handed_out = 0;
    aae_coop_packet(arriving[0].data, Kind_Hello, 0, 0xf000, 0, 0);
    arriving[0].size = AAE_Coop_Packet_Size;
    arrived = 1;
    CHECK(net_get_packet(&from, msg, true) == 1 && handed_out == 1); // the game gets it, and drops it itself
    // the mod back: started anew; aae_coop_pause_remote is there now, so it isn't made again
    set_mod(true);
    sets = 0;
    test_clock += 16;
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(sets == 1 && strcmp(set_names[0], "aae_coop_paused") == 0 && coop.role == 0);
    // gone and back once more, the dvar gone meanwhile: made again
    set_mod(false);
    test_clock += 16;
    fake_CL_Frame(0, 16, 0.5f);
    pause_remote_there = false;
    set_mod(true);
    sets = 0;
    test_clock += 16;
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(sets == 2 && strcmp(set_names[0], "aae_coop_pause_remote") == 0 && strcmp(set_values[0], "0") == 0);
    // alone on this machine's server: with the mod both frames are held while cl_paused is 1 and CanPauseGame says
    // yes; without it, none of that
    dvar_number(&d_sv_running, Dvar_Bool, 1);
    put_client(1, 0, adr(0, 0, 0, 0), 0, 0);
    dvar_number(&d_cl_paused, Dvar_Int, 1);
    test_clock += 16;
    fake_CL_Frame(0, 16, 0.5f);
    test_clock += 16;
    CHECK(call_server_frame(16, true) == 1000 && game_server_msec == 0);
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(coop.role == 1 && game_client_msec == 0 && game_client_frac == 0.0f);
    registers.rcx = 1;
    handler_Can_Pause(&registers);
    CHECK(registers.rcx == (uint64_t)(uintptr_t)&say_yes - Can_Pause_Test);
    game_checks = 0;
    CHECK(fake_SV_CheckPaused() == 7 && game_checks == 1); // one player: the game's own pause
    set_mod(false);
    test_clock += 16;
    CHECK(call_server_frame(16, true) == 1016 && game_server_msec == 16);
    fake_CL_Frame(0, 16, 0.5f);
    CHECK(game_client_msec == 16 && game_client_frac == 0.5f);
    registers.rcx = 1;
    handler_Can_Pause(&registers);
    CHECK(registers.rcx == 1);
    // the debug lines change nothing: what they say is worked out on a copy
    set_mod(true);
    test_clock += 16;
    fake_CL_Frame(0, 16, 0.5f);
    debug = 1;
    static aae_coop_t before;
    memcpy(&before, &coop, sizeof(coop));
    test_clock += 20;
    report();
    CHECK(memcmp(&before, &coop, sizeof(coop)) == 0);
    debug = 0;
}

int main(void) {
    char template[] = "/tmp/bo3macfix-cooppause-test-XXXXXX";
    if (mkdtemp(template) == NULL)
        return 2;
    snprintf(mod_folder, sizeof(mod_folder), "%s/2631943123", template);
    char command[4096 + 64], plugin_file[4096 + 64];
    snprintf(command, sizeof(command), "mkdir -p '%s/T7Overcharged/plugins'", mod_folder);
    if (system(command) != 0)
        return 2;
    snprintf(plugin_file, sizeof(plugin_file), "%s/%s", mod_folder, Plugin_Path);
    close(open(plugin_file, O_CREAT | O_WRONLY, 0644));
    plugin_now_ms = test_plugin_now;

    test_packets();
    test_start();
    test_reading();
    test_counts();
    test_host_rules();
    test_host_slots();
    test_receive();
    test_client();
    test_client_server_info();
    test_roles();
    test_forget();
    test_sending();
    test_early_clock();
    test_gate();
    test_frames();
    test_can_pause();
    test_client_server_frame();
    test_commands();
    test_world();
    test_split_screen();

    test_init();
    loaded_mod = loaded_name;
    game_setup();
    test_game_side();
    test_hooks();

    free(table);
    free(connection_data);
    free(msg_buffer);
    snprintf(command, sizeof(command), "rm -rf '%s'", template);
    if (system(command) != 0)
        failures++;
    if (failures != 0) {
        printf("test_aae_coop_pause: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_coop_pause: all passed\n");
    return 0;
}
