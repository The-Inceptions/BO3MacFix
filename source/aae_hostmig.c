// All-around Enhancement 4's host migration: the plugin's host_migration component.
//
// When the host of an online game leaves, the game either hands the game to another player (host migration: the
// players stay in the match, one of them becomes the host) or ends the match for everyone. That is decided by the
// game's server command hostmigration_start, which the pause menu runs for a host who quits a ranked online game
// (in zombies once the game lobby has enough players). The command asks the game's test for whether it may migrate,
// and that test says no as soon as any mod is loaded (also in a custom game, a theater or a manual playlist, and
// offline): with All-around Enhancement every host who leaves ends the match for everyone.
//
// The plugin changes that, and this does the same while the mod with the plugin is loaded:
// - The decision: in an online game it goes to the migration whatever the test says (the migration still needs a
//   running server and none in progress, and ends the match itself when there is nobody to hand the game to). Any
//   other game keeps the game's own decision.
// - Picking the new host, in an online custom game: each player rates how well it can host, and the game takes the
//   best rating above 1. A player who can't reach every other player gets 0, and in a custom game the players are in
//   the party lobby rather than the game's, so every rating may be 0 and nobody is taken: the match ends. When every
//   player in the migration answered with 0 and there are 2 to 4 of them, the plugin gives each a 2 just while the
//   game picks, so the first of them becomes host, and puts the 0s back.
// - Telling the players where the new host is, in an online custom game: the game sends that message through the game
//   lobby, which needs the new host as a member. When that fails and the new host is a member of the party lobby, the
//   plugin sends it through the party lobby.
//
// On Windows the plugin redirects four calls of the game: the decision's two calls of its tests (each answer
// changed), the migration's jump to its pick of the new host (wrapped), and its one call of the function that sends
// the message (wrapped). On the Mac the decision is one place behind its first test's call, where a detour (from the
// pre-patched executable only) works out the whole decision, the game's own second test included (a plain read of
// the session's game mode, freerun or not), and sends the game on to the migration or lets it end the match. The pick
// and the send are hooked as functions: the pick has one other caller, which only comes when a new host had been
// chosen, so the plugin's rule never applies there; the send has no other caller.
//
// Only the leaving host's machine does any of this: it matters when a Mac player hosts.
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aae_hostmig.h"
#include "aae_plugin.h"
#include "detour.h"
#include "dobby.h"
#include "offsets.h"
#include "prepatch.h"

#define Online_Mask 0x3c0        // the session word's network mode, 2 for online
#define Online_Value 0x80
#define Custom_Game_Mask 0x3c000 // its game mode, as the plugin tests it: 1 for a custom game (MATCHMAKING_MANUAL)
#define Custom_Game_Value 0x4000
#define Game_Mode_Freerun 4      // the game mode the decision's own second test ends the match in
#define Hostmig_State_Answered 2 // a player's entry: it has rated itself
#define Pick_Least_Players 2     // the plugin's rule for picking is for 2 to 4 players in the migration
#define Pick_Most_Players 4
#define Pick_Rating 2            // what each of them gets: the game takes the best rating above 1, the first on a tie
#define Lobby_Up 2               // a lobby session's state when it can migrate (the game's own test for that)

_Static_assert(sizeof(aae_hostmig_client_t) == 0x20, "the game's entries are 0x20 bytes apart");
_Static_assert(offsetof(aae_hostmig_client_t, xuid) == 0x8 && offsetof(aae_hostmig_client_t, sent) == 0x10 &&
                   offsetof(aae_hostmig_client_t, rating) == 0x18 && offsetof(aae_hostmig_client_t, resend) == 0x1c,
               "the game's entry");

bool aae_hostmig_online(uint32_t session_mode) {
    return (session_mode & Online_Mask) == Online_Value;
}

int aae_hostmig_game_mode(uint32_t session_mode) {
    return (int32_t)(session_mode << 14) >> 28;
}

// the plugin's own test for a custom game: the game mode's four bits are 1
static bool custom_game(uint32_t session_mode) {
    return (session_mode & Custom_Game_Mask) == Custom_Game_Value;
}

aae_hostmig_leave_t aae_hostmig_leave_own(bool may_migrate, uint32_t session_mode) {
    if (!may_migrate || aae_hostmig_game_mode(session_mode) == Game_Mode_Freerun)
        return Hostmig_End_Match;
    return Hostmig_Migrate;
}

