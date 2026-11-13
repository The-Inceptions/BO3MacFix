// aae_hostmig.c's rules, and its detour's and hooks' handlers against a made-up game: a session word, the migration's
// entries, the game's pick of a new host (by its own rule) and its "migrate to" message. The thunk that calls the
// detour's handler in the game is x86_64 code and has a test of its own (test_detour.c).
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
#include "../source/aae_hostmig.c"

// ---- what the code asks of the rest of BO3MacFix

static char mod_folder[4096];
const char *t7o_mod_folder(void) {
    return mod_folder;
}
// which places the pre-patched executable has stubs for, and what was hooked
static bool have_stubs;
static int stubs_missing_for = -1;
bool prepatch_has_hook(uint32_t offset) {
    return have_stubs && (int)offset != stubs_missing_for;
}
static struct {
    void *address, *handler, **original;
} hooked[8];
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    if (hooks < 8)
        hooked[hooks] = (typeof(hooked[0])){ address, fake_func, out_origin_func };
    hooks++;
    return 0;
}
static bool was_hooked(uint64_t base, uint32_t offset, void *handler, void *original) {
    for (int i = 0; i < hooks && i < 8; i++)
        if (hooked[i].address == (void *)(base + offset))
            return hooked[i].handler == handler && hooked[i].original == original;
    return false;
}

// ---- the mod with the plugin

static char loaded_name[Mod_Name_Size + 8];
static uint64_t the_time = 5000;
static uint64_t test_now(void) {
    return the_time;
}
static void make_folder(const char *path) {
    char part[4096];
    snprintf(part, sizeof(part), "%s", path);
    for (char *p = part + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(part, 0755);
            *p = '/';
        }
    }
    mkdir(part, 0755);
}
static char plugin_file[4096 + 64];
// the mod with its plugin loaded or not, and enough time gone for the next look
static void set_plugin(bool loaded) {
    close(open(plugin_file, O_CREAT | O_WRONLY, 0644));
    memset(loaded_name, 0, sizeof(loaded_name));
    if (loaded)
        snprintf(loaded_name, sizeof(loaded_name), "%s", strrchr(mod_folder, '/') + 1);
    the_time += Plugin_Look_Ms;
}

// ---- the made-up game

// a session word: the network mode (2 online, 1 LAN, 0 offline), the game mode's four bits (1 custom, 4 freerun), and
// whatever else the game keeps in it
static uint32_t word_of(unsigned network, unsigned game_mode, uint32_t other) {
    return (network & 0xf) << 6 | (game_mode & 0xf) << 14 | (other & ~(0x3c0u | 0x3c000u));
}
#define Online_Custom word_of(2, 1, 0x2)
#define Online_Public word_of(2, 0, 0x2)

static uint32_t game_word;
static int32_t game_state;
static aae_hostmig_client_t game_clients[Hostmig_Clients];
static int32_t game_new_host;
static uint8_t game_in_progress;
static uint8_t game_lobbies[0x500]; // the party lobby: its state at +0x40, its members at +0xf8

static void set_party(int32_t state, int member_at, uint64_t xuid) {
    memset(game_lobbies, 0, sizeof(game_lobbies));
    memcpy(game_lobbies + 0x40, &state, sizeof(state));
    for (int i = 0; i < Hostmig_Clients; i++) {
        uint64_t member = i == member_at ? xuid : 0x1100000000000000ull + (uint64_t)i;
        memcpy(game_lobbies + 0xf8 + i * 0x30, &member, sizeof(member));
    }
}

// The game's pick of the new host, as it is: the best rating above 1 among the players in the migration, the first one
// on a tie; the one picked has its count of messages cleared.
static int pick_calls;
static int32_t ratings_seen[Hostmig_Clients]; // what the pick saw
static void game_pick_new_host(void) {
    pick_calls++;
    int32_t best = 1;
    game_new_host = -1;
    for (int i = 0; i < Hostmig_Clients; i++) {
        ratings_seen[i] = game_clients[i].rating;
        if (game_clients[i].state != 0 && game_clients[i].rating > best) {
            game_new_host = i;
            best = game_clients[i].rating;
        }
    }
    if (game_new_host != -1)
        game_clients[game_new_host].sent = 0;
}

