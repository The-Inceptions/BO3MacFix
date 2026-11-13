// Made-up cases for free aim over the network: client commands for the server, frames of a local client that sends,
// lookups. The same ones went through the plugin's own code (AAEUnified.dll mapped under Wine without running
// anything of it, its hooks called by address: not in the repository) and go through aae_freeaim_net.c; a line goes
// out after every step with everything the code keeps, and the two outputs are compared. What the plugin's side gave
// is in test_aae_freeaim_net.c as a line count and a checksum.
//
// Whoever includes this defines CLIENT_SIZE (its game's client_t) and gives the side_ functions below.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the made-up game, the same for both sides
#define CLC_SIZE 0x25780
#define CLIENT_SLOTS 67 // one before svs.clients, 64, and two behind
static uint8_t *game_client_memory; // CLIENT_SLOTS client_ts
static uint8_t *game_clients;       // what svs.clients holds: game_client_memory + one client_t, or NULL
static uint8_t *game_clc_memory;    // two clientConnection_ts
static uint8_t *game_clc;           // what the clc pointer holds
static int32_t game_sv_state;
static uint64_t game_now;
static const char *game_map;
static int game_active = 1;
static int game_on[2], game_has[2];
static float game_target[2][3];
static char game_sent[512]; // what CL_AddReliableCommand was given during a step

static void game_add_reliable(int local_client, const char *text) {
    size_t used = strlen(game_sent);
    snprintf(game_sent + used, sizeof(game_sent) - used, "[%d:%s]", local_client, text);
}

// ---- what each side gives
static void cases_line(const char *line);
static void side_reset(void);
static bool side_command(const uint8_t *client, const char *text, int from_old_server); // true: swallowed
static void side_frame(int local_client);
static bool side_lookup(uint32_t player, float target[3]);
// what is kept: an entry as it is in memory, the bits of entries in use, the sweep's time, a sender
static void side_entry(int slot, uint32_t *sequence, uintptr_t *client, uint64_t *tick, uint32_t *valid,
                       uint32_t target[3]);
static uint64_t side_in_use(void);
static uint64_t side_swept_at(void);
static void side_sender(int local_client, uint64_t *sent_at, int32_t sent[3], int *sent_target);

static uint64_t cases_seed = 0x5eed;
static uint32_t cases_next(void) {
    cases_seed = cases_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(cases_seed >> 33);
}
static uint32_t cases_bits(float value) {
    uint32_t b;
    memcpy(&b, &value, sizeof(b));
    return b;
}
static float cases_from_bits(uint32_t b) {
    float value;
    memcpy(&value, &b, sizeof(value));
    return value;
}
static uint8_t *cases_client(int slot) {
    return game_client_memory + (ptrdiff_t)(slot + 1) * CLIENT_SIZE;
}
static void cases_state(int slot, int32_t state) {
    memcpy(cases_client(slot), &state, sizeof(state));
}
static void cases_counters(int local_client, uint32_t sequence, uint32_t acknowledged) {
    memcpy(game_clc_memory + (size_t)local_client * CLC_SIZE + 0x130, &sequence, 4);
    memcpy(game_clc_memory + (size_t)local_client * CLC_SIZE + 0x134, &acknowledged, 4);
}
// a client pointer as a place among the made-up client_ts: "slot+offset", or "-" for none
static void cases_where(uintptr_t client, char *out, size_t size) {
    if (client == 0) {
        snprintf(out, size, "-");
        return;
    }
    intptr_t offset = (intptr_t)(client - (uintptr_t)game_client_memory) - CLIENT_SIZE;
    intptr_t slot = offset >= 0 ? offset / CLIENT_SIZE : -1 - (-offset - 1) / CLIENT_SIZE;
    snprintf(out, size, "%ld+%ld", (long)slot, (long)(offset - slot * CLIENT_SIZE));
}