aae_hostmig_leave_t aae_hostmig_leave(bool may_migrate, uint32_t session_mode) {
    bool online = aae_hostmig_online(session_mode);
    // the plugin's answers to the decision's two questions: "may it migrate" is yes online, "is it freerun" no
    bool may = may_migrate || online;
    bool freerun = aae_hostmig_game_mode(session_mode) == Game_Mode_Freerun && !online;
    return !may || freerun ? Hostmig_End_Match : Hostmig_Migrate;
}

bool aae_hostmig_pick_applies(uint32_t session_mode, int32_t migration_state, int32_t new_host, bool in_progress) {
    return aae_hostmig_online(session_mode) && custom_game(session_mode) && migration_state == Hostmig_State_Asked &&
           new_host == -1 && in_progress;
}

uint32_t aae_hostmig_pick_mark(aae_hostmig_client_t clients[Hostmig_Clients]) {
    int players = 0;
    for (int i = 0; i < Hostmig_Clients; i++) {
        const aae_hostmig_client_t *client = &clients[i];
        if (client->state == 0)
            continue;
        // anyone who hasn't answered yet, answered with a rating or was never sent anything: the game picks as it is
        if (client->state != Hostmig_State_Answered || client->rating != 0 || client->sent == 0)
            return 0;
        players++;
    }
    if (players < Pick_Least_Players || players > Pick_Most_Players)
        return 0;
    uint32_t marked = 0;
    for (int i = 0; i < Hostmig_Clients; i++) {
        if (clients[i].state == Hostmig_State_Answered && clients[i].rating == 0) {
            clients[i].rating = Pick_Rating;
            marked |= 1u << i;
        }
    }
    return marked;
}

void aae_hostmig_pick_restore(aae_hostmig_client_t clients[Hostmig_Clients], uint32_t marked) {
    for (int i = 0; i < Hostmig_Clients; i++)
        if ((marked >> i & 1) != 0 && clients[i].rating == Pick_Rating)
            clients[i].rating = 0;
}

bool aae_hostmig_resend_by_party(uint64_t xuid, const void *list, int32_t count, int32_t lobby_type,
                                 uint32_t session_mode, int32_t migration_state, const aae_hostmig_lobby_t *party) {
    if (xuid == 0 || list == NULL || count == 0 || lobby_type != Hostmig_Lobby_Game)
        return false;
    if (!aae_hostmig_online(session_mode) || !custom_game(session_mode) || migration_state != Hostmig_State_Moving)
        return false;
    // (the plugin looks at the party lobby only in this build of the Windows game; in its other one it sends anyway)
    if (party->state != Lobby_Up)
        return false;
    for (int i = 0; i < Hostmig_Clients; i++)
        if (party->members[i] == xuid)
            return true;
    return false;
}

// ---- the game's side
#define Migration_Clients 0x8      // the migration's entries for the players (after its int state)
#define Migration_NewHost 0x3c8    // int: the new host's entry
#define Migration_InProgress 0x3e4 // a byte
#define Lobby_State 0x40           // int in a lobby session
#define Lobby_Members 0xf8         // its members, each with its xuid first
#define Lobby_Member_Size 0x30

static int debug; // BO3MACFIX_HOSTMIGDEBUG=1: a line for each decision, pick and message sent again
static const uint32_t *session_mode;            // the game's word for the session
static const int32_t *migration_state;          // the migration's: its state,
static aae_hostmig_client_t *migration_clients; // its entries for the players,
static const int32_t *new_host;                 // the new host's entry, -1 for none yet,
static const uint8_t *migration_in_progress;    // and whether it is in progress
static const uint8_t *party_lobby;              // lobby session 0

static const char *leave_name(aae_hostmig_leave_t leave) {
    return leave == Hostmig_Migrate ? "migrate" : "end the match";
}

// "test al, al ; je <end the match> ; mov edi, 4" in hostmigration_start, right behind its call of the game's test
// for whether it may migrate, whose answer is in al. The rest of rax is left over from that function. When the
// handler says yes, the game goes on at the migration's own checks; otherwise it runs these instructions, with al 0
// from the handler, and ends the match.
detour_choice(Leave_Decision, "mov %rax, %rdi\n") {
    (void)second;
    bool may_migrate = (uint8_t)first != 0, loaded = aae_plugin_loaded_lately();
    uint32_t mode = *session_mode;
    aae_hostmig_leave_t own = aae_hostmig_leave_own(may_migrate, mode);
    aae_hostmig_leave_t leave = loaded ? aae_hostmig_leave(may_migrate, mode) : own;
    if (debug)
        printf("host migration: the host leaves (%s, game mode %d, the game's test says it %s migrate, the mod with "
               "the plugin is %s): %s, the game's own way: %s\n", aae_hostmig_online(mode) ? "online" : "not online",
               aae_hostmig_game_mode(mode), may_migrate ? "may" : "may not", loaded ? "loaded" : "not loaded",
               leave_name(leave), leave_name(own));
    return leave == Hostmig_Migrate;
}

