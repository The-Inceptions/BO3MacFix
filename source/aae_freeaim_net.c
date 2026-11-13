// All-around Enhancement 4's free aim for players on other machines: the part of the plugin's free_aim component
// that carries where a player aims from their machine to the server.
//
// In the mod's third person view the plugin makes shots go to the point under the crosshair. The mod's script works
// that point out on the player's own machine and leaves it in a dvar (aae_freeaim_target). For a player on the
// machine that runs the server, the server reads the dvar (aae_freeaim.c). A player on another machine tells the
// server, with a reliable client command:
// - "aae X Y Z": X, Y and Z are the point's coordinates halved and rounded to whole numbers (so the server has the
//   point to 2 units), sent at most every 50 ms and only when they changed;
// - "aae off": sent once when free aim goes off after a point was sent.
// A client only sends while its machine runs no server, and only while fewer than 16 of its reliable commands are
// waiting for the server's acknowledgement (the game allows 128). The server takes "aae ..." commands away from the
// game, keeps one entry per player (the client_t the command came from, when, and the point doubled back), and the
// shot hooks look a player's point up there. Entries are cleared by "aae off", and every 500 ms when their client
// isn't at its place in svs.clients in state 5 (active) any more; there is no other time limit.
//
// Everything below is the plugin's own rule, read from its code (FUN_18001e5a0, FUN_18001e500, FUN_180020c10,
// FUN_180021250), so that a Mac player and Windows players with the real plugin understand each other whichever of
// them runs the server.
//
// Threads: commands are taken on the server's thread, entries are read by the shot hooks on the server's thread, and
// the sweep for clients that went away runs on the client's thread (in CL_FinishMove). As in the plugin, each entry
// has a sequence counter that is odd while it is being written: a writer waits for an even count and moves it on by
// one (compare and exchange), writes, and moves it on by one more; a reader takes a copy and keeps it only when the
// count was even and the same before and after (four tries).
#include <stdio.h>
#include <string.h>
#include "aae_freeaim_net.h"

#pragma STDC FP_CONTRACT OFF

#define Send_Every_Ms 50       // a client sends at most this often
#define Max_Unacknowledged 16  // and only while fewer of its reliable commands than this wait for the server
#define Sweep_Every_Ms 500     // how often the server looks for entries whose client went away
#define Read_Tries 4           // tries to read an entry that isn't being written meanwhile
#define Max_Command 128        // a command without its end within this many characters is left to the game
#define Most 0x800000          // the largest number a client may send, either way
#define Client_Active 5        // client_t's state (an int at +0) once the client is in the game
#define Local_Clients 2
#define Clc_Size 0x25780       // clientConnection_t
#define Clc_Sequence 0x130     // reliable commands sent
#define Clc_Acknowledged 0x134 // and acknowledged by the server

// ---- the client's side

// cvttss2si: the float cut to an int towards zero; what doesn't fit (and NaN) becomes 0x80000000
static int32_t cut_like_x86(float value) {
    if (!(value > -2147483904.0f && value < 2147483648.0f))
        return INT32_MIN;
    return (int32_t)value;
}

int32_t aae_freeaim_net_half(float coordinate) {
    float half = coordinate * 0.5f;
    // (for NaN the plugin goes the way of a negative number; NaN stays NaN either way)
    if (half < 0.0f)
        half = half - 0.5f;
    else
        half = half + 0.5f;
    return cut_like_x86(half);
}

bool aae_freeaim_net_send(aae_freeaim_net_sender_t *sender, uint64_t now, bool no_server, bool aiming,
                          const float target[3], bool have_counters, uint32_t sequence, uint32_t acknowledged,
                          char text[AAE_FREEAIM_NET_TEXT]) {
    int32_t numbers[3] = { 0, 0, 0 };
    bool has = no_server && aiming;
    if (has)
        for (int i = 0; i < 3; i++)
            numbers[i] = aae_freeaim_net_half(target[i]);
    bool changed = !sender->sent_target || numbers[0] != sender->sent[0] || numbers[1] != sender->sent[1] ||
                   numbers[2] != sender->sent[2];
    bool send_target = has && changed;
    bool send_off = no_server && !has && sender->sent_target;
    if (!send_target && !send_off)
        return false;
    if ((int64_t)(now - sender->sent_at) < Send_Every_Ms)
        return false;
    if (!have_counters || sequence - acknowledged >= Max_Unacknowledged)
        return false;
    sender->sent_at = now;
    if (send_off) {
        snprintf(text, AAE_FREEAIM_NET_TEXT, "aae off");
        sender->sent_target = false;
        return true;
    }
    snprintf(text, AAE_FREEAIM_NET_TEXT, "aae %d %d %d", numbers[0], numbers[1], numbers[2]);
    memcpy(sender->sent, numbers, sizeof(numbers));
    sender->sent_target = true;
    return true;
}