// The game's "migrate to" message: gets through the lobbies it's told to
static struct {
    uint64_t controller, xuid, lobby_type, count;
    const uint64_t *list;
} sends[4];
static int send_calls;
static bool gets_through[2]; // through the party lobby (0), through the game's (1)
static bool game_send(uint64_t controller, uint64_t xuid, uint64_t lobby_type, const uint64_t *list, uint64_t count) {
    if (send_calls < 4)
        sends[send_calls] = (typeof(sends[0])){ controller, xuid, lobby_type, count, list };
    send_calls++;
    return gets_through[lobby_type & 1];
}

// players in the migration: entries 'at' (a list ending in -1) answered with these ratings
static void set_players(const int *at, int32_t rating) {
    memset(game_clients, 0, sizeof(game_clients));
    for (int i = 0; i < Hostmig_Clients; i++) {
        game_clients[i].xuid = 0x110000100000000ull + (uint64_t)i;
        game_clients[i].rating = 0x5a5a; // what an entry not in the migration holds doesn't count
        game_clients[i].sent = 3;
    }
    for (; *at >= 0; at++) {
        game_clients[*at].state = 2;
        game_clients[*at].rating = rating;
        game_clients[*at].sent = 1;
    }
}

// ---- the session word

static void test_session(void) {
    CHECK(aae_hostmig_online(word_of(2, 0, 0)) && aae_hostmig_online(word_of(2, 15, 0xfffc003f)));
    CHECK(!aae_hostmig_online(word_of(0, 0, 0)) && !aae_hostmig_online(word_of(1, 1, 0)) &&
          !aae_hostmig_online(word_of(3, 1, 0)) && !aae_hostmig_online(word_of(6, 1, 0)) &&
          !aae_hostmig_online(word_of(10, 1, 0)));
    // the game mode as the game's own test reads it: four bits with a sign
    CHECK(aae_hostmig_game_mode(word_of(2, 1, 0)) == 1 && aae_hostmig_game_mode(word_of(0, 4, 0xffffffff)) == 4);
    CHECK(aae_hostmig_game_mode(word_of(2, 0, 0xfffc3fff)) == 0 && aae_hostmig_game_mode(word_of(2, 7, 0)) == 7);
    CHECK(aae_hostmig_game_mode(word_of(2, 15, 0)) == -1 && aae_hostmig_game_mode(word_of(2, 8, 0)) == -8);
    // the plugin's own test for a custom game: those four bits are 1, nothing else counts
    CHECK(custom_game(word_of(2, 1, 0)) && custom_game(word_of(0, 1, 0xfffc3fff)));
    CHECK(!custom_game(word_of(2, 0, 0)) && !custom_game(word_of(2, 3, 0)) && !custom_game(word_of(2, 5, 0)) &&
          !custom_game(word_of(2, 9, 0)));
}

// ---- the leave decision

static void test_leave(void) {
    // every network mode, game mode and answer of the game's test: the game's own decision ends the match unless the
    // test says yes and it isn't freerun; the plugin's goes to the migration in any online game, any other the
    // game's own way
    for (unsigned network = 0; network < 16; network++)
        for (unsigned mode = 0; mode < 16; mode++)
            for (int may = 0; may < 2; may++) {
                uint32_t word = word_of(network, mode, 0x2);
                aae_hostmig_leave_t own = may && mode != 4 ? Hostmig_Migrate : Hostmig_End_Match;
                aae_hostmig_leave_t plugin = network == 2 ? Hostmig_Migrate : own;
                CHECK(aae_hostmig_leave_own(may, word) == own);
                CHECK(aae_hostmig_leave(may, word) == plugin);
            }
    // as they come up: a mod is loaded, so the game's test says no
    CHECK(aae_hostmig_leave(false, Online_Custom) == Hostmig_Migrate);
    CHECK(aae_hostmig_leave(false, Online_Public) == Hostmig_Migrate);
    CHECK(aae_hostmig_leave(false, word_of(1, 1, 0)) == Hostmig_End_Match); // LAN
    CHECK(aae_hostmig_leave(false, word_of(0, 2, 0)) == Hostmig_End_Match); // offline, devmap
    CHECK(aae_hostmig_leave(false, word_of(2, 4, 0)) == Hostmig_Migrate);   // online freerun too
}

