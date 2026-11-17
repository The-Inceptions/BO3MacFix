// aae_lan.c under the fuzzer (make fuzz): what another machine on the local network can send it, and the joiner's
// rules driven by any order of events. The parsing functions are called straight: nothing goes over a network.
//
// The input: its whole bytes as a datagram for the host's query reader and the joiner's reply reader, as a lobby
// packet, and as the text of a connect ("address@steamid"); then, read again as a list of steps, connects, pings,
// answers kept, asks, Joins planned and waited for, at times the input gives.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../source/aae_plugin.c"
#include "../../source/aae_lan.c"

// what aae_lan.c and aae_plugin.c ask of the rest of BO3MacFix
const char *t7o_mod_folder(void) {
    return "";
}
CSteamID steam_id(void) {
    return (CSteamID){ 0x0110000100000001ull };
}
bool prepatch_has_hook(uint32_t offset) {
    (void)offset;
    return false;
}
int DobbyHook(void *address, void *fake_func, void **out_origin_func) {
    (void)address, (void)fake_func, (void)out_origin_func;
    return 0;
}

static uint64_t read_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // the host: a datagram from anyone on the local network
    uint32_t number = 0;
    if (aae_lan_read_query(data, size, &number)) {
        uint8_t reply[AAE_LAN_PACKET];
        aae_lan_reply(reply, number, 0x0110000100000001ull);
        if (!aae_lan_read_reply(reply, sizeof(reply), 1, 2, 1, 2, number, &(uint64_t){ 0 }))
            abort(); // the host's own reply must read back
    }
    // the joiner: a datagram that may be the asked host's reply
    uint64_t id = 0;
    if (size >= 4) {
        uint32_t asked;
        memcpy(&asked, data, sizeof(asked));
        if (aae_lan_read_reply(data, size, asked, AAE_LAN_PORT, asked, AAE_LAN_PORT, read_u32(data + (size > 12 ? 8 : 0)),
                               &id) &&
            !aae_lan_is_individual(id))
            abort();
    }
    aae_lan_lobby_message(0x0110000100000001ull, data, (uint32_t)size);
    aae_lan_lobby_message(0x0110000100000001ull, size > 0 ? data : NULL, (uint32_t)size);

    // the connect's text: an address, maybe with "@" and a Steam ID
    char *text = malloc(size + 1);
    if (text == NULL)
        return 0;
    memcpy(text, data, size);
    text[size] = '\0';
    char address[AAE_LAN_ADDRESS];
    memset(address, 'x', sizeof(address));
    if (aae_lan_split(text, address, &id)) {
        if (memchr(address, '\0', sizeof(address)) == NULL || !aae_lan_is_individual(id) ||
            strncmp(address, text, strlen(address)) != 0)
            abort();
    }
    free(text);

    // the joiner's rules: each 13 bytes a step (its kind, an address, a time step)
    aae_lan_join_t join;
    memset(&join, 0, sizeof(join));
    aae_lan_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    uint64_t now = 1;
    for (size_t at = 0; at + 13 <= size; at += 13) {
        uint8_t kind = data[at];
        uint32_t where = read_u32(data + at + 1);
        uint64_t step = read_u64(data + at + 5);
        now += step % 200000;
        switch (kind % 8) {
        case 0: aae_lan_connect(&join, now); break;
        case 1: aae_lan_ping(&join, where, now); break;
        case 2: aae_lan_cache(&join, where, step & 1 ? 0x0110000100000002ull : 0, now); break;
        case 3: aae_lan_ask(&join, where, join.attempt, now); break;
        case 4: plan = aae_lan_plan(&join, now, (int)(step % 4), step, (int)(where % 6)); break;
        case 5: aae_lan_waited(&join, &plan, now, &id); break;
        case 6: aae_lan_dropped(&join, &plan); break;
        case 7: aae_lan_cached(&join, where, now, &id); break;
        }
        if (join.requested != 0 && !aae_lan_is_private(join.requested))
            abort(); // only an address of a local network is ever asked
    }
    return 0;
}

static void seed_packets(void (*add)(const void *data, size_t size)) {
    uint8_t packet[AAE_LAN_PACKET];
    aae_lan_query(packet, 0x12345678);
    add(packet, sizeof(packet));
    aae_lan_reply(packet, 0x12345678, 0x0110000100000001ull);
    add(packet, sizeof(packet));
    add("192.168.1.20:27017@76561197960265729", 36);
    add("10.0.0.2@0x0110000100000001", 27);
    static const uint8_t lobby[12] = { 0x28, 0, 0, 0, '1', 4, 0, 0, 1, 2, 3, 4 };
    add(lobby, sizeof(lobby));
}
void fuzz_seeds(void (*add)(const void *data, size_t size)) {
    seed_packets(add);
}
void fuzz_dictionary(void (*add)(const void *data, size_t size)) {
    add("AAELAN35", 8);
    add("@", 1);
    add("@0x", 3);
    add("76561197960265729", 17);
    add("\x01\x00\x10\x01", 4);
    add("\xc0\xa8", 2);
}
