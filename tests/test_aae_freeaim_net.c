// aae_freeaim_net.c: free aim for players on other machines (what a client sends, what the server keeps, the lookup
// for the shot hooks) against a made-up game; and the same made-up cases as the plugin's own code got under Wine
// (see freeaim_net_cases.h), checked by line count and checksum.
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// a look into the readers' loads of an entry's sequence counter (see test_meanwhile)
#include <stdbool.h>
#include <stdint.h>
static uint32_t test_sequence(void *entry, int order, bool before);
#define AAE_FREEAIM_NET_SEQUENCE(entry, order, before) test_sequence(entry, order, before)

#define CLIENT_SIZE 0xe5270 // the Mac's client_t
#include "../source/aae_freeaim_net.c"
#include "freeaim_net_cases.h"
#include "freeaim_net_side.h"

static float from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static uint32_t bits_of(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// ---- what is sent

static void test_half(void) {
    static const struct {
        float coordinate;
        int32_t sent;
    } cases[] = {
        { 0.0f, 0 },          { -0.0f, 0 },          { 1.0f, 1 },          { -1.0f, -1 },       { 2.0f, 1 },
        { 3.0f, 2 },          { -3.0f, -2 },         { 0.9f, 0 },          { -0.9f, 0 },        { 1.5f, 1 },
        { 4.0f, 2 },          { 5.0f, 3 },           { -5.0f, -3 },        { 123.4f, 62 },      { -123.4f, -62 },
        // in floats: 0.49999997 + 0.5 is 1 (to even), where the exact sum would be cut to 0
        { 0.99999994f, 1 },   { -0.99999994f, -1 },
        { 16777216.0f, 8388608 }, { -16777216.0f, -8388608 }, { 16777214.0f, 8388607 }, { 16777215.0f, 8388608 },
        // and what doesn't fit an int: what cvttss2si gives
        { 4294967040.0f, 2147483520 }, { 4294967296.0f, INT32_MIN }, { -4294967296.0f, INT32_MIN },
        { -4294967040.0f, -2147483520 }, { 1e30f, INT32_MIN }, { -1e30f, INT32_MIN },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int32_t sent = aae_freeaim_net_half(cases[i].coordinate);
        if (sent != cases[i].sent)
            printf("  half(%.9g) = %d, not %d\n", (double)cases[i].coordinate, sent, cases[i].sent);
        CHECK(sent == cases[i].sent);
    }
    CHECK(aae_freeaim_net_half(from_bits(0x7fc00000)) == INT32_MIN);
    CHECK(aae_freeaim_net_half(from_bits(0xffc00000)) == INT32_MIN);
    CHECK(aae_freeaim_net_half(INFINITY) == INT32_MIN);
    CHECK(aae_freeaim_net_half(-INFINITY) == INT32_MIN);
    CHECK(cut_like_x86(2147483520.0f) == 2147483520);
    CHECK(cut_like_x86(-2147483648.0f) == INT32_MIN);
    CHECK(cut_like_x86(2147483648.0f) == INT32_MIN);
    CHECK(cut_like_x86(-2147483904.0f) == INT32_MIN);
    CHECK(cut_like_x86(-1.9f) == -1);
}

static void test_send(void) {
    aae_freeaim_net_sender_t sender = { 0 };
    char text[AAE_FREEAIM_NET_TEXT] = "";
    float target[3] = { 10.0f, -21.0f, 3.2f };
    uint64_t now = 100000;
    // nothing sent yet, free aim off: nothing to say (not even "off")
    CHECK(!aae_freeaim_net_send(&sender, now, true, false, target, true, 5, 5, text));
    // on: "aae X Y Z" with the halves
    CHECK(aae_freeaim_net_send(&sender, now, true, true, target, true, 5, 5, text));
    CHECK(strcmp(text, "aae 5 -11 2") == 0);
    CHECK(sender.sent_at == now && sender.sent_target && sender.sent[0] == 5 && sender.sent[1] == -11 &&
          sender.sent[2] == 2);
    // the same numbers again: nothing, however long after
    CHECK(!aae_freeaim_net_send(&sender, now + 1000, true, true, target, true, 5, 5, text));
    target[2] = 3.4f; // still 2
    CHECK(!aae_freeaim_net_send(&sender, now + 1000, true, true, target, true, 5, 5, text));
    // other numbers: not before 50 ms
    target[0] = 12.0f;
    CHECK(!aae_freeaim_net_send(&sender, now + 49, true, true, target, true, 5, 5, text));
    CHECK(aae_freeaim_net_send(&sender, now + 50, true, true, target, true, 5, 5, text));
    CHECK(strcmp(text, "aae 6 -11 2") == 0);
    now += 50;
    // each coordinate counts
    for (int i = 0; i < 3; i++) {
        now += 50;
        target[i] += 10.0f;
        CHECK(aae_freeaim_net_send(&sender, now, true, true, target, true, 5, 5, text));
    }
    CHECK(strcmp(text, "aae 11 -6 7") == 0);
    // a clock that went back: nothing until it is 50 ms past the last time again
    target[0] = 500.0f;
    CHECK(!aae_freeaim_net_send(&sender, now - 1, true, true, target, true, 5, 5, text));
    CHECK(!aae_freeaim_net_send(&sender, now - 100000, true, true, target, true, 5, 5, text));
    // the client's reliable commands: fewer than 16 waiting
    now += 50;
    CHECK(!aae_freeaim_net_send(&sender, now, true, true, target, true, 116, 100, text));
    CHECK(!aae_freeaim_net_send(&sender, now, true, true, target, true, 100, 101, text)); // acknowledged beyond: wraps
    CHECK(!aae_freeaim_net_send(&sender, now, true, true, target, false, 5, 5, text));     // no connection
    CHECK(aae_freeaim_net_send(&sender, now, true, true, target, true, 115, 100, text));
    CHECK(strcmp(text, "aae 250 -6 7") == 0);
    // counters about the wrap of 32 bits
    target[0] = 502.0f, now += 50;
    CHECK(aae_freeaim_net_send(&sender, now, true, true, target, true, 3, 0xfffffff8u, text));
    // this machine runs a server: nothing at all, not even "off"
    target[0] = 504.0f, now += 50;
    CHECK(!aae_freeaim_net_send(&sender, now, false, true, target, true, 5, 5, text));
    CHECK(!aae_freeaim_net_send(&sender, now, false, false, target, true, 5, 5, text));
    // free aim goes off: "aae off" once (with the same limits), the numbers kept
    CHECK(!aae_freeaim_net_send(&sender, now, true, false, target, true, 21, 5, text));
    CHECK(aae_freeaim_net_send(&sender, now, true, false, target, true, 5, 5, text));
    CHECK(strcmp(text, "aae off") == 0);
    CHECK(!sender.sent_target && sender.sent_at == now && sender.sent[0] == 251);
    CHECK(!aae_freeaim_net_send(&sender, now + 1000, true, false, target, true, 5, 5, text));
    // on again with the numbers sent before "off": sent again, as they count as not sent
    target[0] = 502.0f;
    CHECK(!aae_freeaim_net_send(&sender, now + 49, true, true, target, true, 5, 5, text));
    CHECK(aae_freeaim_net_send(&sender, now + 50, true, true, target, true, 5, 5, text));
    CHECK(strcmp(text, "aae 251 -6 7") == 0);
    // off before 50 ms have gone by: later
    now += 50;
    CHECK(!aae_freeaim_net_send(&sender, now + 10, true, false, target, true, 5, 5, text));
    CHECK(aae_freeaim_net_send(&sender, now + 50, true, false, target, true, 5, 5, text));
    // the largest numbers and the plugin's NaN
    aae_freeaim_net_sender_t fresh = { 0 };
    float far[3] = { -16777216.0f, from_bits(0x7fc00000), 1e30f };
    // (never sent counts as sent at time 0: with a clock under 50 ms nothing goes yet)
    CHECK(!aae_freeaim_net_send(&fresh, 49, true, true, far, true, 0, 0, text));
    CHECK(aae_freeaim_net_send(&fresh, 50, true, true, far, true, 0, 0, text));
    CHECK(strcmp(text, "aae -8388608 -2147483648 -2147483648") == 0);
}

// ---- what the server reads

static void test_read(void) {
    static const struct {
        const char *text;
        int kind;
        float target[3];
    } cases[] = {
        { "aae 1 2 3", AAE_FREEAIM_NET_TARGET, { 2, 4, 6 } },
        { "aae -1 +2 -0", AAE_FREEAIM_NET_TARGET, { -2, 4, 0 } },
        { "aae \t 1\n2\v3 \f\r", AAE_FREEAIM_NET_TARGET, { 2, 4, 6 } },
        { "aae 8388608 -8388608 0", AAE_FREEAIM_NET_TARGET, { 16777216, -16777216, 0 } },
        { "aae 0008388608 1 1", AAE_FREEAIM_NET_TARGET, { 16777216, 2, 2 } },
        { "aae 8388609 1 1", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae -8388609 1 1", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1 8388609 1", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1 1 -8388609", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 8388610 1 1", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 8388700 1 1", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae -83886080 1 1", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1 1 -9999999999", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 99999999999 1 1", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1 2", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1 2 3 4", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1,2 3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1 2,3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1 2 3,", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1.0 2 3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1: 2 3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae :1 2 3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae - 1 2 3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae -1 - 2 3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1 2 -", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae +-1 2 3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1\x80 2 3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae \x80", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1\x08 2 3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae 1\x0e 2 3", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae ", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae off ", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae  off", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae offf", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae of", AAE_FREEAIM_NET_BAD, { 0, 0, 0 } },
        { "aae off", AAE_FREEAIM_NET_OFF, { 0, 0, 0 } },
        { "aae", AAE_FREEAIM_NET_NOT_OURS, { 0, 0, 0 } },
        { "aaeoff", AAE_FREEAIM_NET_NOT_OURS, { 0, 0, 0 } },
        { "aae\t1 2 3", AAE_FREEAIM_NET_NOT_OURS, { 0, 0, 0 } },
        { "AAE 1 2 3", AAE_FREEAIM_NET_NOT_OURS, { 0, 0, 0 } },
        { "", AAE_FREEAIM_NET_NOT_OURS, { 0, 0, 0 } },
        { NULL, AAE_FREEAIM_NET_NOT_OURS, { 0, 0, 0 } },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float target[3] = { 9, 9, 9 };
        int kind = aae_freeaim_net_read(cases[i].text, target);
        if (kind != cases[i].kind)
            printf("  read(\"%s\") = %d, not %d\n", cases[i].text ? cases[i].text : "(null)", kind, cases[i].kind);
        CHECK(kind == cases[i].kind);
        if (kind == AAE_FREEAIM_NET_TARGET)
            CHECK(memcmp(target, cases[i].target, sizeof(target)) == 0);
        else
            CHECK(target[0] == 9 && target[1] == 9 && target[2] == 9);
    }
    // 127 characters are read, 128 without an end are the game's
    char text[200];
    memset(text, ' ', sizeof(text));
    memcpy(text, "aae 1 2 3", 9);
    text[127] = '\0';
    float target[3];
    CHECK(aae_freeaim_net_read(text, target) == AAE_FREEAIM_NET_TARGET);
    text[127] = ' ', text[128] = '\0';
    CHECK(aae_freeaim_net_read(text, target) == AAE_FREEAIM_NET_NOT_OURS);
}

// ---- the server's table

static uint8_t *test_clients;
static uint8_t *client_at(int slot) {
    return test_clients + (ptrdiff_t)slot * CLIENT_SIZE;
}
static void set_state(int slot, int32_t state) {
    memcpy(client_at(slot), &state, sizeof(state));
}

static void test_slot(void) {
    CHECK(aae_freeaim_net_slot(client_at(0), test_clients, CLIENT_SIZE) == 0);
    CHECK(aae_freeaim_net_slot(client_at(63), test_clients, CLIENT_SIZE) == 63);
    CHECK(aae_freeaim_net_slot(client_at(64), test_clients, CLIENT_SIZE) == -1);
    CHECK(aae_freeaim_net_slot(client_at(2) + 1, test_clients, CLIENT_SIZE) == -1);
    CHECK(aae_freeaim_net_slot(client_at(2) - 1, test_clients, CLIENT_SIZE) == -1);
    CHECK(aae_freeaim_net_slot(client_at(5), client_at(1), CLIENT_SIZE) == 4);
    CHECK(aae_freeaim_net_slot(client_at(0), client_at(1), CLIENT_SIZE) == -1);
    CHECK(aae_freeaim_net_slot(client_at(0), NULL, CLIENT_SIZE) == -1);
    CHECK(aae_freeaim_net_slot(NULL, test_clients, CLIENT_SIZE) == -1);
    CHECK(aae_freeaim_net_slot(client_at(3), test_clients, 0xe5170) == -1); // the Windows game's size doesn't fit
}

static aae_freeaim_net_table_t test_table;
static void table_empty(void) {
    memset(&test_table, 0, sizeof(test_table));
}
static bool entry_is(int slot, uint32_t sequence, const uint8_t *client, uint64_t at, uint32_t valid, float x,
                     float y, float z) {
    aae_freeaim_net_entry_t *entry = &test_table.entries[slot];
    return atomic_load(&entry->sequence) == sequence && atomic_load(&entry->client) == (uintptr_t)client &&
           atomic_load(&entry->received_at) == at && atomic_load(&entry->valid) == valid &&
           atomic_load(&entry->target[0]) == bits_of(x) && atomic_load(&entry->target[1]) == bits_of(y) &&
           atomic_load(&entry->target[2]) == bits_of(z);
}
static bool command(const char *text, int slot, int old, const char *map, uint64_t now) {
    return aae_freeaim_net_command(&test_table, text, slot < -100 ? NULL : client_at(slot), old, map, test_clients,
                                   CLIENT_SIZE, now);
}

static void test_command(void) {
    table_empty();
    for (int slot = -1; slot < 66; slot++)
        set_state(slot, 5);
    // not ours: the game's
    CHECK(!command("say hi", 3, 0, "zm_x", 10));
    CHECK(!command(NULL, 3, 0, "zm_x", 10));
    CHECK(!command("aaex", 3, 0, "zm_x", 10));
    // stored: the client, when, valid, the numbers doubled
    CHECK(command("aae 1 -2 3", 3, 0, "zm_x", 10));
    CHECK(entry_is(3, 2, client_at(3), 10, 1, 2, -4, 6));
    CHECK(atomic_load(&test_table.in_use) == 1u << 3);
    CHECK(command("aae 4 5 6", 3, 0, "zm_x", 20));
    CHECK(entry_is(3, 4, client_at(3), 20, 1, 8, 10, 12));
    CHECK(command("aae 7 7 7", 63, 0, "zm_x", 30));
    CHECK(atomic_load(&test_table.in_use) == ((1ull << 63) | (1u << 3)));
    // nothing changes for anything not understood, but it's taken from the game
    CHECK(command("aae 1 2", 3, 0, "zm_x", 40));
    CHECK(entry_is(3, 4, client_at(3), 20, 1, 8, 10, 12));
    // nor for a server that isn't on a zombies map, a command from an old server, no client, a client not in the
    // game, one not in svs.clients
    static const char *const maps[] = { NULL, "", "zm", "mp_zm_", "ZM_x", "zm-x" };
    for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++)
        CHECK(command("aae off", 3, 0, maps[i], 50));
    CHECK(command("aae off", 3, 1, "zm_", 50));
    CHECK(command("aae off", 3, -1, "zm_", 50));
    CHECK(command("aae off", -1000, 0, "zm_", 50));
    for (int32_t state = -1; state < 8; state++)
        if (state != 5) {
            set_state(3, state);
            CHECK(command("aae off", 3, 0, "zm_", 50));
        }
    set_state(3, 5);
    CHECK(command("aae off", -1, 0, "zm_", 50));
    CHECK(command("aae off", 64, 0, "zm_", 50));
    CHECK(aae_freeaim_net_command(&test_table, "aae off", client_at(3) + 4, 0, "zm_", test_clients, CLIENT_SIZE, 50));
    CHECK(aae_freeaim_net_command(&test_table, "aae off", client_at(3), 0, "zm_", NULL, CLIENT_SIZE, 50));
    CHECK(entry_is(3, 4, client_at(3), 20, 1, 8, 10, 12));
    // "aae off" clears, also an entry that wasn't in use (which still counts as written)
    CHECK(command("aae off", 3, 0, "zm_", 60));
    CHECK(entry_is(3, 6, NULL, 0, 0, 0, 0, 0));
    CHECK(atomic_load(&test_table.in_use) == 1ull << 63);
    CHECK(command("aae off", 3, 0, "zm_", 60));
    CHECK(entry_is(3, 8, NULL, 0, 0, 0, 0, 0));
    // "aae off" written longer is the game's when it's 128 characters without an end
    char text[200];
    memset(text, ' ', sizeof(text));
    memcpy(text, "aae 1 1 1", 9);
    text[128] = '\0';
    CHECK(!command(text, 3, 0, "zm_", 70));
    text[127] = '\0';
    CHECK(command(text, 3, 0, "zm_", 70));
    CHECK(entry_is(3, 10, client_at(3), 70, 1, 2, 2, 2));
    // the bit for an entry in use is set when it wasn't valid before, kept when it was
    atomic_store(&test_table.in_use, 0);
    CHECK(command("aae 1 1 1", 3, 0, "zm_", 80));
    CHECK(atomic_load(&test_table.in_use) == 0);
    CHECK(command("aae 1 1 1", 4, 0, "zm_", 80));
    CHECK(atomic_load(&test_table.in_use) == 1u << 4);
}

static void test_copy(void) {
    table_empty();
    set_state(2, 5);
    CHECK(command("aae 1 2 3", 2, 0, "zm_", 5));
    aae_freeaim_net_copy_t copy;
    CHECK(aae_freeaim_net_copy(&test_table, 2, &copy));
    CHECK(copy.client == (uintptr_t)client_at(2) && copy.received_at == 5 && copy.valid == 1 && copy.target[0] == 2 &&
          copy.target[1] == 4 && copy.target[2] == 6);
    // an entry being written can't be read
    atomic_store(&test_table.entries[2].sequence, 3);
    CHECK(!aae_freeaim_net_copy(&test_table, 2, &copy));
    atomic_store(&test_table.entries[2].sequence, 4);
    CHECK(aae_freeaim_net_copy(&test_table, 2, &copy));
}

static void test_lookup(void) {
    table_empty();
    for (int slot = 0; slot < 64; slot++)
        set_state(slot, 5);
    CHECK(command("aae 10 20 30", 7, 0, "zm_", 5));
    float target[3] = { 0 };
    CHECK(aae_freeaim_net_lookup(&test_table, 7, "zm_", true, test_clients, CLIENT_SIZE, target));
    CHECK(target[0] == 20 && target[1] == 40 && target[2] == 60);
    float untouched[3] = { 9, 9, 9 };
    CHECK(!aae_freeaim_net_lookup(&test_table, 6, "zm_", true, test_clients, CLIENT_SIZE, untouched));
    CHECK(!aae_freeaim_net_lookup(&test_table, 7, "mp_", true, test_clients, CLIENT_SIZE, untouched));
    CHECK(!aae_freeaim_net_lookup(&test_table, 7, NULL, true, test_clients, CLIENT_SIZE, untouched));
    CHECK(!aae_freeaim_net_lookup(&test_table, 7, "zm_", false, test_clients, CLIENT_SIZE, untouched));
    CHECK(!aae_freeaim_net_lookup(&test_table, 7, "zm_", true, NULL, CLIENT_SIZE, untouched));
    CHECK(!aae_freeaim_net_lookup(&test_table, 7 + 64, "zm_", true, test_clients, CLIENT_SIZE, untouched));
    CHECK(!aae_freeaim_net_lookup(&test_table, 0xffffffffu, "zm_", true, test_clients, CLIENT_SIZE, untouched));
    // svs.clients moved: the entry's client isn't that player's any more
    CHECK(!aae_freeaim_net_lookup(&test_table, 6, "zm_", true, client_at(1), CLIENT_SIZE, untouched));
    // the client left the game
    set_state(7, 4);
    CHECK(!aae_freeaim_net_lookup(&test_table, 7, "zm_", true, test_clients, CLIENT_SIZE, untouched));
    set_state(7, 5);
    // received at time 0: counts as none
    CHECK(command("aae 1 1 1", 7, 0, "zm_", 0));
    CHECK(!aae_freeaim_net_lookup(&test_table, 7, "zm_", true, test_clients, CLIENT_SIZE, untouched));
    // being written
    CHECK(command("aae 1 1 1", 7, 0, "zm_", 1));
    atomic_fetch_add(&test_table.entries[7].sequence, 1);
    CHECK(!aae_freeaim_net_lookup(&test_table, 7, "zm_", true, test_clients, CLIENT_SIZE, untouched));
    atomic_fetch_add(&test_table.entries[7].sequence, 1);
    CHECK(aae_freeaim_net_lookup(&test_table, 7, "zm_", true, test_clients, CLIENT_SIZE, target));
    CHECK(aae_freeaim_net_has_entry(&test_table, 7) && !aae_freeaim_net_has_entry(&test_table, 6));
    CHECK(!aae_freeaim_net_has_entry(&test_table, 7 + 64));
    CHECK(command("aae 1 1 1", 63, 0, "zm_", 5));
    CHECK(aae_freeaim_net_has_entry(&test_table, 63) && !aae_freeaim_net_has_entry(&test_table, 64));
    atomic_fetch_add(&test_table.entries[7].sequence, 1);
    CHECK(!aae_freeaim_net_has_entry(&test_table, 7));
    atomic_fetch_add(&test_table.entries[7].sequence, 1);
    atomic_store(&test_table.entries[7].client, 0);
    CHECK(!aae_freeaim_net_has_entry(&test_table, 7));
    atomic_store(&test_table.entries[7].client, (uintptr_t)client_at(7));
    // not valid, whatever else is there
    atomic_store(&test_table.entries[7].valid, 0);
    CHECK(!aae_freeaim_net_has_entry(&test_table, 7));
    CHECK(!aae_freeaim_net_lookup(&test_table, 7, "zm_", true, test_clients, CLIENT_SIZE, untouched));
    atomic_store(&test_table.entries[7].valid, 1);
    // cleared
    CHECK(command("aae off", 7, 0, "zm_", 2));
    CHECK(!aae_freeaim_net_lookup(&test_table, 7, "zm_", true, test_clients, CLIENT_SIZE, untouched));
    CHECK(untouched[0] == 9 && untouched[1] == 9 && untouched[2] == 9);
}

static void test_sweep(void) {
    table_empty();
    for (int slot = 0; slot < 64; slot++)
        set_state(slot, 5);
    // nothing in use: nothing happens, the clock isn't noted
    aae_freeaim_net_sweep(&test_table, 1000, test_clients, CLIENT_SIZE);
    CHECK(test_table.swept_at == 0);
    CHECK(command("aae 1 1 1", 0, 0, "zm_", 900));
    CHECK(command("aae 2 2 2", 5, 0, "zm_", 900));
    CHECK(command("aae 3 3 3", 63, 0, "zm_", 900));
    // all there: nothing cleared, the time noted
    aae_freeaim_net_sweep(&test_table, 1000, test_clients, CLIENT_SIZE);
    CHECK(test_table.swept_at == 1000 && atomic_load(&test_table.in_use) == (1ull << 63 | 1u << 5 | 1u));
    // gone, but less than 500 ms since: kept
    set_state(5, 4);
    aae_freeaim_net_sweep(&test_table, 1499, test_clients, CLIENT_SIZE);
    CHECK(test_table.swept_at == 1000 && atomic_load(&test_table.in_use) == (1ull << 63 | 1u << 5 | 1u));
    aae_freeaim_net_sweep(&test_table, 999, test_clients, CLIENT_SIZE); // a clock that went back
    CHECK(test_table.swept_at == 1000 && atomic_load(&test_table.in_use) == (1ull << 63 | 1u << 5 | 1u));
    // at 500 ms: cleared (written once more)
    aae_freeaim_net_sweep(&test_table, 1500, test_clients, CLIENT_SIZE);
    CHECK(test_table.swept_at == 1500 && atomic_load(&test_table.in_use) == (1ull << 63 | 1u));
    CHECK(entry_is(5, 4, NULL, 0, 0, 0, 0, 0));
    CHECK(entry_is(0, 2, client_at(0), 900, 1, 2, 2, 2));
    // svs.clients gone: the time noted, nothing cleared
    aae_freeaim_net_sweep(&test_table, 2000, NULL, CLIENT_SIZE);
    CHECK(test_table.swept_at == 2000 && atomic_load(&test_table.in_use) == (1ull << 63 | 1u));
    // svs.clients moved: no entry's client is at its place
    aae_freeaim_net_sweep(&test_table, 2500, client_at(1), CLIENT_SIZE);
    CHECK(atomic_load(&test_table.in_use) == 0);
    CHECK(entry_is(0, 4, NULL, 0, 0, 0, 0, 0) && entry_is(63, 4, NULL, 0, 0, 0, 0, 0));
    // an entry being written meanwhile is left alone (four tries), and one whose bit is set without being valid
    CHECK(command("aae 1 1 1", 9, 0, "zm_", 3000));
    set_state(9, 0);
    atomic_fetch_add(&test_table.entries[9].sequence, 1);
    aae_freeaim_net_sweep(&test_table, 3000, test_clients, CLIENT_SIZE);
    CHECK(atomic_load(&test_table.in_use) == 1u << 9);
    atomic_fetch_add(&test_table.entries[9].sequence, 1);
    atomic_store(&test_table.entries[9].valid, 0);
    aae_freeaim_net_sweep(&test_table, 3500, test_clients, CLIENT_SIZE);
    CHECK(atomic_load(&test_table.in_use) == 1u << 9 && atomic_load(&test_table.entries[9].sequence) == 4);
    // the first sweep with the clock at 0 comes at once; and with the last at 0 the next one too
    table_empty();
    set_state(9, 5);
    CHECK(command("aae 1 1 1", 9, 0, "zm_", 0));
    set_state(9, 0);
    aae_freeaim_net_sweep(&test_table, 0, test_clients, CLIENT_SIZE);
    CHECK(test_table.swept_at == 0 && atomic_load(&test_table.in_use) == 0);
    set_state(9, 5);
    CHECK(command("aae 1 1 1", 9, 0, "zm_", 0));
    aae_freeaim_net_sweep(&test_table, 1, test_clients, CLIENT_SIZE);
    CHECK(test_table.swept_at == 1 && atomic_load(&test_table.in_use) == 1u << 9);
}

// ---- what goes on while an entry is read or written

static int odd_loads;                                // the next this many loads ahead of a copy see a write going on
static void (*meanwhile)(aae_freeaim_net_entry_t *); // done once at the next load behind a copy, as by another thread
static uint32_t test_sequence(void *at, int order, bool before) {
    aae_freeaim_net_entry_t *entry = at;
    static uint32_t seen_before;
    uint32_t count = atomic_load_explicit(&entry->sequence, order);
    if (before) {
        seen_before = count;
        if (odd_loads > 0) {
            odd_loads--;
            return count | 1;
        }
        return count;
    }
    if (meanwhile != NULL) {
        void (*write)(aae_freeaim_net_entry_t *) = meanwhile;
        meanwhile = NULL;
        write(entry);
        return seen_before; // as if read before the other thread's write was done
    }
    return count;
}
static void written_again(aae_freeaim_net_entry_t *entry) {
    // the other thread's writes come after the copy's loads: the copy has the old ones and its counts agree
    atomic_store(&entry->received_at, 4242);
    atomic_fetch_add(&entry->sequence, 2);
}

static void *store_thread(void *unused) {
    (void)unused;
    command("aae 3 3 3", 12, 0, "zm_", 77);
    return NULL;
}

static void test_meanwhile(void) {
    table_empty();
    for (int slot = 0; slot < 64; slot++)
        set_state(slot, 5);
    CHECK(command("aae 1 2 3", 12, 0, "zm_", 5));
    aae_freeaim_net_copy_t copy;
    // four tries
    odd_loads = 3;
    CHECK(aae_freeaim_net_copy(&test_table, 12, &copy) && copy.received_at == 5);
    odd_loads = 4;
    CHECK(!aae_freeaim_net_copy(&test_table, 12, &copy));
    odd_loads = 0;
    // the sweep: an entry written again between its copy and the sweep's own write is left as it is
    set_state(12, 0);
    meanwhile = written_again;
    aae_freeaim_net_sweep(&test_table, 1000, test_clients, CLIENT_SIZE);
    CHECK(meanwhile == NULL);
    CHECK(atomic_load(&test_table.entries[12].received_at) == 4242 && atomic_load(&test_table.in_use) == 1u << 12 &&
          atomic_load(&test_table.entries[12].sequence) == 6);
    set_state(12, 5);
    // a writer waits while another one is at it
    atomic_store(&test_table.entries[12].sequence, 7);
    pthread_t writer;
    pthread_create(&writer, NULL, store_thread, NULL);
    struct timespec wait = { 0, 50 * 1000 * 1000 };
    nanosleep(&wait, NULL);
    CHECK(atomic_load(&test_table.entries[12].sequence) == 7 &&
          atomic_load(&test_table.entries[12].received_at) == 4242);
    atomic_store(&test_table.entries[12].sequence, 8);
    pthread_join(writer, NULL);
    CHECK(atomic_load(&test_table.entries[12].sequence) == 10 && entry_is(12, 10, client_at(12), 77, 1, 6, 6, 6));
}

// ---- the layer facing the game, with freeaim_net_cases.h's made-up game

static int calls_on, calls_target, order_ok;
static bool counting_on(int local_client, uint64_t now) {
    calls_on++;
    order_ok = calls_target == calls_on - 1;
    return fake_on(local_client, now);
}
static bool counting_target(int local_client, float target[3]) {
    calls_target++;
    return fake_target(local_client, target);
}
static int map_asked;
static const char *counting_map(void) {
    map_asked++;
    return game_map;
}

static void test_layer(void) {
    cases_start();
    aae_freeaim_net_game_t with = {
        .active = fake_active,
        .now_ms = fake_now,
        .map = counting_map,
        .on = counting_on,
        .local_target = counting_target,
        .add_reliable_command = game_add_reliable,
        .server_state = &game_sv_state,
        .client_connections = &game_clc,
        .server_clients = &game_clients,
        .client_size = CLIENT_SIZE,
    };
    aae_freeaim_net_init(&with);
    game_on[1] = game_has[1] = 1;
    game_target[1][0] = 100.0f, game_target[1][1] = 7.0f, game_target[1][2] = -7.0f;
    // the second local client's own counters
    cases_counters(0, 50, 0);
    cases_counters(1, 3, 1);
    aae_freeaim_net_client_frame(1);
    CHECK(strcmp(game_sent, "[1:aae 50 4 -4]") == 0);
    CHECK(calls_on == 1 && calls_target == 1 && order_ok);
    game_sent[0] = '\0';
    cases_counters(1, 30, 1);
    game_target[1][0] = 0.0f, game_now += 100;
    aae_freeaim_net_client_frame(1);
    CHECK(game_sent[0] == '\0');
    // the target is asked for also while free aim is off (as the plugin does)
    game_on[1] = 0;
    aae_freeaim_net_client_frame(1);
    CHECK(calls_on == 3 && calls_target == 3 && game_sent[0] == '\0');
    game_on[1] = 1;
    // asked even while this machine runs a server, whose clients' entries are swept from here
    game_sv_state = 2;
    cases_counters(1, 3, 1);
    aae_freeaim_net_client_frame(1);
    CHECK(game_sent[0] == '\0' && calls_on == 3 + 1 && calls_target == 3 + 1);
    game_sv_state = 0;
    // not for local clients beyond the second, nor without the mod
    aae_freeaim_net_client_frame(2);
    aae_freeaim_net_client_frame(-1);
    game_active = 0;
    aae_freeaim_net_client_frame(1);
    CHECK(calls_on == 4 && game_sent[0] == '\0');
    game_active = 1;
    aae_freeaim_net_client_frame(1);
    CHECK(strcmp(game_sent, "[1:aae 0 4 -4]") == 0);
    game_sent[0] = '\0';
    // the server's side: only "aae " is looked at
    map_asked = 0;
    CHECK(!aae_freeaim_net_server_command(cases_client(4), "say aae 1 2 3", 0));
    CHECK(!aae_freeaim_net_server_command(cases_client(4), NULL, 0));
    CHECK(map_asked == 0);
    CHECK(aae_freeaim_net_server_command(cases_client(4), "aae 1 2 3", 0));
    CHECK(map_asked == 1);
    float target[3];
    CHECK(aae_freeaim_net_remote_target(4, target) && target[0] == 2 && target[1] == 4 && target[2] == 6);
    CHECK(aae_freeaim_net_remote_entry(4) && !aae_freeaim_net_remote_entry(5));
    CHECK(!aae_freeaim_net_remote_target(5, target));
    game_active = 0; // without the mod: all the game's
    CHECK(!aae_freeaim_net_server_command(cases_client(4), "aae off", 0));
    CHECK(!aae_freeaim_net_remote_target(4, target) && !aae_freeaim_net_remote_entry(4));
    game_active = 1;
    CHECK(aae_freeaim_net_remote_target(4, target));
    // the sweep from the client's frame, with svs.clients
    cases_state(4, 1);
    game_now += 1000;
    aae_freeaim_net_client_frame(0);
    CHECK(!aae_freeaim_net_remote_target(4, target) && atomic_load(&aae_freeaim_net_table()->in_use) == 0);
    // no clc: nothing sent
    game_clc = NULL;
    game_has[1] = 0;
    game_now += 1000;
    aae_freeaim_net_client_frame(1);
    CHECK(game_sent[0] == '\0');
    game_clc = game_clc_memory;
    aae_freeaim_net_client_frame(1);
    CHECK(strcmp(game_sent, "[1:aae off]") == 0);
    game_sent[0] = '\0';
}

// ---- two threads: the server's writes and the shot hooks' reads, against the client's sweep

static atomic_int stop_threads;
static atomic_long torn, reads_ok;
static void *server_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; !atomic_load(&stop_threads); i++) {
        int slot = (int)(i % 4);
        char text[64];
        snprintf(text, sizeof(text), "aae %u %u %u", i % 1000, i % 1000, i % 1000);
        command(i % 7 == 0 ? "aae off" : text, slot, 0, "zm_", 1 + i);
        float target[3];
        if (aae_freeaim_net_lookup(&test_table, (uint32_t)((i + 1) % 4), "zm_", true, test_clients, CLIENT_SIZE,
                                   target)) {
            atomic_fetch_add(&reads_ok, 1);
            if (target[0] != target[1] || target[1] != target[2])
                atomic_fetch_add(&torn, 1);
        }
    }
    return NULL;
}
static void *client_thread(void *unused) {
    (void)unused;
    for (uint64_t now = 1000; !atomic_load(&stop_threads); now += 500) {
        aae_freeaim_net_sweep(&test_table, now, test_clients, CLIENT_SIZE);
        for (int slot = 0; slot < 4; slot++) {
            aae_freeaim_net_copy_t copy;
            if (aae_freeaim_net_copy(&test_table, slot, &copy) &&
                (copy.target[0] != copy.target[1] || (copy.valid != 0) != (copy.client != 0)))
                atomic_fetch_add(&torn, 1);
        }
    }
    return NULL;
}
static void test_threads(void) {
    table_empty();
    for (int slot = 0; slot < 64; slot++)
        set_state(slot, 5);
    pthread_t server, client;
    pthread_create(&server, NULL, server_thread, NULL);
    pthread_create(&client, NULL, client_thread, NULL);
    struct timespec wait = { 0, 300 * 1000 * 1000 };
    nanosleep(&wait, NULL);
    atomic_store(&stop_threads, 1);
    pthread_join(server, NULL);
    pthread_join(client, NULL);
    CHECK(atomic_load(&torn) == 0);
    CHECK(atomic_load(&reads_ok) > 100);
    for (int slot = 0; slot < 4; slot++)
        CHECK((atomic_load(&test_table.entries[slot].sequence) & 1) == 0);
}