// everything kept, after a step
static void cases_dump(const char *step) {
    static char line[16384];
    int used = snprintf(line, sizeof(line), "%s | %016llx %llu", step, (unsigned long long)side_in_use(),
                        (unsigned long long)side_swept_at());
    for (int lc = 0; lc < 2; lc++) {
        uint64_t at;
        int32_t sent[3];
        int flag;
        side_sender(lc, &at, sent, &flag);
        used += snprintf(line + used, sizeof(line) - used, " s%d=%llu,%d,%d,%d,%d", lc, (unsigned long long)at,
                         sent[0], sent[1], sent[2], flag);
    }
    for (int slot = 0; slot < 64; slot++) {
        uint32_t sequence, valid, target[3];
        uintptr_t client;
        uint64_t tick;
        side_entry(slot, &sequence, &client, &tick, &valid, target);
        if (sequence == 0 && client == 0 && tick == 0 && valid == 0 && target[0] == 0 && target[1] == 0 &&
            target[2] == 0)
            continue;
        char where[64];
        cases_where(client, where, sizeof(where));
        used += snprintf(line + used, sizeof(line) - used, " e%d=%u,%s,%llu,%u,%08x,%08x,%08x", slot, sequence, where,
                         (unsigned long long)tick, valid, target[0], target[1], target[2]);
    }
    if (game_sent[0] != '\0')
        used += snprintf(line + used, sizeof(line) - used, " sent %s", game_sent);
    game_sent[0] = '\0';
    cases_line(line);
}

static void cases_start(void) {
    side_reset();
    memset(game_client_memory, 0, (size_t)CLIENT_SLOTS * CLIENT_SIZE);
    memset(game_clc_memory, 0, 2 * CLC_SIZE);
    game_clients = game_client_memory + CLIENT_SIZE;
    game_clc = game_clc_memory;
    game_sv_state = 0;
    game_now = 1000000;
    game_map = "zm_theater";
    game_active = 1;
    memset(game_on, 0, sizeof(game_on));
    memset(game_has, 0, sizeof(game_has));
    memset(game_target, 0, sizeof(game_target));
    game_sent[0] = '\0';
    for (int slot = 0; slot < 64; slot++)
        cases_state(slot, 5);
}

static void cases_command_at(const uint8_t *client, const char *text, int from_old_server, const char *label) {
    char step[400];
    // (commands always with the plugin there: on the Mac nothing is taken from the game without it, see the notes)
    int active = game_active;
    game_active = 1;
    bool swallowed = side_command(client, text, from_old_server);
    game_active = active;
    snprintf(step, sizeof(step), "c %s %d", label, swallowed);
    cases_dump(step);
}

