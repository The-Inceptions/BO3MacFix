// aae_coop_pause.c under the fuzzer (make fuzz): the plugin's packets from other machines, the server commands a
// host sends, and the frames between them, in any order, against a made-up game whose answers the input gives.
// Nothing goes over a network: the packets are handed to the receiving function straight.
//
// The input is a list of steps, each a kind byte and what that kind takes: a packet (48 bytes, or as many as are
// left, from one of four addresses), a server command (text up to a newline), a client frame, a server frame, the
// sending, a change of what the game answers (map, sv_running, cl_paused, the clients' table), or time passing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/aae_plugin.c"
#include "../../source/aae_coop_pause.c"

// what aae_coop_pause.c and aae_plugin.c ask of the rest of BO3MacFix
const char *t7o_mod_folder(void) {
    return "";
}
bool prepatch_has_hook(uint32_t offset) {
    (void)offset;
    return false;
}
int DobbyHook(void *address, void *fake_func, void **out_origin_func) {
    (void)address, (void)fake_func, (void)out_origin_func;
    return 0;
}

// the made-up game
static struct {
    uint64_t now;
    int map; // 0 none, 1 a zombies map, 2 the frontend, 3 a name that isn't one
    int64_t running, cl_paused, sv_paused, max_clients, pause_remote;
    aae_coop_client_t clients[AAE_Coop_Slots + 4];
    int connection;
    aae_coop_adr_t server;
    uint64_t server_id;
    bool has_server;
    uint64_t nonce;
    int locks;
} fake;

static uint64_t fake_now(const aae_coop_game_t *g) {
    (void)g;
    return fake.now;
}
static void fake_value(const aae_coop_game_t *g, int dvar, aae_coop_value_t *value) {
    (void)g;
    memset(value, 0, sizeof(*value));
    static const char *const maps[] = { NULL, "zm_theater", "core_frontend", "zm the(ater)" };
    switch (dvar) {
    case AAE_Coop_Map:
        value->kind = fake.map == 0 ? AAE_Coop_None : AAE_Coop_Text;
        value->text = maps[fake.map & 3];
        break;
    case AAE_Coop_Sv_Running: value->kind = AAE_Coop_Number, value->number = fake.running; break;
    case AAE_Coop_Cl_Paused: value->kind = AAE_Coop_Number, value->number = fake.cl_paused; break;
    case AAE_Coop_Sv_Paused: value->kind = AAE_Coop_Number, value->number = fake.sv_paused; break;
    case AAE_Coop_Max_Clients: value->kind = AAE_Coop_Number, value->number = fake.max_clients; break;
    case AAE_Coop_Pause_Remote:
        value->kind = fake.pause_remote < 0 ? AAE_Coop_Text : AAE_Coop_Number;
        value->number = fake.pause_remote;
        value->text = "1";
        break;
    }
}
static void fake_set_dvar(const aae_coop_game_t *g, const char *name, const char *value) {
    (void)g;
    if (strcmp(name, "cl_paused") == 0)
        fake.cl_paused = value[0] == '1';
}
static bool fake_client(const aae_coop_game_t *g, int index, aae_coop_client_t *client) {
    (void)g;
    // the game's table holds as many as its count dvar says; reading past that would be the fault looked for
    if (index < 0 || index >= fake.max_clients || index >= (int)(sizeof(fake.clients) / sizeof(fake.clients[0])))
        abort();
    *client = fake.clients[index];
    return true;
}
static int fake_connection(const aae_coop_game_t *g) {
    (void)g;
    return fake.connection;
}
static bool fake_server(const aae_coop_game_t *g, aae_coop_adr_t *adr, uint64_t *id) {
    (void)g;
    *adr = fake.server;
    *id = fake.server_id;
    return fake.has_server;
}
static int fake_local_sock(const aae_coop_game_t *g) {
    (void)g;
    return 0;
}
static uint64_t fake_nonce(const aae_coop_game_t *g) {
    (void)g;
    return fake.nonce++;
}
static void fake_lock(const aae_coop_game_t *g) {
    (void)g;
    if (fake.locks++ != 0)
        abort(); // taken twice: a deadlock in the game
}
static void fake_unlock(const aae_coop_game_t *g) {
    (void)g;
    if (--fake.locks != 0)
        abort();
}
static const aae_coop_game_t fake_game = { NULL, fake_now, fake_value, fake_set_dvar, fake_client, fake_connection,
                                           fake_server, fake_local_sock, fake_nonce, fake_lock, fake_unlock };