// ---- the plugin's own output for freeaim_net_cases.h: its line count and checksum
#define Plugin_Lines 75346
#define Plugin_Checksum 0x7bc365618d518679ULL
static long case_lines;
static uint64_t case_checksum = 0xcbf29ce484222325ULL;
static void cases_line(const char *line) {
    case_lines++;
    for (const unsigned char *p = (const unsigned char *)line; *p; p++)
        case_checksum = (case_checksum ^ *p) * 0x100000001b3ULL;
    case_checksum = (case_checksum ^ '\n') * 0x100000001b3ULL;
}
static void test_like_the_plugin(void) {
    cases_run();
    if (case_lines != Plugin_Lines || case_checksum != Plugin_Checksum)
        printf("  the made-up cases: %ld lines, checksum %#llx\n", case_lines, (unsigned long long)case_checksum);
    CHECK(case_lines == Plugin_Lines);
    CHECK(case_checksum == Plugin_Checksum);
}

int main(void) {
    test_clients = calloc(68, CLIENT_SIZE);
    game_client_memory = calloc(CLIENT_SLOTS, CLIENT_SIZE);
    game_clc_memory = calloc(2, CLC_SIZE);
    if (test_clients == NULL || game_client_memory == NULL || game_clc_memory == NULL)
        return 2;
    test_clients += CLIENT_SIZE; // one before it
    test_half();
    test_send();
    test_read();
    test_slot();
    test_command();
    test_copy();
    test_lookup();
    test_sweep();
    test_meanwhile();
    test_layer();
    test_threads();
    test_like_the_plugin();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("aae_freeaim_net: all passed\n");
    return 0;
}