// ---- the server's side: reading a command

// a char as the plugin's compiler has it: with a sign
typedef signed char plugin_char;
static bool is_space(plugin_char c) {
    return c == ' ' || (unsigned char)(c - 9) < 5;
}

// a whole number: a sign or none, digits (as many as fit: at most 0x800000 either way)
static bool read_number(const plugin_char **text, int32_t *value) {
    const plugin_char *p = *text;
    plugin_char c = *p;
    bool negative = false, any = false;
    uint32_t number = 0;
    if (c == '+' || c == '-') {
        negative = c == '-';
        c = *++p;
    }
    if (c < '0')
        return false;
    for (;;) {
        c = *p;
        if (c > '9') {
            if (!any)
                return false;
            break;
        }
        if (number > Most / 10 || (number == Most / 10 && (uint32_t)(c - '0') > Most % 10))
            return false;
        number = number * 10 + (uint32_t)(c - '0');
        p++;
        any = true;
        if (*p < '0')
            break;
    }
    if (!negative && number > Most)
        return false;
    *value = negative ? -(int32_t)number : (int32_t)number;
    *text = p;
    return true;
}

int aae_freeaim_net_read(const char *text, float target[3]) {
    if (text == NULL || strncmp(text, "aae ", 4) != 0 || strnlen(text, Max_Command) >= Max_Command)
        return AAE_FREEAIM_NET_NOT_OURS;
    if (strcmp(text + 4, "off") == 0)
        return AAE_FREEAIM_NET_OFF;
    const plugin_char *p = (const plugin_char *)text + 4;
    float numbers[3];
    for (int i = 0; i < 3; i++) {
        int32_t number;
        while (is_space(*p))
            p++;
        if (!read_number(&p, &number))
            return AAE_FREEAIM_NET_BAD;
        if (i < 2 && !is_space(*p))
            return AAE_FREEAIM_NET_BAD;
        numbers[i] = (float)number + (float)number;
    }
    while (is_space(*p))
        p++;
    if (*p != '\0')
        return AAE_FREEAIM_NET_BAD;
    memcpy(target, numbers, sizeof(numbers));
    return AAE_FREEAIM_NET_TARGET;
}

static bool is_zombies(const char *map) {
    return map != NULL && strncmp(map, "zm_", 3) == 0;
}

int aae_freeaim_net_slot(const uint8_t *client, const uint8_t *clients, size_t client_size) {
    if (client == NULL || clients == NULL || client < clients)
        return -1;
    uintptr_t offset = (uintptr_t)client - (uintptr_t)clients;
    if (offset % client_size != 0 || offset / client_size >= AAE_FREEAIM_NET_PLAYERS)
        return -1;
    return (int)(offset / client_size);
}

static int32_t client_state(const uint8_t *client) {
    int32_t state;
    memcpy(&state, client, sizeof(state));
    return state;
}

// ---- the table

// how a reader loads an entry's sequence counter (before: ahead of the copy, or after it); tests can look in here
#ifndef AAE_FREEAIM_NET_SEQUENCE
#define AAE_FREEAIM_NET_SEQUENCE(entry, order, before) atomic_load_explicit(&(entry)->sequence, order)
#endif

static uint32_t write_begin(aae_freeaim_net_entry_t *entry) {
    for (;;) {
        uint32_t count = atomic_load_explicit(&entry->sequence, memory_order_relaxed);
        if (count & 1)
            continue;
        if (atomic_compare_exchange_strong_explicit(&entry->sequence, &count, count + 1, memory_order_acquire,
                                                    memory_order_relaxed)) {
            atomic_thread_fence(memory_order_release);
            return count;
        }
    }
}
static void write_end(aae_freeaim_net_entry_t *entry, uint32_t count) {
    atomic_store_explicit(&entry->sequence, count + 2, memory_order_release);
}
static void put(aae_freeaim_net_entry_t *entry, uintptr_t client, uint64_t received_at, uint32_t valid,
                const uint32_t target[3]) {
    atomic_store_explicit(&entry->client, client, memory_order_relaxed);
    atomic_store_explicit(&entry->received_at, received_at, memory_order_relaxed);
    atomic_store_explicit(&entry->valid, valid, memory_order_relaxed);
    for (int i = 0; i < 3; i++)
        atomic_store_explicit(&entry->target[i], target[i], memory_order_relaxed);
}
static const uint32_t nothing[3];

