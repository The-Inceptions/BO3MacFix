// T7Overcharged's script function anim_event(entnum, event): All-around Enhancement's inspect and first raise on the
// player's body, as other players and the third person view see it (t7o_anim_event.c)
#ifndef T7O_ANIM_EVENT_H
#define T7O_ANIM_EVENT_H

#include <stdbool.h>
#include <stdint.h>

// what became of a call, for the debug line and the tests
typedef enum {
    AnimEvent_NoServer,    // this machine runs no server, so there is no player state to play it on
    AnimEvent_NotAnEntity, // the number is none of the game's 1024 entities
    AnimEvent_NotAPlayer,  // T7Overcharged's own test: the entity has no client or isn't a player
    AnimEvent_NotMoving,   // the client's slot in the server's moves isn't one, or holds no player state yet
    AnimEvent_Played,      // the game's BG_AnimScriptEvent was asked to play it
} anim_event_outcome_t;

// T7Overcharged's test of the entity it got: it has a client, and its type is 1, a player (read as 32 bits)
bool t7o_anim_event_is_player(const uint8_t *entity);

void t7o_anim_event_init(uint64_t game_base_address);
// anim_event from a script, through T7Overcharged's isprofilebuild (t7overcharged.c): the script's parameter 1 is the
// entity's number and 2 the event. Like T7Overcharged it gives the script nothing back
anim_event_outcome_t t7o_anim_event(int inst);

#endif