// the detour behind the test's call: al is its answer, the rest of rax whatever that function left there
static void test_leave_detour(void) {
    session_mode = &game_word;
    set_plugin(true);
    game_word = Online_Custom;
    CHECK(handler_Leave_Decision(0x12345600, 0x77)); // al 0: the game would end the match
    CHECK(handler_Leave_Decision(0x1, 0));
    game_word = word_of(0, 2, 0x2); // offline: the game's own way
    CHECK(!handler_Leave_Decision(0xffffff00, 0));
    CHECK(handler_Leave_Decision(0xabcd01, 0));
    game_word = word_of(1, 4, 0x2); // LAN freerun
    CHECK(!handler_Leave_Decision(0x1, 0));
    // without the mod and its plugin, the game's own way online too
    set_plugin(false);
    game_word = Online_Custom;
    CHECK(!handler_Leave_Decision(0x100, 0));
    game_word = Online_Public;
    CHECK(handler_Leave_Decision(0x201, 0));
    game_word = word_of(2, 4, 0x2);
    CHECK(!handler_Leave_Decision(0x1, 0));
}

// ---- picking the new host

static void test_pick_rule(void) {
    // when the rule applies at all
    CHECK(aae_hostmig_pick_applies(Online_Custom, 1, -1, true));
    CHECK(!aae_hostmig_pick_applies(Online_Public, 1, -1, true));
    CHECK(!aae_hostmig_pick_applies(word_of(1, 1, 0x2), 1, -1, true)); // LAN custom game
    CHECK(!aae_hostmig_pick_applies(word_of(2, 5, 0x2), 1, -1, true));
    CHECK(!aae_hostmig_pick_applies(Online_Custom, 2, -1, true));
    CHECK(!aae_hostmig_pick_applies(Online_Custom, 4, -1, true));
    CHECK(!aae_hostmig_pick_applies(Online_Custom, 0, -1, true));
    CHECK(!aae_hostmig_pick_applies(Online_Custom, 1, 0, true) && !aae_hostmig_pick_applies(Online_Custom, 1, 3, true));
    CHECK(!aae_hostmig_pick_applies(Online_Custom, 1, -2, true));
    CHECK(!aae_hostmig_pick_applies(Online_Custom, 1, -1, false));

    // 2, 3 and 4 players who all answered with 0: each counts as 2 while the game picks, then 0 again
    static const int two[] = { 3, 7, -1 }, three[] = { 0, 9, 17, -1 }, four[] = { 1, 2, 4, 16, -1 };
    static const int *const groups[] = { two, three, four };
    for (int g = 0; g < 3; g++) {
        set_players(groups[g], 0);
        aae_hostmig_client_t before[Hostmig_Clients];
        memcpy(before, game_clients, sizeof(before));
        uint32_t expected = 0;
        for (const int *at = groups[g]; *at >= 0; at++)
            expected |= 1u << *at;
        uint32_t marked = aae_hostmig_pick_mark(game_clients);
        CHECK(marked == expected);
        for (int i = 0; i < Hostmig_Clients; i++) {
            CHECK(game_clients[i].rating == ((expected >> i & 1) != 0 ? 2 : before[i].rating));
            CHECK(game_clients[i].state == before[i].state && game_clients[i].sent == before[i].sent &&
                  game_clients[i].xuid == before[i].xuid);
        }
        aae_hostmig_pick_restore(game_clients, marked);
        CHECK(memcmp(before, game_clients, sizeof(before)) == 0);
    }

    // nothing changes: too few or too many players, one who hasn't answered, answered with a rating, was never sent
    // anything, or is in another state
    static const int none[] = { -1 }, one[] = { 5, -1 }, five[] = { 0, 1, 2, 3, 4, -1 };
    static const int *const not_these[] = { none, one, five };
    for (int g = 0; g < 3; g++) {
        set_players(not_these[g], 0);
        aae_hostmig_client_t before[Hostmig_Clients];
        memcpy(before, game_clients, sizeof(before));
        CHECK(aae_hostmig_pick_mark(game_clients) == 0);
        CHECK(memcmp(before, game_clients, sizeof(before)) == 0);
    }
    for (int change = 0; change < 5; change++) {
        set_players(three, 0);
        aae_hostmig_client_t *odd = &game_clients[change == 4 ? 0 : 17];
        if (change == 0)
            odd->state = 1;
        else if (change == 1)
            odd->rating = 3;
        else if (change == 2)
            odd->sent = 0;
        else if (change == 3)
            odd->state = 3;
        else
            odd->rating = -1;
        aae_hostmig_client_t before[Hostmig_Clients];
        memcpy(before, game_clients, sizeof(before));
        CHECK(aae_hostmig_pick_mark(game_clients) == 0);
        CHECK(memcmp(before, game_clients, sizeof(before)) == 0);
    }
    // a player in the migration with a count of messages above 1 is still one of them; an entry not in it with
    // rating 0 isn't
    set_players(two, 0);
    game_clients[3].sent = 4;
    game_clients[12].rating = 0;
    game_clients[13].rating = 0;
    game_clients[13].sent = 0;
    CHECK(aae_hostmig_pick_mark(game_clients) == (1u << 3 | 1u << 7));
    CHECK(game_clients[12].rating == 0 && game_clients[13].rating == 0);

    // putting back: only those marked, and only while they still have the 2
    set_players(three, 0);
    game_clients[0].rating = 2;
    game_clients[9].rating = 2;
    game_clients[17].rating = 6; // changed meanwhile
    game_clients[12].rating = 2; // not marked
    aae_hostmig_pick_restore(game_clients, 1u << 0 | 1u << 9 | 1u << 17);
    CHECK(game_clients[0].rating == 0 && game_clients[9].rating == 0 && game_clients[17].rating == 6 &&
          game_clients[12].rating == 2);
    aae_hostmig_pick_restore(game_clients, 0);
    CHECK(game_clients[12].rating == 2 && game_clients[17].rating == 6);
}

