// T7Overcharged's script function anim_event(entnum, event): All-around Enhancement's inspect and first raise on the
// player's body.
//
// AAE plays an animation on the gun when a player inspects it (the reload key with a full magazine, sprint and reload
// on a gamepad, or by itself after 30 seconds of standing still with a weapon that has an inspect animation) and after
// its ammo bag refills it. Its server scripts tell every client "lowready,<entity>" or
// "firstraise,<entity>" (the client system "deadshot_keyline"), and its client script then starts the animation in
// first person (startweaponanim, weapon_anim.h) and asks T7Overcharged's anim_event for the matching event on that
// player's body: 101 FLOURISH, the inspect in third person, or 15 FIRSTRAISEWEAPON. (A third branch, for
// "reloadempty" and 17 RELOAD_EMPTY, is sent by no script of AAE 4.1.7.) The body is what the third person view and
// the other players see. On the Mac anim_event did nothing, so there it stood still.
//
// T7Overcharged's handler (T7O 0x180071b30), all of it:
//     entity = GetEntity({ number = Scr_GetInt(inst, 1), class 0 })
//     if (entity != NULL && entity->client != NULL && entity->s.eType == 1)
//         BG_AnimScriptEvent(&pmove[entity->client->ps.clientNum], Scr_GetInt(inst, 2), false, true,
//                            G_GetRandomSeed());
// and nothing goes back to the script. The game makes the same call itself when a player gets off something
// (0x1012d1cee). BG_AnimScriptEvent looks up the player's anim script for the event in the player's state (weapon,
// stance, movement), puts its animation in the player state and gives back how long it lasts, or -1 when the anim
// script has nothing for it. The server sends the player state to everyone: the third person view and the other
// players show the animation.
//
// It acts on the server's entities and player states, whichever script calls it. From a client script, as AAE's, it
// only changes something where this machine runs the server (solo, or the host of a co-op game), and it does so
// while the server's own thread moves the players, as on Windows. BG_AnimScriptEvent takes the anim scripts from the
// calling thread's game state, here the client's, which on the host has loaded the same map's as the server.
//
// What is different here, each for a case T7Overcharged leaves to chance:
// - with no server on this machine nothing is done (T7Overcharged would read the entities of a server that doesn't
//   run: zeroes, or those of a game hosted earlier);
// - the number has to be one of the game's 1024 entities (it would read past them);
// - the client's number has to be one of the 18 slots of the server's moves, and the slot has to hold a player state
//   (the game would crash without one; a slot gets it with its client's first move).
// The parameters are read with the game's own Scr_GetInt, as T7Overcharged reads them: a missing one, or one that
// isn't a number, is a script error.
#include <stdio.h>
#include <stdlib.h>
#include "offsets.h"
#include "t7o_anim_event.h"

#define Max_Entities 1024     // the game's entities (0x100d49551: "%i is not a valid entity number" from 0x400 up)
#define Entity_Size 0x4f8     // gentity_t
#define Entity_Type 0x1a0     // s.eType, an int (the game writes it whole, 0x100f622ae)
#define Entity_Type_Player 1  // ET_PLAYER
#define Entity_Client 0x250   // gclient_t *: the player state first in it, the client's number first in that
#define Max_Clients 18        // the server's moves: the game's next variable begins where an 18th would end
#define Pmove_Size 0x1660     // pmove_t, with the pointer to its player state first
#define Event_Names 102       // the anim script events' names (ADDR_animEventTypesStr), for the debug line
#define Event_Name_Size 16
// the body's animations in the player state, for the debug line (T7Overcharged's playerState_t; the game's
// BG_PlayAnim, 0x1012def8b, reads and writes them there)
#define State_Legs_Timer 0x11c
#define State_Torso_Timer 0x120
#define State_Legs_Anim 0x124  // 16 bits each: the animation, and a bit that flips when it starts again
#define State_Torso_Anim 0x126

// the game's
static int (*Scr_GetInt)(int inst, unsigned int index);
static int (*BG_AnimScriptEvent)(uint8_t *pmove, int event, int is_continue, int force, int *random_seed);
static int *(*G_GetRandomSeed)(void);
static const int32_t *server_state; // sv.state, 0 while this machine runs no server
static uint8_t *const *entities;    // g_entities
static uint8_t *moves;              // the server's pmove_t for each client
static const uint8_t *event_names;  // a pointer to each event's name, every 16 bytes

// BO3MACFIX_ANIMEVENTDEBUG=1 in the environment: a line for every call (they come with a player's inspect, rarely),
// with the body's animations before and after
static int debug;

typedef struct {
    uint16_t legs, torso;
    int32_t legs_timer, torso_timer;
} body_t;