// ---- the server: texts
static void cases_texts(void) {
    static const char *const written[] = {
        "aae 1 2 3", "aae off", "aae off ", "aae  off", "aae of", "aae o", "aae ", "aae", "aa", "", "aae\t1 2 3",
        "aaeoff", "AAE 1 2 3", "aae 0 0 0", "aae -0 +0 -0", "aae +1 -2 +3", "aae 1 2", "aae 1 2 3 4", "aae 1 2 3 ",
        "aae 1 2 3\t\n\v\f\r", "aae \t 1 \n 2 \r 3", "aae 1  2  3", "aae 1,2,3", "aae 1.5 2 3", "aae 1 2 3.5",
        "aae 1 2 3x", "aae x 2 3", "aae - 1 2 3", "aae -1 -2 -3", "aae 8388608 8388608 8388608",
        "aae -8388608 -8388608 -8388608", "aae 8388609 0 0", "aae -8388609 0 0", "aae 0 8388609 0", "aae 0 0 8388609",
        "aae 0 0 -8388609", "aae 838860 838860 838860", "aae 8388600 0 0", "aae 8388607 0 0", "aae 83886080 0 0",
        "aae 00000000000000000001 2 3", "aae 0000000000000008388608 0 0", "aae 4294967296 0 0",
        "aae 99999999999 0 0", "aae 1 2 3\x80", ("aae \x80" "1 2 3"), "aae 1\x80 2 3", "aae 1 2 \xff", "aae 1:2 3 4",
        "aae 1/ 2 3", "aae +-1 2 3", "aae ++1 2 3", "aae 1 - 2", "aae 12 34 56", "aae -4194304 4194303 1",
        "aae 1 2 off", "aae offf", "aae off\t", "aae 1e3 2 3", "aae 0x10 2 3", "aae 7 8 9\x01", "aae 7\x01 8 9",
        "aae\x20\x20", "aae 5 5 5\n", "aae 1 2 3\x1f", "aae 1\x0e 2 3", "aae 1\x08 2 3",
    };
    for (size_t i = 0; i < sizeof(written) / sizeof(written[0]); i++) {
        char label[300];
        snprintf(label, sizeof(label), "text%zu", i);
        cases_command_at(cases_client((int)(i % 64)), written[i], 0, label);
    }
    // long ones, about the 128 characters the plugin copies
    for (int length = 120; length <= 140; length++) {
        char text[200];
        snprintf(text, sizeof(text), "aae 1 2 3");
        memset(text + 9, ' ', sizeof(text) - 9);
        text[length] = '\0';
        char label[40];
        snprintf(label, sizeof(label), "long%d", length);
        cases_command_at(cases_client(7), text, 0, label);
        memset(text, '0', sizeof(text));
        memcpy(text, "aae 5 6 ", 8);
        text[length] = '\0';
        snprintf(label, sizeof(label), "zeros%d", length);
        cases_command_at(cases_client(8), text, 0, label);
    }
    // made-up ones
    static const char pieces[] = "0123456789+- \t\n\v\f\roffaex.,";
    for (int i = 0; i < 30000; i++) {
        char text[64] = "aae ";
        int length = 4 + (int)(cases_next() % 30);
        for (int k = 4; k < length; k++) {
            uint32_t pick = cases_next() % 64;
            if (pick < 40)
                text[k] = "0123456789"[cases_next() % 10];
            else if (pick < 50)
                text[k] = ' ';
            else if (pick == 50)
                text[k] = (char)(0x80 + cases_next() % 128);
            else if (pick == 51)
                text[k] = (char)(1 + cases_next() % 31);
            else
                text[k] = pieces[cases_next() % (sizeof(pieces) - 1)];
        }
        text[length] = '\0';
        if (cases_next() % 16 == 0)
            memcpy(text, "aaf", 3);
        char label[40];
        snprintf(label, sizeof(label), "made%d", i);
        cases_command_at(cases_client((int)(cases_next() % 64)), text, 0, label);
        if (cases_next() % 8 == 0) {
            snprintf(label, sizeof(label), "off%d", i);
            cases_command_at(cases_client((int)(cases_next() % 64)), "aae off", 0, label);
        }
        game_now += cases_next() % 3;
    }
}

// ---- the server: who sends
static void cases_senders(void) {
    static const char *const maps[] = { "zm_theater", NULL, "", "z", "zm", "zm_", "mp_nuketown", "ZM_x", "zm_x\x80" };
    for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        cases_start();
        game_map = maps[m];
        char label[64];
        snprintf(label, sizeof(label), "map%zu", m);
        cases_command_at(cases_client(3), "aae 4 5 6", 0, label);
        cases_command_at(cases_client(3), "aae off", 0, label);
        cases_command_at(cases_client(3), "x", 0, label);
    }
    cases_start();
    for (int old = 0; old < 3; old++)
        cases_command_at(cases_client(1), "aae 1 1 1", old, "old");
    cases_command_at(NULL, "aae 1 1 1", 0, "null");
    cases_command_at(NULL, "aae off", 0, "nulloff");
    static const int32_t states[] = { 0, 1, 2, 3, 4, 5, 6, -1 };
    for (size_t s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
        cases_state(9, states[s]);
        cases_command_at(cases_client(9), "aae 9 9 9", 0, "state");
        cases_command_at(cases_client(9), "aae off", 0, "stateoff");
        cases_command_at(cases_client(9), "aae 9 9 10", 0, "state");
    }
    cases_state(-1, 5), cases_state(64, 5), cases_state(65, 5);
    cases_command_at(cases_client(-1), "aae 2 2 2", 0, "before");
    cases_command_at(cases_client(63), "aae 2 2 2", 0, "last");
    cases_command_at(cases_client(64), "aae 2 2 2", 0, "beyond");
    cases_command_at(cases_client(65), "aae 2 2 2", 0, "beyond2");
    for (int odd = 1; odd < 9; odd++) {
        uint8_t *client = cases_client(10) + odd * 3;
        int32_t five = 5;
        memcpy(client, &five, 4);
        cases_command_at(client, "aae 3 3 3", 0, "misplaced");
    }
    game_clients = NULL;
    cases_command_at(cases_client(11), "aae 3 3 3", 0, "noclients");
    game_clients = cases_client(2);
    cases_command_at(cases_client(11), "aae 3 3 3", 0, "moved");
    cases_command_at(cases_client(0), "aae 3 3 3", 0, "moved");
    game_clients = game_client_memory + CLIENT_SIZE;
}

