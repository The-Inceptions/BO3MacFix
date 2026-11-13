// All-around Enhancement 4's host migration, as its Windows plugin does it (AAEHostMigration). See aae_hostmig.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void aae_hostmig_init(uint64_t game_base_address, int enabled);

// ---- the session: the game's word for what kind of game is played

// online: the network mode (bits 6 to 9) is 2
bool aae_hostmig_online(uint32_t session_mode);
// the game mode (bits 14 to 17, with a sign, as the game's own test reads it): 1 is a custom game, 4 freerun
int aae_hostmig_game_mode(uint32_t session_mode);

// ---- what the host's leave decision does (the game's command hostmigration_start)

typedef enum {
    Hostmig_End_Match, // the match ends for everyone
    Hostmig_Migrate,   // on to the migration: when a server runs and none is in progress, another player becomes host
} aae_hostmig_leave_t;

// The game's own decision: migrate when its test for that says yes (may_migrate: no mod is loaded, the game is online
// and isn't a custom game, a theater or a manual playlist) and the game isn't a freerun one.
aae_hostmig_leave_t aae_hostmig_leave_own(bool may_migrate, uint32_t session_mode);
// The plugin's: an online game goes to the migration whatever the test says; any other the game's own way.
aae_hostmig_leave_t aae_hostmig_leave(bool may_migrate, uint32_t session_mode);

// ---- the migration: what the game keeps for each player it hands the game over to

// one player's entry, as the game has it (18 of them)
typedef struct {
    int32_t state;    // 0 not part of it, 1 asked to rate itself, 2 has answered
    int32_t unused;
    uint64_t xuid;
    int32_t sent;     // how many messages it was sent; at 5 the game drops it
    int32_t deadline; // when its answer is due
    int32_t rating;   // how well it can host, from its answer; 0 when it can't reach everybody
    int32_t resend;   // when the new host is told again about players still on their way
} aae_hostmig_client_t;
#define Hostmig_Clients 18
#define Hostmig_State_Asked 1  // the migration's state: the players were asked to rate themselves
#define Hostmig_State_Moving 4 // the new host is chosen, and the players still on their way are told where it is

// Whether the plugin's rule for picking a new host applies at all: an online custom game whose migration has asked
// the players to rate themselves (state 1), hasn't chosen a new host yet (-1) and is in progress.
bool aae_hostmig_pick_applies(uint32_t session_mode, int32_t migration_state, int32_t new_host, bool in_progress);
// Before the game picks: when every player in the migration has answered with rating 0 (and was sent a message), and
// there are 2 to 4 of them, each gets rating 2, so that the game's pick (ratings above 1, the first one on a tie) takes
// the first of them. Returns which ones were changed, one bit each (0 for none).
uint32_t aae_hostmig_pick_mark(aae_hostmig_client_t clients[Hostmig_Clients]);
// After the pick: those still at 2 get their 0 back.
void aae_hostmig_pick_restore(aae_hostmig_client_t clients[Hostmig_Clients], uint32_t marked);

// ---- telling the players where the new host is

// What the plugin wants to know of the party lobby: its state (+0x40) and its members' xuids.
typedef struct {
    int32_t state;
    uint64_t members[Hostmig_Clients];
} aae_hostmig_lobby_t;
#define Hostmig_Lobby_Game 1  // the game's lobby
#define Hostmig_Lobby_Party 0 // the private one the players came in as a party

// Whether a "migrate to" message the game lobby couldn't send is to be sent through the party lobby instead. The
// message tells the players still on their way (a list of them) which player is the new host, and goes through a
// lobby the new host is a member of. Yes when it went through the game lobby, about a new host (xuid) to players
// listed, in an online custom game whose new host is chosen (state 4), and the party lobby is up (state 2) with the
// new host among its members.
bool aae_hostmig_resend_by_party(uint64_t xuid, const void *list, int32_t count, int32_t lobby_type,
                                 uint32_t session_mode, int32_t migration_state, const aae_hostmig_lobby_t *party);