// the hook around the game's pick
static void test_pick_hook(void) {
    session_mode = &game_word;
    migration_state = &game_state;
    migration_clients = game_clients;
    new_host = &game_new_host;
    migration_in_progress = &game_in_progress;
    orig_PickNewHost = game_pick_new_host;
    static const int three[] = { 2, 5, 11, -1 };

    // the plugin's case: an online custom game where everyone answered 0; the first of them becomes host
    set_plugin(true);
    game_word = Online_Custom;
    game_state = 1;
    game_new_host = -1;
    game_in_progress = 1;
    set_players(three, 0);
    pick_calls = 0;
    fake_PickNewHost();
    CHECK(pick_calls == 1 && game_new_host == 2);
    CHECK(ratings_seen[2] == 2 && ratings_seen[5] == 2 && ratings_seen[11] == 2 && ratings_seen[0] == 0x5a5a);
    CHECK(game_clients[2].rating == 0 && game_clients[5].rating == 0 && game_clients[11].rating == 0);
    CHECK(game_clients[2].sent == 0 && game_clients[5].sent == 1);

    // anything else: the game picks from the ratings as they are, here nobody
    for (int change = 0; change < 6; change++) {
        set_plugin(change != 0);
        game_word = change == 1 ? Online_Public : change == 2 ? word_of(1, 1, 0x2) : Online_Custom;
        game_state = change == 3 ? 2 : 1;
        game_new_host = change == 4 ? 5 : -1; // the pick's other caller: its new host had dropped out
        game_in_progress = change == 5 ? 0 : 1;
        set_players(three, 0);
        pick_calls = 0;
        fake_PickNewHost();
        CHECK(pick_calls == 1 && game_new_host == -1 && ratings_seen[2] == 0 && ratings_seen[11] == 0);
        CHECK(game_clients[2].rating == 0 && game_clients[5].rating == 0);
    }
    // one answered with a rating of its own: the game picks from them as they are
    set_plugin(true);
    game_word = Online_Custom;
    game_state = 1;
    game_new_host = -1;
    set_players(three, 0);
    game_clients[5].rating = 9;
    game_clients[11].rating = 0;
    game_clients[2].rating = 0;
    fake_PickNewHost();
    CHECK(game_new_host == 5 && ratings_seen[2] == 0 && ratings_seen[11] == 0 && game_clients[2].rating == 0);
}

// ---- the "migrate to" message through the party lobby