// ---- lookups
static void cases_lookups(const char *label) {
    char step[128];
    char found[8192] = "";
    size_t used = 0;
    static const uint32_t players[] = { 64, 65, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe };
    for (uint32_t i = 0; i < 64 + sizeof(players) / sizeof(players[0]); i++) {
        uint32_t player = i < 64 ? i : players[i - 64];
        float target[3] = { 7.0f, 7.0f, 7.0f };
        if (side_lookup(player, target))
            used += (size_t)snprintf(found + used, sizeof(found) - used, " %u=%08x,%08x,%08x", player,
                                     cases_bits(target[0]), cases_bits(target[1]), cases_bits(target[2]));
    }
    snprintf(step, sizeof(step), "l %s", label);
    char line[8400];
    snprintf(line, sizeof(line), "%s%s", step, found);
    cases_line(line);
}

// ---- the sweep (in the client's frames) and lookups
static void cases_sweeps(void) {
    cases_start();
    game_sv_state = 2;
    for (int slot = 0; slot < 64; slot += 3) {
        char text[64];
        snprintf(text, sizeof(text), "aae %d %d %d", slot, -slot, slot * 1000);
        cases_command_at(cases_client(slot), text, 0, "fill");
    }
    cases_lookups("filled");
    for (int round = 0; round < 600; round++) {
        uint32_t pick = cases_next() % 16;
        game_now += cases_next() % 300;
        int slot = (int)(cases_next() % 64);
        char label[64];
        switch (pick) {
        case 0: cases_state(slot, (int32_t)(cases_next() % 7)); break;
        case 1: cases_state(slot, 5); break;
        case 2: game_now += cases_next() % 700; break;
        case 3: game_now -= cases_next() % 100; break;
        case 4: game_clients = cases_next() % 2 ? NULL : cases_client((int)(cases_next() % 3) - 1); break;
        case 5: game_clients = game_client_memory + CLIENT_SIZE; break;
        case 6: game_map = cases_next() % 2 ? "mp_x" : "zm_x"; break;
        case 7: game_active = (int)(cases_next() % 4 != 0); break;
        case 8: {
            char text[64];
            snprintf(text, sizeof(text), "aae %d 1 2", (int)(cases_next() % 100));
            cases_command_at(cases_client(slot), text, 0, "refill");
            break;
        }
        case 9: cases_command_at(cases_client(slot), "aae off", 0, "clear"); break;
        default: break;
        }
        snprintf(label, sizeof(label), "sweep%d.%u", round, pick);
        side_frame((int)(cases_next() % 2));
        cases_dump(label);
        cases_lookups(label);
    }
    // the sweep's clock: the first time, under 500 ms, at 500 ms, and a clock that went back
    cases_start();
    game_sv_state = 2;
    cases_command_at(cases_client(4), "aae 1 1 1", 0, "fill");
    static const int64_t steps[] = { 0, 1, 498, 1, 0, 1, 499, 500, -1, -600, 2, 1000000, -2000000, 0, 500 };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        game_now += (uint64_t)steps[i];
        if (i % 3 == 2)
            cases_state(4, i % 2 ? 5 : 4);
        if (i == 6)
            cases_command_at(cases_client(4), "aae 1 1 2", 0, "again");
        char label[64];
        snprintf(label, sizeof(label), "clock%zu", i);
        side_frame(0);
        cases_dump(label);
    }
    cases_start();
    game_sv_state = 2;
    game_now = 0; // a sweep at time 0 counts as none
    cases_command_at(cases_client(5), "aae 1 1 1", 0, "fill0");
    cases_state(5, 3);
    side_frame(0);
    cases_dump("zero");
    side_frame(1);
    cases_dump("zero");
    game_now = 10;
    side_frame(1);
    cases_dump("zero10");
}