static void clear(aae_freeaim_net_table_t *table, int slot) {
    aae_freeaim_net_entry_t *entry = &table->entries[slot];
    uint32_t count = write_begin(entry);
    put(entry, 0, 0, 0, nothing);
    atomic_fetch_and(&table->in_use, ~((uint64_t)1 << slot));
    write_end(entry, count);
}

static void store(aae_freeaim_net_table_t *table, int slot, const uint8_t *client, uint64_t now,
                  const float target[3]) {
    aae_freeaim_net_entry_t *entry = &table->entries[slot];
    uint32_t bits[3];
    memcpy(bits, target, sizeof(bits));
    uint32_t count = write_begin(entry);
    uint32_t was = atomic_load_explicit(&entry->valid, memory_order_relaxed);
    put(entry, (uintptr_t)client, now, 1, bits);
    if (was == 0)
        atomic_fetch_or(&table->in_use, (uint64_t)1 << slot);
    write_end(entry, count);
}

bool aae_freeaim_net_copy(aae_freeaim_net_table_t *table, int slot, aae_freeaim_net_copy_t *copy) {
    aae_freeaim_net_entry_t *entry = &table->entries[slot];
    for (int i = 0; i < Read_Tries; i++) {
        uint32_t before = AAE_FREEAIM_NET_SEQUENCE(entry, memory_order_acquire, true);
        if (before & 1)
            continue;
        copy->client = atomic_load_explicit(&entry->client, memory_order_relaxed);
        copy->received_at = atomic_load_explicit(&entry->received_at, memory_order_relaxed);
        copy->valid = atomic_load_explicit(&entry->valid, memory_order_relaxed);
        for (int j = 0; j < 3; j++) {
            uint32_t bits = atomic_load_explicit(&entry->target[j], memory_order_relaxed);
            memcpy(&copy->target[j], &bits, sizeof(bits));
        }
        atomic_thread_fence(memory_order_acquire);
        uint32_t after = AAE_FREEAIM_NET_SEQUENCE(entry, memory_order_relaxed, false);
        if (before == after)
            return true;
    }
    return false;
}

bool aae_freeaim_net_command(aae_freeaim_net_table_t *table, const char *text, const uint8_t *client,
                             int from_old_server, const char *map, const uint8_t *clients, size_t client_size,
                             uint64_t now) {
    float target[3];
    int kind = aae_freeaim_net_read(text, target);
    if (kind == AAE_FREEAIM_NET_NOT_OURS)
        return false;
    if (!is_zombies(map) || from_old_server != 0 || client == NULL || client_state(client) != Client_Active)
        return true;
    int slot = aae_freeaim_net_slot(client, clients, client_size);
    if (slot < 0)
        return true;
    if (kind == AAE_FREEAIM_NET_OFF)
        clear(table, slot);
    else if (kind == AAE_FREEAIM_NET_TARGET)
        store(table, slot, client, now, target);
    return true;
}

void aae_freeaim_net_sweep(aae_freeaim_net_table_t *table, uint64_t now, const uint8_t *clients,
                           size_t client_size) {
    uint64_t left = atomic_load(&table->in_use);
    if (left == 0)
        return;
    if (table->swept_at != 0 && (int64_t)(now - table->swept_at) < Sweep_Every_Ms)
        return;
    table->swept_at = now;
    if (clients == NULL)
        return;
    while (left != 0) {
        int slot = __builtin_ctzll(left);
        left &= left - 1;
        aae_freeaim_net_copy_t seen;
        if (!aae_freeaim_net_copy(table, slot, &seen) || seen.valid == 0)
            continue;
        uintptr_t place = (uintptr_t)clients + (uintptr_t)slot * client_size;
        if (seen.client == place && place != 0 && client_state((const uint8_t *)place) == Client_Active)
            continue;
        // gone: cleared, unless the entry was written again meanwhile
        aae_freeaim_net_entry_t *entry = &table->entries[slot];
        uint32_t count = write_begin(entry);
        if (atomic_load_explicit(&entry->valid, memory_order_relaxed) != 0 &&
            atomic_load_explicit(&entry->client, memory_order_relaxed) == seen.client &&
            atomic_load_explicit(&entry->received_at, memory_order_relaxed) == seen.received_at) {
            put(entry, 0, 0, 0, nothing);
            atomic_fetch_and(&table->in_use, ~((uint64_t)1 << slot));
        }
        write_end(entry, count);
    }
}