static void test_resend_rule(void) {
    const uint64_t xuid = 0x110000100000007ull, list[3] = { 1, 2, 3 };
    aae_hostmig_lobby_t party = { .state = 2 };
    for (int i = 0; i < Hostmig_Clients; i++)
        party.members[i] = 0x2200000000000000ull + (uint64_t)i;
    party.members[17] = xuid;
    CHECK(aae_hostmig_resend_by_party(xuid, list, 3, 1, Online_Custom, 4, &party));
    CHECK(aae_hostmig_resend_by_party(xuid, list, -1, 1, Online_Custom, 4, &party)); // the plugin only asks for not 0
    party.members[17] = 0;
    party.members[0] = xuid;
    CHECK(aae_hostmig_resend_by_party(xuid, list, 1, 1, Online_Custom, 4, &party));
    // each condition in turn
    CHECK(!aae_hostmig_resend_by_party(0, list, 3, 1, Online_Custom, 4, &party));
    CHECK(!aae_hostmig_resend_by_party(xuid, NULL, 3, 1, Online_Custom, 4, &party));
    CHECK(!aae_hostmig_resend_by_party(xuid, list, 0, 1, Online_Custom, 4, &party));
    CHECK(!aae_hostmig_resend_by_party(xuid, list, 3, 0, Online_Custom, 4, &party)); // sent through the party already
    CHECK(!aae_hostmig_resend_by_party(xuid, list, 3, 2, Online_Custom, 4, &party));
    CHECK(!aae_hostmig_resend_by_party(xuid, list, 3, 1, Online_Public, 4, &party));
    CHECK(!aae_hostmig_resend_by_party(xuid, list, 3, 1, word_of(1, 1, 0x2), 4, &party));
    CHECK(!aae_hostmig_resend_by_party(xuid, list, 3, 1, Online_Custom, 3, &party));
    CHECK(!aae_hostmig_resend_by_party(xuid, list, 3, 1, Online_Custom, 1, &party));
    party.state = 1;
    CHECK(!aae_hostmig_resend_by_party(xuid, list, 3, 1, Online_Custom, 4, &party));
    party.state = 3;
    CHECK(!aae_hostmig_resend_by_party(xuid, list, 3, 1, Online_Custom, 4, &party));
    party.state = 2;
    party.members[0] = xuid + 1; // the new host isn't in the party
    CHECK(!aae_hostmig_resend_by_party(xuid, list, 3, 1, Online_Custom, 4, &party));
}

// the hook around the game's message: everything handed on as it came
static void test_resend_hook(void) {
    session_mode = &game_word;
    migration_state = &game_state;
    party_lobby = game_lobbies;
    orig_SendToNewHost = game_send;
    const uint64_t xuid = 0x110000100000005ull, list[2] = { 11, 12 };
    const uint64_t controller = 0xdead000000000000ull, type = 0xbeef000000000001ull, count = 0xf00d000000000002ull;

    // the plugin's case: the game lobby can't send it, the party lobby has the new host
    set_plugin(true);
    game_word = Online_Custom;
    game_state = 4;
    set_party(2, 6, xuid);
    gets_through[1] = false;
    gets_through[0] = true;
    send_calls = 0;
    CHECK(fake_SendToNewHost(controller, xuid, type, list, count));
    CHECK(send_calls == 2);
    CHECK(sends[0].controller == controller && sends[0].xuid == xuid && sends[0].lobby_type == type &&
          sends[0].list == list && sends[0].count == count);
    CHECK(sends[1].controller == controller && sends[1].xuid == xuid && sends[1].lobby_type == 0 &&
          sends[1].list == list && sends[1].count == count);
    // ... and what the party lobby answers is the answer
    gets_through[0] = false;
    send_calls = 0;
    CHECK(!fake_SendToNewHost(controller, xuid, type, list, count) && send_calls == 2);

    // the game lobby sent it: once
    gets_through[1] = true;
    gets_through[0] = true;
    send_calls = 0;
    CHECK(fake_SendToNewHost(controller, xuid, type, list, count) && send_calls == 1);

    // the rule says no: once, and false
    gets_through[1] = false;
    for (int change = 0; change < 5; change++) {
        set_plugin(change != 0);
        game_word = change == 1 ? Online_Public : Online_Custom;
        game_state = change == 2 ? 3 : 4;
        set_party(change == 3 ? 1 : 2, change == 4 ? -1 : 6, xuid);
        send_calls = 0;
        CHECK(!fake_SendToNewHost(controller, xuid, type, list, count) && send_calls == 1);
    }
    set_plugin(true);
    game_word = Online_Custom;
    game_state = 4;
    set_party(2, 6, xuid);
    gets_through[0] = false;
    send_calls = 0;
    CHECK(!fake_SendToNewHost(controller, xuid, 0, list, count) && send_calls == 1); // through the party already
    CHECK(!fake_SendToNewHost(controller, xuid, type, list, 0xf00d000000000000ull) && send_calls == 2); // nobody
}