// ---- the client: what it sends
static float cases_coordinate(void) {
    switch (cases_next() % 24) {
    case 0: return cases_from_bits(cases_next() * 2u + (cases_next() & 1));
    case 1: return (float)(int)(cases_next() % 2000) * 0.5f - 500.0f; // halves and quarters about the edges of rounding
    case 2: return (float)(int)(cases_next() % 4000) * 0.25f - 500.0f;
    case 3: return cases_from_bits(0x7fc00000);
    case 4: return cases_from_bits(0x7f800000);
    case 5: return cases_from_bits(0xff800000);
    case 6: return -0.0f;
    case 7: return 16777216.0f;
    case 8: return -16777216.0f;
    case 9: return cases_from_bits(0x4b000000 + cases_next() % 0x1000000); // 2^23 and on
    case 10: return -cases_from_bits(0x4b000000 + cases_next() % 0x1000000);
    case 11: return 4294967296.0f * (cases_next() % 2 ? 1.0f : -1.0f);
    case 12: return cases_from_bits(0x4effffff + cases_next() % 3) * (cases_next() % 2 ? 1.0f : -1.0f);
    case 13: return cases_from_bits(cases_next() % 0x800000); // tiny
    case 14: return 0.99999994f * (cases_next() % 2 ? 1.0f : -1.0f);
    case 15: return 2.9999998f * (cases_next() % 2 ? 1.0f : -1.0f);
    default: return (float)((int)(cases_next() % 200000) - 100000) * 0.01f + (float)(cases_next() % 8) * 1000.0f;
    }
}

static void cases_clients_send(void) {
    cases_start();
    for (int frame = 0; frame < 40000; frame++) {
        int lc = (int)(cases_next() % 2);
        uint32_t pick = cases_next() % 40;
        if (pick == 0)
            game_sv_state = (int32_t)(cases_next() % 3);
        else if (pick == 1)
            game_sv_state = 0;
        else if (pick == 2)
            game_on[lc] = !game_on[lc];
        else if (pick == 3)
            game_has[lc] = !game_has[lc];
        else if (pick < 12)
            for (int i = 0; i < 3; i++)
                game_target[lc][i] = cases_coordinate();
        else if (pick == 12)
            game_target[lc][cases_next() % 3] += 1.0f;
        else if (pick == 13) {
            uint32_t sequence = cases_next() % 4 ? cases_next() % 200 : cases_next();
            cases_counters(lc, sequence, sequence - cases_next() % 20 + (cases_next() % 8 == 0 ? 30 : 0));
        } else if (pick == 14)
            game_clc = cases_next() % 4 ? game_clc_memory : NULL;
        else if (pick == 15)
            game_active = (int)(cases_next() % 8 != 0);
        else if (pick == 16)
            game_now -= cases_next() % 80;
        else if (pick == 17)
            game_on[lc] = 1, game_has[lc] = 1, game_sv_state = 0, game_clc = game_clc_memory, game_active = 1;
        game_now += cases_next() % 40;
        int which = cases_next() % 64 == 0 ? (int)(cases_next() % 5) - 2 : lc;
        side_frame(which);
        char label[64];
        snprintf(label, sizeof(label), "f%d.%d.%u", frame, which, pick);
        cases_dump(label);
    }
}

static void cases_run(void) {
    cases_start();
    cases_texts();
    cases_lookups("texts");
    cases_senders();
    cases_lookups("senders");
    cases_sweeps();
    cases_clients_send();
}