static body_t body_of(const uint8_t *player_state) {
    return (body_t){ *(const uint16_t *)(player_state + State_Legs_Anim),
                     *(const uint16_t *)(player_state + State_Torso_Anim),
                     *(const int32_t *)(player_state + State_Legs_Timer),
                     *(const int32_t *)(player_state + State_Torso_Timer) };
}

bool t7o_anim_event_is_player(const uint8_t *entity) {
    return *(void *const *)(entity + Entity_Client) != NULL &&
           *(const int32_t *)(entity + Entity_Type) == Entity_Type_Player;
}

static const char *event_name(int event) {
    if (event < 0 || event >= Event_Names)
        return "?";
    const char *name = *(const char *const *)(event_names + (size_t)event * Event_Name_Size);
    return name != NULL ? name : "?";
}

// before and after are only given for one that was played; own: the move's player state is the client's
static void say(int number, int event, anim_event_outcome_t outcome, int given, const body_t *before,
                const body_t *after, bool own) {
    static const char *const what[] = {
        [AnimEvent_NoServer] = "this machine runs no server, nothing done",
        [AnimEvent_NotAnEntity] = "no such entity, nothing done",
        [AnimEvent_NotAPlayer] = "not a player, nothing done",
        [AnimEvent_NotMoving] = "the server has no player state for its client yet, nothing done",
        [AnimEvent_Played] = "played",
    };
    if (outcome == AnimEvent_Played) {
        bool changed = before->legs != after->legs || before->torso != after->torso ||
                       before->legs_timer != after->legs_timer || before->torso_timer != after->torso_timer;
        printf("anim_event: entity %d, event %d %s: %s, the game gave %d%s; torso 0x%04x -> 0x%04x (timer %d -> "
               "%d), legs 0x%04x -> 0x%04x (timer %d -> %d): the body's animation %s; the move's player state is "
               "%s\n",
               number, event, event_name(event), what[outcome], given,
               given < 0 ? " (its anim script has nothing for this event now)" : "", before->torso, after->torso,
               before->torso_timer, after->torso_timer, before->legs, after->legs, before->legs_timer,
               after->legs_timer, changed ? "changed" : "stayed", own ? "the client's" : "another one");
    } else if (outcome == AnimEvent_NotMoving)
        printf("anim_event: entity %d, event %d %s: %s\n", number, event, event_name(event), what[outcome]);
    else
        printf("anim_event: entity %d: %s\n", number, what[outcome]);
}

anim_event_outcome_t t7o_anim_event(int inst) {
    int number = Scr_GetInt(inst, 1);
    int event = 0, given = 0;
    anim_event_outcome_t outcome;
    const uint8_t *entity = NULL;
    body_t before = { 0 }, after = { 0 };
    bool own = false;
    if (*server_state == 0)
        outcome = AnimEvent_NoServer;
    else if ((unsigned int)number >= Max_Entities)
        outcome = AnimEvent_NotAnEntity;
    else if (entity = *entities + (size_t)number * Entity_Size, !t7o_anim_event_is_player(entity))
        outcome = AnimEvent_NotAPlayer;
    else {
        event = Scr_GetInt(inst, 2);
        int32_t client = **(const int32_t *const *)(entity + Entity_Client);
        uint8_t *move = (unsigned int)client < Max_Clients ? moves + (size_t)client * Pmove_Size : NULL;
        if (move == NULL || *(void **)move == NULL)
            outcome = AnimEvent_NotMoving;
        else {
            const uint8_t *player_state = *(const uint8_t **)move;
            if (debug)
                before = body_of(player_state), own = player_state == *(const uint8_t *const *)(entity + Entity_Client);
            given = BG_AnimScriptEvent(move, event, 0, 1, G_GetRandomSeed());
            outcome = AnimEvent_Played;
            if (debug)
                after = body_of(player_state);
        }
    }
    if (debug)
        say(number, event, outcome, given, &before, &after, own);
    return outcome;
}

void t7o_anim_event_init(uint64_t game_base_address) {
    const char *env = getenv("BO3MACFIX_ANIMEVENTDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    Scr_GetInt = (void *)(game_base_address + ADDR_Scr_GetInt);
    BG_AnimScriptEvent = (void *)(game_base_address + ADDR_BG_AnimScriptEvent);
    G_GetRandomSeed = (void *)(game_base_address + ADDR_G_GetRandomSeed);
    server_state = (const int32_t *)(game_base_address + ADDR_sv_state);
    entities = (uint8_t *const *)(game_base_address + ADDR_g_entities_Ptr);
    moves = (uint8_t *)(game_base_address + ADDR_pmove);
    event_names = (const uint8_t *)(game_base_address + ADDR_animEventTypesStr);
}