// ---- init: the places, and nothing at all without the pre-patched executable's stubs for all three

static void test_init(void) {
    const uint64_t base = 0x7f0000000000ull; // never read
    setenv("BO3MACFIX_HOSTMIGDEBUG", "1", 1);
    have_stubs = true;
    aae_hostmig_init(base, 0);
    CHECK(hooks == 0);
    static const uint32_t places[] = { ADDR_HostMigrationStart_Decision, ADDR_HostMigration_PickNewHost,
                                       ADDR_HostMigration_SendToNewHost };
    for (int i = 0; i < 3; i++) {
        stubs_missing_for = (int)places[i];
        aae_hostmig_init(base, 1);
        CHECK(hooks == 0);
    }
    stubs_missing_for = -1;
    have_stubs = false;
    aae_hostmig_init(base, 1);
    CHECK(hooks == 0 && session_mode == NULL && debug == 0 && loaded_mod == NULL);

    have_stubs = true;
    aae_hostmig_init(base, 1);
    CHECK(hooks == 3);
    CHECK(was_hooked(base, ADDR_HostMigrationStart_Decision, (void *)handler_Leave_Decision, &orig_Leave_Decision) &&
          other_Leave_Decision == (void *)(base + ADDR_HostMigrationStart_Migrate));
    CHECK(was_hooked(base, ADDR_HostMigration_PickNewHost, (void *)fake_PickNewHost, &orig_PickNewHost));
    CHECK(was_hooked(base, ADDR_HostMigration_SendToNewHost, (void *)fake_SendToNewHost, &orig_SendToNewHost));
    CHECK(session_mode == (const uint32_t *)(base + ADDR_SessionMode) &&
          migration_state == (const int32_t *)(base + ADDR_HostMigration) &&
          (uint8_t *)migration_clients == (uint8_t *)(base + ADDR_HostMigration + 0x8) &&
          new_host == (const int32_t *)(base + ADDR_HostMigration + 0x3c8) &&
          migration_in_progress == (const uint8_t *)(base + ADDR_HostMigration + 0x3e4) &&
          party_lobby == (const uint8_t *)(base + ADDR_LobbySessions) &&
          loaded_mod == (const char *)(base + ADDR_Mods_LoadedName));
    CHECK(debug == 1);
    // the migration's own checks come after the detoured instructions (9 bytes), the second test (call 5, test 2,
    // je 2) and the end of the match (pop rbp 1, jmp 5)
    CHECK(ADDR_HostMigrationStart_Migrate == ADDR_HostMigrationStart_Decision + 9 + 5 + 2 + 2 + 1 + 5);
}

int main(void) {
    char template[] = "/tmp/bo3macfix-hostmig-test-XXXXXX";
    if (mkdtemp(template) == NULL)
        return 2;
    snprintf(mod_folder, sizeof(mod_folder), "%s/mods/2631943123", template);
    char folder[4096 + 64];
    snprintf(folder, sizeof(folder), "%s/T7Overcharged/plugins", mod_folder);
    make_folder(folder);
    snprintf(plugin_file, sizeof(plugin_file), "%s/%s", mod_folder, Plugin_Path);
    plugin_now_ms = test_now;

    test_init(); // first: it points the code at the game, and the other tests at the made-up one
    loaded_mod = loaded_name;
    test_session();
    test_leave();
    test_leave_detour();
    test_pick_rule();
    test_pick_hook();
    test_resend_rule();
    test_resend_hook();

    unlink(plugin_file);
    char command[4096 + 32];
    snprintf(command, sizeof(command), "rm -rf '%s'", template);
    if (system(command) != 0)
        failures++;
    if (failures != 0) {
        printf("test_aae_hostmig: %d failed\n", failures);
        return 1;
    }
    printf("test_aae_hostmig: all passed\n");
    return 0;
}
