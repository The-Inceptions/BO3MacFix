// BO3MacFix's side of freeaim_net_cases.h: aae_freeaim_net.c through its three entry points, with the made-up game
// of freeaim_net_cases.h. Included by test_aae_freeaim_net.c (which checks the plugin's output by its line count and
// checksum) and by the program that compares with the plugin's own code. Include aae_freeaim_net.c and
// freeaim_net_cases.h first.
static bool fake_active(void) {
    return game_active != 0;
}
static uint64_t fake_now(void) {
    return game_now;
}
static const char *fake_map(void) {
    return game_map;
}
static bool fake_on(int local_client, uint64_t now) {
    (void)now;
    return game_on[local_client] != 0;
}
static bool fake_target(int local_client, float target[3]) {
    if (!game_has[local_client])
        return false;
    memcpy(target, game_target[local_client], 3 * sizeof(float));
    return true;
}

static void side_reset(void) {
    aae_freeaim_net_game_t with = {
        .active = fake_active,
        .now_ms = fake_now,
        .map = fake_map,
        .on = fake_on,
        .local_target = fake_target,
        .add_reliable_command = game_add_reliable,
        .server_state = &game_sv_state,
        .client_connections = &game_clc,
        .server_clients = &game_clients,
        .client_size = CLIENT_SIZE,
    };
    aae_freeaim_net_init(&with);
}
static bool side_command(const uint8_t *client, const char *text, int from_old_server) {
    return aae_freeaim_net_server_command(client, text, from_old_server);
}
static void side_frame(int local_client) {
    aae_freeaim_net_client_frame(local_client);
}
static bool side_lookup(uint32_t player, float target[3]) {
    return aae_freeaim_net_remote_target(player, target);
}
static void side_entry(int slot, uint32_t *sequence, uintptr_t *client, uint64_t *tick, uint32_t *valid,
                       uint32_t target[3]) {
    aae_freeaim_net_entry_t *entry = &aae_freeaim_net_table()->entries[slot];
    *sequence = atomic_load(&entry->sequence);
    *client = atomic_load(&entry->client);
    *tick = atomic_load(&entry->received_at);
    *valid = atomic_load(&entry->valid);
    for (int i = 0; i < 3; i++)
        target[i] = atomic_load(&entry->target[i]);
}
static uint64_t side_in_use(void) {
    return atomic_load(&aae_freeaim_net_table()->in_use);
}
static uint64_t side_swept_at(void) {
    return aae_freeaim_net_table()->swept_at;
}
static void side_sender(int local_client, uint64_t *sent_at, int32_t sent[3], int *sent_target) {
    aae_freeaim_net_sender_t *sender = aae_freeaim_net_sender(local_client);
    *sent_at = sender->sent_at;
    memcpy(sent, sender->sent, 3 * sizeof(int32_t));
    *sent_target = sender->sent_target;
}

