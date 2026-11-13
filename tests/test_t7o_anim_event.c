// t7o_anim_event.c: T7Overcharged's anim_event, its tests of the entity and what it asks of the game, against a
// made-up game: its entities, the server's moves, Scr_GetInt (a script error ends the call, as the game's longjmp
// does), BG_AnimScriptEvent and G_GetRandomSeed.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/t7o_anim_event.c"

// ---- the made-up game
static uint8_t *game_entities;     // 1024 of 0x4f8 bytes
static uint8_t *game_moves;        // 18 of 0x1660 bytes
// what an entity's client points to: its player state first, which begins with the client's number
static uint8_t game_clients[20][0x200];
static uint8_t game_player_states[18][0x200]; // player states that are no entity's client's
static int32_t game_server_state;
static uint8_t game_event_names[102 * 16];
static int game_seed;

// the script's parameters (0 is the native's hash); asking for one there isn't is a script error
static struct {
    int params[4], count;
    unsigned int asked[8]; // which parameter, and from which instance, each time
    int instances[8], asks;
    jmp_buf error;
} script;
static int fake_Scr_GetInt(int inst, unsigned int index) {
    if (script.asks < 8)
        script.asked[script.asks] = index, script.instances[script.asks] = inst;
    script.asks++;
    if (index >= (unsigned int)script.count)
        longjmp(script.error, 1);
    return script.params[index];
}
static struct {
    int calls, event, is_continue, force, answer;
    uint8_t *move;
    int *seed;
    // what it does to the body's animations in the move's player state (legs, torso, their timers), when set
    bool plays;
    uint16_t legs, torso;
    int32_t legs_timer, torso_timer;
} played;
static int fake_BG_AnimScriptEvent(uint8_t *move, int event, int is_continue, int force, int *seed) {
    played.calls++;
    played.move = move, played.event = event, played.is_continue = is_continue, played.force = force;
    played.seed = seed;
    if (played.plays) {
        uint8_t *state;
        memcpy(&state, move, sizeof(state));
        memcpy(state + 0x124, &played.legs, 2);
        memcpy(state + 0x126, &played.torso, 2);
        memcpy(state + 0x11c, &played.legs_timer, 4);
        memcpy(state + 0x120, &played.torso_timer, 4);
    }
    return played.answer;
}
static int seeds_asked;
static int *fake_G_GetRandomSeed(void) {
    seeds_asked++;
    return &game_seed;
}

static uint8_t *entity(int number) {
    return game_entities + (size_t)number * 0x4f8;
}
static void make_player(int number, int client) {
    static int next;
    uint8_t *own = game_clients[next++ % 20]; // each its own client, never shared with another entity's
    memset(own, 0, sizeof(game_clients[0]));
    memset(entity(number), 0, 0x4f8);
    int32_t type = 1;
    memcpy(entity(number) + 0x1a0, &type, sizeof(type));
    int32_t client_number = client;
    memcpy(own, &client_number, sizeof(client_number));
    memcpy(entity(number) + 0x250, &own, sizeof(own));
}
// the server's move for a client, with a player state that is no entity's client's
static void give_move(int client) {
    void *state = game_player_states[client];
    memcpy(game_moves + (size_t)client * 0x1660, &state, sizeof(state));
}
// as the game has it after the client's first move: the move points at the entity's client's own player state
static void give_own_move(int number) {
    uint8_t *client;
    memcpy(&client, entity(number) + 0x250, sizeof(client));
    int32_t client_number;
    memcpy(&client_number, client, sizeof(client_number));
    memcpy(game_moves + (size_t)client_number * 0x1660, &client, sizeof(client));
}
// the body's animations in a player state
static void set_body(uint8_t *state, uint16_t legs, uint16_t torso, int32_t legs_timer, int32_t torso_timer) {
    memcpy(state + 0x124, &legs, 2);
    memcpy(state + 0x126, &torso, 2);
    memcpy(state + 0x11c, &legs_timer, 4);
    memcpy(state + 0x120, &torso_timer, 4);
}
static void new_game(void) {
    memset(game_entities, 0, 1024 * 0x4f8);
    memset(game_moves, 0, 18 * 0x1660);
    memset(game_clients, 0, sizeof(game_clients));
    game_server_state = 2;
    memset(&played, 0, sizeof(played));
    played.answer = 700;
    memset(game_player_states, 0, sizeof(game_player_states));
}