static aae_coop_adr_t adr_of(uint8_t which) {
    aae_coop_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    adr.bytes[0] = 192, adr.bytes[1] = 168, adr.bytes[2] = 1, adr.bytes[3] = (uint8_t)(10 + (which & 3));
    adr.bytes[4] = 0x69, adr.bytes[5] = 0x87;
    adr.bytes[8] = 3 + (which >> 2 & 1);
    return adr;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static aae_coop_t coop;
    aae_coop_reset(&coop);
    memset(&fake, 0, sizeof(fake));
    fake.now = 1000;
    fake.map = 1;
    fake.running = 1;
    fake.max_clients = 4;
    fake.pause_remote = 1;
    fake.nonce = 0x1234;
    fake.connection = 10;
    fake.server = adr_of(0);
    fake.server_id = 0x0110000100000002ull;
    fake.has_server = true;
    for (int i = 0; i < 4; i++) {
        fake.clients[i].state = 5;
        fake.clients[i].adr = adr_of((uint8_t)i);
        fake.clients[i].id = 0x0110000100000010ull + (uint64_t)i;
    }
    for (size_t at = 0; at < size;) {
        uint8_t kind = data[at++];
        switch (kind % 10) {
        case 0: { // a packet
            uint8_t from = at < size ? data[at++] : 0;
            uint32_t length = (uint32_t)(size - at < AAE_Coop_Packet_Size ? size - at : AAE_Coop_Packet_Size);
            // a copy of exactly its length, as the game's buffer would end there for the sanitizer
            uint8_t *packet = malloc(length ? length : 1);
            memcpy(packet, data + at, length);
            aae_coop_adr_t adr = adr_of(from);
            if (aae_coop_ours(packet, length))
                aae_coop_receive(&coop, &fake_game, &adr, from >> 4 & 7, packet, length);
            free(packet);
            at += length;
            break;
        }
        case 1: { // a server command, up to a newline
            const uint8_t *end = memchr(data + at, '\n', size - at);
            size_t length = end != NULL ? (size_t)(end - (data + at)) : size - at;
            char *text = malloc(length + 1);
            memcpy(text, data + at, length);
            text[length] = '\0';
            const char *given = aae_coop_server_command(&coop, &fake_game, text);
            if (given != text && strncmp(given, "q cl_paused", 11) != 0)
                abort();
            free(text);
            at += length + (end != NULL);
            break;
        }
        case 2: { // a client frame
            int msec = 16;
            float frac = 0.5f;
            aae_coop_frame_t frame;
            aae_coop_client_frame(&coop, &fake_game, at < size ? data[at++] & 1 : 0, &msec, &frac, &frame);
            aae_coop_client_frame_done(&coop, &fake_game, &frame);
            break;
        }
        case 3: aae_coop_server_frame(&coop, &fake_game, 50); break;
        case 4: aae_coop_check_paused(&coop, &fake_game); break;
        case 5: {
            aae_coop_outgoing_t out[AAE_Coop_Slots + 2];
            int count = aae_coop_send(&coop, &fake_game, out);
            if (count < 0 || count > AAE_Coop_Slots + 2)
                abort();
            for (int i = 0; i < count; i++) {
                aae_coop_adr_t from = out[i].to;
                if (!aae_coop_ours(out[i].data, AAE_Coop_Packet_Size))
                    abort();
                (void)from;
            }
            break;
        }
        case 6: aae_coop_can_pause(&coop, &fake_game); break;
        case 7: { // the game changes
            if (at + 2 > size)
                return 0;
            uint8_t what = data[at], value = data[at + 1];
            at += 2;
            switch (what % 8) {
            case 0: fake.map = value & 3; break;
            case 1: fake.running = (int8_t)value % 3; break;
            case 2: fake.cl_paused = (int8_t)value % 3; break;
            case 3: fake.max_clients = (int8_t)value % (AAE_Coop_Slots + 4); break;
            case 4: fake.pause_remote = (int8_t)value % 3; break;
            case 5: fake.connection = (int8_t)value % 13; break;
            case 6: {
                aae_coop_client_t *client = &fake.clients[value % (AAE_Coop_Slots + 4)];
                client->state = value >> 5;
                client->bot = value & 0x10;
                client->id = value & 8 ? 0 : client->id + 1;
                client->adr = adr_of(value);
                break;
            }
            case 7: fake.has_server = value & 1, fake.server = adr_of(value >> 1); break;
            }
            break;
        }
        case 8: fake.now += at < size ? (uint64_t)data[at++] * 37 : 1; break;
        case 9: fake.nonce = at < size ? data[at++] : 0; break;
        }
    }
    return 0;
}

void fuzz_seeds(void (*add)(const void *data, size_t size)) {
    uint8_t steps[256];
    size_t used = 0;
    // a host with clients: frames, a hello from a client, sending, an acknowledgement
    steps[used++] = 2, steps[used++] = 0;
    steps[used++] = 3;
    steps[used++] = 0, steps[used++] = 1;
    aae_coop_packet(steps + used, 1, 0, 0x55, 0, 0);
    used += AAE_Coop_Packet_Size;
    steps[used++] = 5;
    steps[used++] = 0, steps[used++] = 1;
    aae_coop_packet(steps + used, 3, 0x1234, 0x55, 2, 1);
    used += AAE_Coop_Packet_Size;
    steps[used++] = 7, steps[used++] = 2, steps[used++] = 1;
    steps[used++] = 3;
    steps[used++] = 4;
    add(steps, used);
    // a client: a state from its host, a server command
    used = 0;
    steps[used++] = 7, steps[used++] = 1, steps[used++] = 0;
    steps[used++] = 2, steps[used++] = 0;
    steps[used++] = 0, steps[used++] = 0;
    aae_coop_packet(steps + used, 2, 0x99, 0x1234, 3, 1);
    used += AAE_Coop_Packet_Size;
    steps[used++] = 2, steps[used++] = 0;
    steps[used++] = 1;
    memcpy(steps + used, "q @ 1 00ff\n", 11);
    used += 11;
    steps[used++] = 1;
    memcpy(steps + used, "q cl_paused \"0\"\n", 16);
    used += 16;
    add(steps, used);
}
void fuzz_dictionary(void (*add)(const void *data, size_t size)) {
    add("\xe1\0AAECP1", 8);
    add("\x01\x00\x00\x00", 4);
    add("q @ ", 4);
    add("q cl_paused ", 12);
}