bool aae_freeaim_net_has_entry(aae_freeaim_net_table_t *table, uint32_t player) {
    aae_freeaim_net_copy_t seen;
    return player < AAE_FREEAIM_NET_PLAYERS && aae_freeaim_net_copy(table, (int)player, &seen) && seen.valid != 0 &&
           seen.client != 0;
}

bool aae_freeaim_net_lookup(aae_freeaim_net_table_t *table, uint32_t player, const char *map, bool active,
                            const uint8_t *clients, size_t client_size, float target[3]) {
    aae_freeaim_net_copy_t seen;
    if (!is_zombies(map) || player >= AAE_FREEAIM_NET_PLAYERS || !aae_freeaim_net_copy(table, (int)player, &seen))
        return false;
    if (seen.valid == 0 || seen.received_at == 0 || !active || clients == NULL)
        return false;
    uintptr_t place = (uintptr_t)clients + (uintptr_t)player * client_size;
    if (seen.client != place || place == 0 || client_state((const uint8_t *)place) != Client_Active)
        return false;
    memcpy(target, seen.target, sizeof(seen.target));
    return true;
}

// ---- the game's side

static aae_freeaim_net_game_t game;
static bool ready;
static aae_freeaim_net_table_t table;
static aae_freeaim_net_sender_t senders[Local_Clients]; // only the client's thread uses these

void aae_freeaim_net_init(const aae_freeaim_net_game_t *with) {
    game = *with;
    memset(senders, 0, sizeof(senders));
    for (int i = 0; i < AAE_FREEAIM_NET_PLAYERS; i++) {
        aae_freeaim_net_entry_t *entry = &table.entries[i];
        atomic_store(&entry->sequence, 0);
        put(entry, 0, 0, 0, nothing);
    }
    atomic_store(&table.in_use, 0);
    table.swept_at = 0;
    ready = true;
}

aae_freeaim_net_table_t *aae_freeaim_net_table(void) {
    return &table;
}
aae_freeaim_net_sender_t *aae_freeaim_net_sender(int local_client) {
    return local_client >= 0 && local_client < Local_Clients ? &senders[local_client] : NULL;
}

void aae_freeaim_net_client_frame(int local_client) {
    if (!ready || !game.active() || local_client < 0 || local_client >= Local_Clients)
        return;
    uint64_t now = game.now_ms();
    aae_freeaim_net_sweep(&table, now, *game.server_clients, game.client_size);
    bool no_server = *game.server_state == 0;
    bool on = game.on(local_client, now);
    float target[3] = { 0.0f, 0.0f, 0.0f };
    bool have_target = game.local_target(local_client, target);
    const uint8_t *connection = *game.client_connections;
    uint32_t sequence = 0, acknowledged = 0;
    if (connection != NULL) {
        connection += (size_t)local_client * Clc_Size;
        memcpy(&sequence, connection + Clc_Sequence, sizeof(sequence));
        memcpy(&acknowledged, connection + Clc_Acknowledged, sizeof(acknowledged));
    }
    char text[AAE_FREEAIM_NET_TEXT];
    if (aae_freeaim_net_send(&senders[local_client], now, no_server, on && have_target, target, connection != NULL,
                             sequence, acknowledged, text))
        game.add_reliable_command(local_client, text);
}

bool aae_freeaim_net_server_command(const uint8_t *client, const char *text, int from_old_server) {
    if (!ready || text == NULL || strncmp(text, "aae ", 4) != 0 || !game.active())
        return false;
    return aae_freeaim_net_command(&table, text, client, from_old_server, game.map(), *game.server_clients,
                                   game.client_size, game.now_ms());
}

bool aae_freeaim_net_remote_target(uint32_t player, float target[3]) {
    if (!ready)
        return false;
    return aae_freeaim_net_lookup(&table, player, game.map(), game.active(), *game.server_clients, game.client_size,
                                  target);
}

bool aae_freeaim_net_remote_entry(uint32_t player) {
    return ready && game.active() && aae_freeaim_net_has_entry(&table, player);
}