// the game's pick of the new host: the best rating above 1 among the players in the migration (no arguments)
install_hook_name(PickNewHost, void, void) {
    bool applies = aae_plugin_loaded_lately() &&
                   aae_hostmig_pick_applies(*session_mode, *migration_state, *new_host, *migration_in_progress != 0);
    uint32_t marked = applies ? aae_hostmig_pick_mark(migration_clients) : 0;
    orig_PickNewHost();
    aae_hostmig_pick_restore(migration_clients, marked);
    if (debug)
        printf("host migration: picking the new host in a %s game: %s, the game picked entry %d\n",
               custom_game(*session_mode) ? "custom" : "non-custom",
               !applies       ? "the plugin's rule doesn't apply"
               : marked != 0 ? "the players' ratings were all 0, each counted as 2"
                             : "the ratings stay as they are",
               *new_host);
}

// The game's "migrate to" message: (controller, the new host's xuid, the lobby to send it through, the xuids of the
// players to tell, how many), true when it was sent. It goes through that lobby only when the new host is a member.
// The game's one call of it doesn't look at the answer, so the answer being in al alone is enough.
install_hook_name(SendToNewHost, bool, uint64_t controller, uint64_t xuid, uint64_t lobby_type, const uint64_t *list,
                  uint64_t count) {
    bool sent = orig_SendToNewHost(controller, xuid, lobby_type, list, count);
    if (sent || !aae_plugin_loaded_lately())
        return sent;
    aae_hostmig_lobby_t party;
    memcpy(&party.state, party_lobby + Lobby_State, sizeof(party.state));
    for (int i = 0; i < Hostmig_Clients; i++)
        memcpy(&party.members[i], party_lobby + Lobby_Members + i * Lobby_Member_Size, sizeof(party.members[i]));
    if (!aae_hostmig_resend_by_party(xuid, list, (int32_t)count, (int32_t)lobby_type, *session_mode, *migration_state,
                                     &party))
        return false;
    sent = orig_SendToNewHost(controller, xuid, Hostmig_Lobby_Party, list, count);
    if (debug) {
        static _Atomic int said = -1; // a line when the outcome changes: the game sends it again every 200 ms
        if (atomic_exchange(&said, sent) != (int)sent)
            printf("host migration: the game lobby couldn't tell %d player(s) where the new host is; through the party "
                   "lobby: %s\n", (int32_t)count, sent ? "sent" : "not sent either");
    }
    return sent;
}

void aae_hostmig_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    if (!prepatch_has_hook(ADDR_HostMigrationStart_Decision) || !prepatch_has_hook(ADDR_HostMigration_PickNewHost) ||
        !prepatch_has_hook(ADDR_HostMigration_SendToNewHost)) {
        printf("host migration: not running from the pre-patched executable: a host who leaves a game with "
               "All-around Enhancement ends it for everyone, as the game does with any mod\n");
        return;
    }
    aae_plugin_init(game_base_address);
    const char *env = getenv("BO3MACFIX_HOSTMIGDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    session_mode = (const uint32_t *)(game_base_address + ADDR_SessionMode);
    migration_state = (const int32_t *)(game_base_address + ADDR_HostMigration);
    migration_clients = (aae_hostmig_client_t *)(game_base_address + ADDR_HostMigration + Migration_Clients);
    new_host = (const int32_t *)(game_base_address + ADDR_HostMigration + Migration_NewHost);
    migration_in_progress = (const uint8_t *)(game_base_address + ADDR_HostMigration + Migration_InProgress);
    party_lobby = (const uint8_t *)(game_base_address + ADDR_LobbySessions);
    install_detour_Leave_Decision((void *)(game_base_address + ADDR_HostMigrationStart_Decision),
                                  (void *)(game_base_address + ADDR_HostMigrationStart_Migrate));
    install_hook_PickNewHost((void *)(game_base_address + ADDR_HostMigration_PickNewHost));
    install_hook_SendToNewHost((void *)(game_base_address + ADDR_HostMigration_SendToNewHost));
}