// anim_event(number, event) from the given script instance: false when it ended in a script error
static anim_event_outcome_t outcome;
static bool call(int inst, int count, int number, int event) {
    script.params[0] = 0x50ee1cfb, script.params[1] = number, script.params[2] = event;
    script.count = count, script.asks = 0;
    memset(script.asked, 0xff, sizeof(script.asked));
    memset(script.instances, 0xff, sizeof(script.instances));
    played.calls = 0, played.move = NULL, played.seed = NULL;
    seeds_asked = 0;
    outcome = (anim_event_outcome_t)-1;
    if (setjmp(script.error) != 0)
        return false;
    outcome = t7o_anim_event(inst);
    return true;
}

// the same with what it prints kept in said
static char said[4096];
static bool call_said(int inst, int count, int number, int event) {
    fflush(stdout);
    int saved = dup(1);
    FILE *file = tmpfile();
    dup2(fileno(file), 1);
    bool done = call(inst, count, number, event);
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    rewind(file);
    size_t length = fread(said, 1, sizeof(said) - 1, file);
    said[length] = '\0';
    fclose(file);
    return done;
}

static void test_is_player(void) {
    _Alignas(16) uint8_t one[0x4f8];
    memset(one, 0, sizeof(one));
    int32_t type = 1;
    memcpy(one + 0x1a0, &type, sizeof(type));
    CHECK(!t7o_anim_event_is_player(one)); // no client
    void *client = game_clients[0];
    memcpy(one + 0x250, &client, sizeof(client));
    CHECK(t7o_anim_event_is_player(one));
    const int32_t others[] = { 0, 2, 15, -1, 0x10001, 0x101, 0x1000001 };
    for (unsigned int i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        memcpy(one + 0x1a0, &others[i], sizeof(others[i]));
        CHECK(!t7o_anim_event_is_player(one)); // 0x10001: a 1 in its lower half isn't enough, the whole int counts
    }
    // the type is where T7Overcharged reads it, and nowhere else
    memset(one + 0x1a0, 0, 4);
    memcpy(one + 0x1a4, &type, sizeof(type));
    CHECK(!t7o_anim_event_is_player(one));
    memcpy(one + 0x19c, &type, sizeof(type));
    CHECK(!t7o_anim_event_is_player(one));
    // and the client
    memcpy(one + 0x1a0, &type, sizeof(type));
    memset(one + 0x250, 0, 8);
    memcpy(one + 0x258, &client, sizeof(client));
    CHECK(!t7o_anim_event_is_player(one));
    // a client pointer with only its high half set is still one
    uint64_t high = 0x100000000;
    memcpy(one + 0x250, &high, sizeof(high));
    CHECK(t7o_anim_event_is_player(one));
}

