// aae_freeaim_net.c and the text half of aae_freeaim.c under the fuzzer (make fuzz): the "aae X Y Z" client command
// another player's game sends a host, the table the host keeps of it, and the point a mod's script leaves in the
// aae_freeaim_target dvar. The parsing functions are called straight: nothing goes over a network.
//
// The input: its first byte picks a slot, the rest is the command's text (cut at the first NUL), which goes to the
// command's reader, the server's parser with a made-up svs.clients, and the dvar's reader; then the table is looked
// up, swept and read for every player.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// (aae_freeaim_net.c and gsc_obj.c are built beside it: see FUZZ_SOURCES_freeaim_net in the Makefile)
#include "../../source/aae_freeaim.c"
#include "../../source/aae_plugin.c"

// what the included files ask of the rest of BO3MacFix
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

#define Clients 8
#define Made_Up_Client_Size 64 // the game's is 0xe5270; the table only compares addresses and reads the state at +0

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static aae_freeaim_net_table_t table;
    static uint8_t clients[Clients * Made_Up_Client_Size];
    memset(&table, 0, sizeof(table));
    memset(clients, 0, sizeof(clients));
    for (int i = 0; i < Clients; i++)
        clients[i * Made_Up_Client_Size] = 5; // in the game
    if (size == 0)
        return 0;
    uint8_t slot = data[0];
    char *text = malloc(size);
    memcpy(text, data + 1, size - 1);
    text[size - 1] = '\0';

    float target[3];
    int kind = aae_freeaim_net_read(text, target);
    if (kind == AAE_FREEAIM_NET_TARGET)
        for (int i = 0; i < 3; i++)
            if (!(target[i] >= -16777216.0f && target[i] <= 16777216.0f))
                abort(); // twice a number of at most 0x800000 either way
    // the command from one of the clients, or from an address that isn't one
    const uint8_t *client = slot < Clients ? clients + (size_t)slot * Made_Up_Client_Size
                            : slot < 200    ? clients + slot % (Clients * Made_Up_Client_Size)
                                            : NULL;
    bool ours = aae_freeaim_net_command(&table, text, client, slot & 0x80, slot & 0x40 ? "mp_x" : "zm_theater", clients,
                                        Made_Up_Client_Size, 1000);
    if (ours != (kind != AAE_FREEAIM_NET_NOT_OURS))
        abort();
    for (uint32_t player = 0; player < AAE_FREEAIM_NET_PLAYERS + 2; player++) {
        float point[3];
        aae_freeaim_net_has_entry(&table, player);
        aae_freeaim_net_lookup(&table, player, "zm_theater", true, clients, Made_Up_Client_Size, point);
    }
    clients[(slot % Clients) * Made_Up_Client_Size] = 2; // that client goes away
    aae_freeaim_net_sweep(&table, 2000, clients, Made_Up_Client_Size);
    if (atomic_load(&table.in_use) >> Clients != 0)
        abort(); // an entry for a player that isn't in svs.clients

    // the same text as the mod's point in the dvar
    float point[3];
    if (aae_freeaim_target(text, point))
        for (int i = 0; i < 3; i++)
            if (!(point[i] >= -16777216.0f && point[i] <= 16777216.0f))
                abort();
    const char *rest = text;
    float number;
    aae_freeaim_number(&rest, &number);
    if (rest < text || rest > text + strlen(text))
        abort();
    free(text);
    return 0;
}

void fuzz_seeds(void (*add)(const void *data, size_t size)) {
    add("\x01" "aae 120 -40 31", 15);
    add("\x02" "aae off", 8);
    add("\x03" "aae 8388608 -8388608 0", 23);
    add("\x00" "1.5e3 -20 7.25", 15);
    add("\x00" "  -0.5   2e-3  +9 ", 18);
}
void fuzz_dictionary(void (*add)(const void *data, size_t size)) {
    add("aae ", 4);
    add("off", 3);
    add("8388608", 7);
    add("e38", 3);
    add("e-39", 4);
    add(".", 1);
}