static void test_played(void) {
    new_game();
    make_player(0, 0);
    give_move(0);
    // AAE's FLOURISH for the host's player, from its client script (instance 1)
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_Played);
    CHECK(played.calls == 1 && played.move == game_moves && played.event == 101);
    CHECK(played.is_continue == 0 && played.force == 1); // not a continuation, and forced, as T7Overcharged asks
    CHECK(played.seed == &game_seed && seeds_asked == 1);
    CHECK(script.asks == 2 && script.asked[0] == 1 && script.asked[1] == 2);
    CHECK(script.instances[0] == 1 && script.instances[1] == 1); // from the calling script's instance
    // the same from the server's scripts
    CHECK(call(0, 3, 0, 15) && outcome == AnimEvent_Played && played.event == 15);
    CHECK(script.instances[0] == 0 && script.instances[1] == 0);
    // the move is the client's, by the client's own number, not the entity's
    make_player(7, 2);
    give_move(2);
    CHECK(call(1, 3, 7, 101) && outcome == AnimEvent_Played && played.move == game_moves + 2 * 0x1660);
    make_player(3, 17);
    give_move(17);
    CHECK(call(1, 3, 3, 15) && outcome == AnimEvent_Played && played.move == game_moves + 17 * 0x1660);
    // the entity is the one by that number: its neighbours don't count
    new_game();
    make_player(4, 1);
    make_player(6, 1);
    give_move(1);
    CHECK(call(1, 3, 5, 101) && outcome == AnimEvent_NotAPlayer && played.calls == 0);
    make_player(5, 1);
    CHECK(call(1, 3, 5, 101) && outcome == AnimEvent_Played);
    // the last of the game's entities is one
    make_player(1023, 3);
    give_move(3);
    CHECK(call(1, 3, 1023, 17) && outcome == AnimEvent_Played && played.move == game_moves + 3 * 0x1660 &&
          played.event == 17);
    // whatever the game answers, the call is done (its -1: no legs animation started, a torso one alone or none)
    played.answer = -1;
    CHECK(call(1, 3, 1023, 17) && outcome == AnimEvent_Played && played.calls == 1);
}

static void test_not_done(void) {
    new_game();
    make_player(0, 0);
    give_move(0);
    // no server on this machine: nothing more is read or asked
    game_server_state = 0;
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_NoServer);
    CHECK(played.calls == 0 && seeds_asked == 0 && script.asks == 1 && script.asked[0] == 1);
    game_server_state = 1; // while it loads, it runs
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_Played);
    game_server_state = -1;
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_Played);
    game_server_state = 2;
    // a number that is no entity's
    const int numbers[] = { 1024, -1, 4096, 0x7fffffff, (int)0x80000000 };
    for (unsigned int i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        CHECK(call(1, 3, numbers[i], 101) && outcome == AnimEvent_NotAnEntity);
        CHECK(played.calls == 0 && seeds_asked == 0 && script.asks == 1);
    }
    // T7Overcharged's tests: without a client, or not a player, the event isn't even read
    new_game();
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_NotAPlayer && script.asks == 1 && played.calls == 0);
    make_player(0, 0);
    give_move(0);
    int32_t vehicle = 15;
    memcpy(entity(0) + 0x1a0, &vehicle, sizeof(vehicle));
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_NotAPlayer && script.asks == 1 && played.calls == 0);
    make_player(0, 0);
    memset(entity(0) + 0x250, 0, 8);
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_NotAPlayer && script.asks == 1 && played.calls == 0);
    // a client whose number has no slot in the server's moves, or whose slot has no player state yet: the event
    // is read, as T7Overcharged reads it before the game is called, but the game isn't called
    new_game();
    make_player(0, 18);
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_NotMoving && script.asks == 2 && played.calls == 0);
    make_player(0, -1);
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_NotMoving && played.calls == 0);
    make_player(0, 0x7fffffff);
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_NotMoving && played.calls == 0);
    give_move(2);
    make_player(0, 0x10002); // the client's number is a whole int: its lower half alone would be slot 2
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_NotMoving && played.calls == 0);
    make_player(0, 4);
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_NotMoving && played.calls == 0);
    give_move(4);
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_Played && played.move == game_moves + 4 * 0x1660);
    // only the slot's first eight bytes are its player state's pointer
    memset(game_moves + 4 * 0x1660, 0, 8);
    memset(game_moves + 4 * 0x1660 + 8, 0xff, 8);
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_NotMoving && played.calls == 0);
    uint64_t high = 0x100000000;
    memcpy(game_moves + 4 * 0x1660, &high, sizeof(high));
    CHECK(call(1, 3, 0, 101) && outcome == AnimEvent_Played);
}

static void test_script_errors(void) {
    new_game();
    make_player(0, 0);
    give_move(0);
    // no parameter after the hash: the game's script error, nothing else happens
    CHECK(!call(1, 1, 0, 101) && script.asks == 1 && played.calls == 0 && seeds_asked == 0);
    // no event: the error comes after T7Overcharged's tests of the entity, before the game is asked
    CHECK(!call(1, 2, 0, 101) && script.asks == 2 && played.calls == 0 && seeds_asked == 0);
    // and only when they pass: for an entity that isn't a player, a missing event is no error
    make_player(1, 1);
    memset(entity(1) + 0x250, 0, 8);
    CHECK(call(1, 2, 1, 101) && outcome == AnimEvent_NotAPlayer);
    game_server_state = 0;
    CHECK(call(1, 2, 0, 101) && outcome == AnimEvent_NoServer);
}

static void test_names(void) {
    const char *names[102] = { [15] = "FIRSTRAISEWEAPON", [17] = "RELOAD_EMPTY", [101] = "FLOURISH" };
    for (int i = 0; i < 102; i++)
        memcpy(game_event_names + i * Event_Name_Size, &names[i], sizeof(names[i]));
    CHECK(strcmp(event_name(15), "FIRSTRAISEWEAPON") == 0 && strcmp(event_name(17), "RELOAD_EMPTY") == 0 &&
          strcmp(event_name(101), "FLOURISH") == 0);
    CHECK(strcmp(event_name(16), "?") == 0); // no name
    CHECK(strcmp(event_name(102), "?") == 0 && strcmp(event_name(-1), "?") == 0 && strcmp(event_name(1000), "?") == 0);
    // the debug line of every outcome, with names and without; nothing without the switch
    new_game();
    make_player(0, 0);
    give_own_move(0);
    uint8_t *own_state; // the client's own player state, as the move has it
    memcpy(&own_state, game_moves, sizeof(own_state));
    set_body(own_state, 0x0003, 0x0012, 5, 0);
    played.plays = true, played.legs = 0x0003, played.torso = 0x2045, played.legs_timer = 5, played.torso_timer = 700;
    CHECK(call_said(1, 3, 0, 101) && outcome == AnimEvent_Played && said[0] == '\0');
    debug = 1;
    set_body(own_state, 0x0003, 0x0012, 5, 0);
    CHECK(call_said(1, 3, 0, 101) && outcome == AnimEvent_Played);
    CHECK(strcmp(said, "anim_event: entity 0, event 101 FLOURISH: played, the game gave 700; torso 0x0012 -> 0x2045 "
                       "(timer 0 -> 700), legs 0x0003 -> 0x0003 (timer 5 -> 5): the body's animation changed; the "
                       "move's player state is the client's\n") == 0);
    // nothing the game does to it: the body stays; a move whose player state isn't the client's is said
    give_move(0);
    set_body(game_player_states[0], 0x1001, 0x2002, 300, 400);
    played.plays = false, played.answer = -1;
    CHECK(call_said(1, 3, 0, 102) && outcome == AnimEvent_Played);
    CHECK(strcmp(said, "anim_event: entity 0, event 102 ?: played, the game gave -1 (it started no legs animation: a "
                       "torso one alone, or none); torso 0x2002 -> 0x2002 (timer 400 -> 400), legs 0x1001 -> 0x1001 "
                       "(timer 300 -> 300): the body's animation stayed; the move's player state is another "
                       "one\n") == 0);
    // any one of the four counts as a change
    const struct {
        uint16_t legs, torso;
        int32_t legs_timer, torso_timer;
    } changes[] = { { 0x1101, 0x2002, 300, 400 }, { 0x1001, 0x2102, 300, 400 }, { 0x1001, 0x2002, 301, 400 },
                    { 0x1001, 0x2002, 300, 401 }, { 0x1001, 0x2002, 300, 400 } };
    for (unsigned int i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        set_body(game_player_states[0], 0x1001, 0x2002, 300, 400);
        played.plays = true, played.legs = changes[i].legs, played.torso = changes[i].torso;
        played.legs_timer = changes[i].legs_timer, played.torso_timer = changes[i].torso_timer;
        CHECK(call_said(1, 3, 0, 15) && outcome == AnimEvent_Played);
        CHECK((strstr(said, "the body's animation changed;") != NULL) == (i < 4));
        CHECK((strstr(said, "the body's animation stayed;") != NULL) == (i == 4));
    }
    played.plays = false, played.answer = 700;
    CHECK(call_said(1, 3, 2000, 101) && outcome == AnimEvent_NotAnEntity);
    CHECK(strcmp(said, "anim_event: entity 2000: no such entity, nothing done\n") == 0);
    CHECK(call_said(1, 3, 1, 101) && outcome == AnimEvent_NotAPlayer);
    CHECK(strcmp(said, "anim_event: entity 1: not a player, nothing done\n") == 0);
    make_player(1, 9);
    CHECK(call_said(1, 3, 1, 15) && outcome == AnimEvent_NotMoving);
    CHECK(strcmp(said, "anim_event: entity 1, event 15 FIRSTRAISEWEAPON: the server has no player state for its "
                       "client yet, nothing done\n") == 0);
    game_server_state = 0;
    CHECK(call_said(1, 3, 0, 101) && outcome == AnimEvent_NoServer);
    CHECK(strcmp(said, "anim_event: entity 0: this machine runs no server, nothing done\n") == 0);
    // a script error ends the call before its line
    game_server_state = 2;
    CHECK(!call_said(1, 2, 0, 101) && said[0] == '\0');
    debug = 0;
}

static void test_init(void) {
    const uint64_t base = 0x100000000;
    unsetenv("BO3MACFIX_ANIMEVENTDEBUG");
    debug = 5;
    t7o_anim_event_init(base);
    CHECK(debug == 0);
    CHECK(Scr_GetInt == (void *)(base + 0x11a1e78) && BG_AnimScriptEvent == (void *)(base + 0x12dfb48) &&
          G_GetRandomSeed == (void *)(base + 0x11c042e));
    CHECK(server_state == (void *)(base + 0x144e8500) && entities == (void *)(base + 0x1966be8) &&
          moves == (void *)(base + 0x118e0b10) && event_names == (void *)(base + 0x20fba20));
    setenv("BO3MACFIX_ANIMEVENTDEBUG", "1", 1);
    t7o_anim_event_init(base);
    CHECK(debug == 1);
    unsetenv("BO3MACFIX_ANIMEVENTDEBUG");
    // the game's layout, as it was read from its code
    CHECK(Max_Entities == 1024 && Entity_Size == 0x4f8 && Entity_Type == 0x1a0 && Entity_Type_Player == 1 &&
          Entity_Client == 0x250 && Max_Clients == 18 && Pmove_Size == 0x1660 && Event_Names == 102 &&
          Event_Name_Size == 16);
}

int main(void) {
    test_init(); // first: it points the code at the game, the rest at the made-up one
    game_entities = calloc(1024, 0x4f8);
    game_moves = calloc(18, 0x1660);
    static uint8_t *entities_slot;
    entities_slot = game_entities;
    entities = &entities_slot;
    moves = game_moves;
    server_state = &game_server_state;
    event_names = game_event_names;
    Scr_GetInt = fake_Scr_GetInt;
    BG_AnimScriptEvent = fake_BG_AnimScriptEvent;
    G_GetRandomSeed = fake_G_GetRandomSeed;
    debug = 0;

    test_is_player();
    test_played();
    test_not_done();
    test_script_errors();
    test_names();
    free(game_entities);
    free(game_moves);
    if (failures == 0)
        printf("test_t7o_anim_event: all passed\n");
    return failures != 0;
}
