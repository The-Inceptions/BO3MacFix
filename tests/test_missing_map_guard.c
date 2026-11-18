// missing_map_guard.c: the preload stage's decisions made before its teardown, against a made-up launch task that
// does what the game's does (its tests, its teardown, its two failures), so that "the guard refuses exactly what the
// game would refuse after its teardown" is checked for every combination (the test includes the source itself).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../source/missing_map_guard.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define Base 0x100000000ULL
// the game's own states of the launch task (its jump table at 0x10108875c), kept apart from the guard's header so
// that a wrong value there shows
#define Game_Preload 0x1f
#define Game_Failed 0x25

// --- the made-up game --------------------------------------------------------------------------------------------
static void *hooked_at, *hooked_with;
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)out_origin_func;
    hooked_at = address, hooked_with = fake_func, hooks++;
    return 0;
}

// what the game's functions were asked, in order, as one string ("host in_game map gametype usermap ...")
static char asked[512];
static void ask(const char *what) {
    size_t used = strlen(asked);
    snprintf(asked + used, sizeof(asked) - used, "%s%s", used == 0 ? "" : " ", what);
}

static int32_t task_state;
static const char *task_error;
static int dvar_objects[3];
static void *dvar_map = &dvar_objects[0], *dvar_gametype = &dvar_objects[1], *dvar_allowed = &dvar_objects[2];
static const char *lobby_map, *lobby_gametype;
static bool host, in_game, server_running_alone, allowed, test_skipped;
static const char *maps_that_can_be_had[4];
// a workshop map's entry: the list matches it by its name at +0x84, the game loads it by the name at +0x64
static uint8_t usermap_entry[0x4c8];
static bool usermap_installed;
static const uint8_t *current_usermap;
static const char *const missing_key = "PLATFORM_MISSINGMAP", *const disconnected = "EXE_DISCONNECTED";
static const char *const preload_format = "Unable to begin preloading map %s gametype %s: %s\n";
static const char *const missing_text = "You do not have this map. You can get this map by downloading map packs";
static char console[1024];
static int console_lines, last_channel;

static bool game_is_host(int session) {
    ask(session == 1 ? "host" : "host?");
    return host;
}
static bool game_in_game(void) {
    ask("in_game");
    return in_game;
}
static bool game_server_without_menus(void) {
    ask("server_alone");
    return server_running_alone;
}
static bool game_dvar_bool(void *dvar) {
    ask(dvar == dvar_allowed ? "allowed" : "bool?");
    return dvar == dvar_allowed ? allowed : false;
}
static const char *game_dvar_string(void *dvar) {
    ask(dvar == dvar_map ? "map" : dvar == dvar_gametype ? "gametype" : "string?");
    return dvar == dvar_map ? lobby_map : dvar == dvar_gametype ? lobby_gametype : "";
}
static const uint8_t *game_select_usermap(const char *map) {
    ask("usermap");
    current_usermap = usermap_installed && strcmp(map, (const char *)usermap_entry + 0x84) == 0 ? usermap_entry : NULL;
    return current_usermap;
}
static bool game_test_skipped(void) {
    ask("skipped");
    return test_skipped;
}
static bool game_can_be_had(const char *map) {
    ask("can_be_had");
    if (map == NULL || *map == '\0')
        return false;
    // a workshop map that is there, as the game's test finds it: the current one by its loading name
    if (current_usermap != NULL && strcmp(map, (const char *)current_usermap + 0x64) == 0)
        return true;
    for (size_t i = 0; i < sizeof(maps_that_can_be_had) / sizeof(maps_that_can_be_had[0]); i++)
        if (maps_that_can_be_had[i] != NULL && strcmp(map, maps_that_can_be_had[i]) == 0)
            return true;
    return false;
}
static const char *game_localize(const char *key) {
    ask("localize");
    return key == missing_key ? missing_text : key;
}
static void game_print_error(int channel, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(console, sizeof(console), format, args);
    va_end(args);
    last_channel = channel, console_lines++;
}

static const missing_map_game_t made_up = {
    .state = &task_state, .error = &task_error, .lobby_map_dvar = &dvar_map, .gametype_dvar = &dvar_gametype,
    .launch_allowed_dvar = &dvar_allowed, .is_host = game_is_host, .in_game = game_in_game,
    .server_without_menus = game_server_without_menus, .dvar_bool = game_dvar_bool, .dvar_string = game_dvar_string,
    .select_usermap = game_select_usermap, .map_test_skipped = game_test_skipped, .map_can_be_had = game_can_be_had,
    .localize = game_localize, .print_error = game_print_error, .missing_map_key = missing_key,
    .disconnected_error = disconnected, .preload_failed_format = preload_format,
};

// the game's launch task as it is (0x10108830b): at the preload stage its two tests, the reads, the teardown of the
// menus' level (the per-local-client memory freed), then its two failures, each after a second shutdown, or the
// memory made again for the map; the failed state reports "failed" at the next step
static bool menus_running, memory_freed;
static int teardowns, game_steps;
static uint64_t last_task;
static int game_pump(uint64_t task) {
    game_steps++;
    last_task = task;
    switch (task_state) {
    case Game_Preload: {
        if (!host) {
            task_error = disconnected;
            task_state = Game_Failed;
            return 0;
        }
        if (in_game) {
            task_state = 0x24;
            return 0;
        }
        const char *map = lobby_map, *gametype = lobby_gametype;
        game_select_usermap(map);
        const char *target = current_usermap != NULL ? (const char *)current_usermap + 0x64 : map;
        if (menus_running) {
            menus_running = false, memory_freed = true, teardowns++;
        }
        if (server_running_alone || !allowed) {
            memory_freed = true, teardowns++;
            task_error = disconnected;
            task_state = Game_Failed;
            return 0;
        }
        if (!test_skipped && !game_can_be_had(target)) {
            memory_freed = true, teardowns++;
            task_error = game_localize(missing_key);
            game_print_error(0xb, preload_format, target, gametype, task_error);
            task_state = Game_Failed;
            return 0;
        }
        memory_freed = false;
        task_state = 0x20;
        return 0;
    }
    case Game_Failed:
        return 2;
    case 0x24:
        return 1;
    default:
        return 0;
    }
}

static void set_game(void) {
    game = made_up;
    orig_LaunchGamePump = game_pump;
    debug = false;
    task_state = Game_Preload;
    task_error = NULL;
    lobby_map = "zm_factory", lobby_gametype = "zclassic";
    host = true, in_game = false, server_running_alone = false, allowed = true, test_skipped = false;
    maps_that_can_be_had[0] = "zm_theater", maps_that_can_be_had[1] = "zm_zod";
    maps_that_can_be_had[2] = maps_that_can_be_had[3] = NULL;
    memset(usermap_entry, 0, sizeof(usermap_entry));
    strcpy((char *)usermap_entry + 0x84, "zm_ou_deimos_folder");
    strcpy((char *)usermap_entry + 0x64, "zm_ou_deimos");
    usermap_installed = false, current_usermap = NULL;
    menus_running = true, memory_freed = false, teardowns = 0, game_steps = 0, last_task = 0;
    asked[0] = console[0] = '\0';
    console_lines = 0, last_channel = -1;
}

// what was printed while the block ran
static char printed[8192];
static int saved_stdout = -1;
static FILE *capture;
static void capture_start(void) {
    fflush(stdout);
    capture = tmpfile();
    saved_stdout = dup(STDOUT_FILENO);
    dup2(fileno(capture), STDOUT_FILENO);
}
static void capture_end(void) {
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    rewind(capture);
    size_t n = fread(printed, 1, sizeof(printed) - 1, capture);
    printed[n] = '\0';
    fclose(capture);
}
static int count_in(const char *text, const char *what) {
    int n = 0;
    for (const char *p = text; (p = strstr(p, what)) != NULL; p++)
        n++;
    return n;
}

// --- the tests ---------------------------------------------------------------------------------------------------
static void test_target(void) {
    uint8_t entry[0x4c8] = { 0 };
    strcpy((char *)entry + 0x64, "zm_loading_name");
    const char *map = "lobby_name";
    CHECK(missing_map_guard_target(map, NULL) == map);
    CHECK(missing_map_guard_target(map, entry) == (const char *)entry + 0x64);
    CHECK(strcmp(missing_map_guard_target(map, entry), "zm_loading_name") == 0);
}

static void test_verdict_order(void) {
    const char *target = NULL, *gametype = NULL;
    // a map that can't be had: everything the stage asks, in its order
    set_game();
    CHECK(missing_map_guard_verdict(&made_up, &target, &gametype) == Launch_Refuse_Missing_Map);
    CHECK(strcmp(asked, "host in_game map gametype usermap server_alone allowed skipped can_be_had") == 0);
    CHECK(strcmp(target, "zm_factory") == 0 && strcmp(gametype, "zclassic") == 0);
    // one that can: the same questions, the same order
    set_game();
    lobby_map = "zm_theater";
    target = gametype = NULL;
    CHECK(missing_map_guard_verdict(&made_up, &target, &gametype) == Launch_Pass);
    CHECK(strcmp(asked, "host in_game map gametype usermap server_alone allowed skipped can_be_had") == 0);
    CHECK(strcmp(target, "zm_theater") == 0);
    // not the host: nothing else asked, nothing read
    set_game();
    host = false;
    target = gametype = NULL;
    CHECK(missing_map_guard_verdict(&made_up, &target, &gametype) == Launch_Pass);
    CHECK(strcmp(asked, "host") == 0 && target == NULL && gametype == NULL);
    // in a game already: the stage ends the task without reading the map
    set_game();
    in_game = true;
    CHECK(missing_map_guard_verdict(&made_up, &target, &gametype) == Launch_Pass);
    CHECK(strcmp(asked, "host in_game") == 0);
    // the stage's first failure: the map test isn't asked
    set_game();
    server_running_alone = true;
    CHECK(missing_map_guard_verdict(&made_up, &target, &gametype) == Launch_Refuse_Disconnected);
    CHECK(strcmp(asked, "host in_game map gametype usermap server_alone") == 0);
    set_game();
    allowed = false;
    CHECK(missing_map_guard_verdict(&made_up, &target, &gametype) == Launch_Refuse_Disconnected);
    CHECK(strcmp(asked, "host in_game map gametype usermap server_alone allowed") == 0);
    // Com_PreloadMap's own condition for not testing: the test isn't made
    set_game();
    test_skipped = true;
    CHECK(missing_map_guard_verdict(&made_up, &target, &gametype) == Launch_Pass);
    CHECK(strcmp(asked, "host in_game map gametype usermap server_alone allowed skipped") == 0);
}

static void test_verdict_usermap(void) {
    const char *target = NULL, *gametype = NULL;
    // a workshop map that is there: tested by its loading name, which can be had
    set_game();
    lobby_map = "zm_ou_deimos_folder";
    usermap_installed = true;
    CHECK(missing_map_guard_verdict(&made_up, &target, &gametype) == Launch_Pass);
    CHECK(target == (const char *)usermap_entry + 0x64);
    // the same map once it is gone (unsubscribed, deleted, being updated): its lobby name is tested and refused
    set_game();
    lobby_map = "zm_ou_deimos_folder";
    usermap_installed = false;
    CHECK(missing_map_guard_verdict(&made_up, &target, &gametype) == Launch_Refuse_Missing_Map);
    CHECK(target == lobby_map);
    // an empty map name, as the game's test answers it
    set_game();
    lobby_map = "";
    CHECK(missing_map_guard_verdict(&made_up, &target, &gametype) == Launch_Refuse_Missing_Map);
}

static void test_refuse(void) {
    set_game();
    missing_map_guard_refuse(&made_up, Launch_Refuse_Missing_Map, "zm_factory", "zclassic");
    CHECK(task_state == Game_Failed && task_error == missing_text);
    CHECK(console_lines == 1 && last_channel == 0xb);
    CHECK(strcmp(console, "Unable to begin preloading map zm_factory gametype zclassic: You do not have this map. "
                          "You can get this map by downloading map packs\n") == 0);
    set_game();
    missing_map_guard_refuse(&made_up, Launch_Refuse_Disconnected, "zm_factory", "zclassic");
    CHECK(task_state == Game_Failed && task_error == disconnected && console_lines == 0);
    CHECK(strcmp(asked, "") == 0); // the game's error as it is, not localized: the stage stores it so
}

static void test_hook_other_states(void) {
    // every other state: the game's step as it is, its result handed back, nothing asked
    static const int32_t states[] = { 0x1d, 0x1e, 0x20, 0x21, 0x24, Game_Failed, 0, -1 };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        set_game();
        task_state = states[i];
        int result = fake_LaunchGamePump(0x1234);
        CHECK(game_steps == 1 && last_task == 0x1234 && asked[0] == '\0');
        CHECK(result == (states[i] == Game_Failed ? 2 : states[i] == 0x24 ? 1 : 0));
        CHECK(task_state == states[i] && teardowns == 0);
    }
}

static void test_hook_missing_map(void) {
    // the crash's case: the lobby holds a map that can't be had. The menus stay up, the task fails at this step
    // and reports it at the next, the game's way
    set_game();
    capture_start();
    int first = fake_LaunchGamePump(0x77);
    capture_end();
    CHECK(first == 0 && game_steps == 0 && teardowns == 0 && menus_running && !memory_freed);
    CHECK(task_state == Game_Failed && task_error == missing_text);
    CHECK(console_lines == 1 && count_in(console, "Unable to begin preloading map zm_factory gametype zclassic") == 1);
    CHECK(count_in(printed, "missing map guard: the lobby's start of zm_factory (zclassic) is refused before the menus "
                            "are shut down: You do not have this map") == 1);
    CHECK(count_in(printed, "missing map guard:") == 1);
    int second = fake_LaunchGamePump(0x77);
    CHECK(second == 2 && game_steps == 1 && teardowns == 0);
    // without the guard the same step tears the menus down first (what crashed)
    set_game();
    CHECK(game_pump(0x77) == 0 && task_state == Game_Failed && teardowns == 2 && memory_freed);
}

static void test_hook_map_there(void) {
    // a map that can be had: the game's own stage, teardown and all, and no line
    set_game();
    lobby_map = "zm_theater";
    capture_start();
    int result = fake_LaunchGamePump(0x88);
    capture_end();
    CHECK(result == 0 && game_steps == 1 && last_task == 0x88 && task_state == 0x20);
    CHECK(teardowns == 1 && !memory_freed && console_lines == 0 && printed[0] == '\0');
    // with debug, one line that says so
    set_game();
    lobby_map = "zm_zod";
    debug = true;
    capture_start();
    fake_LaunchGamePump(0x88);
    capture_end();
    CHECK(count_in(printed, "missing map guard: the lobby starts zm_zod (zclassic): it can be had") == 1);
    // debug says nothing for a step that isn't a preload, or one the stage ends by itself
    set_game();
    debug = true;
    host = false;
    capture_start();
    fake_LaunchGamePump(0x88);
    capture_end();
    CHECK(printed[0] == '\0' && game_steps == 1 && task_state == Game_Failed && task_error == disconnected);
}

static void test_hook_disconnected(void) {
    set_game();
    allowed = false;
    capture_start();
    int result = fake_LaunchGamePump(0x99);
    capture_end();
    CHECK(result == 0 && game_steps == 0 && teardowns == 0 && task_state == Game_Failed);
    CHECK(task_error == disconnected && console_lines == 0);
    CHECK(count_in(printed, "missing map guard: the lobby's start of zm_factory (zclassic) is refused before the menus "
                            "are shut down with the game's own EXE_DISCONNECTED") == 1);
}

// every combination of what the stage asks: the guard refuses exactly when the game's stage would fail after its
// teardown, with the same error and console line, and otherwise the game's stage runs and ends the same way
static void test_same_answer_as_the_game(void) {
    static const char *const maps[] = { "zm_factory", "zm_theater", "zm_ou_deimos_folder", "" };
    int combinations = 0, refusals = 0;
    for (int bits = 0; bits < 64; bits++)
        for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++)
            for (int installed = 0; installed < 2; installed++) {
                // the game alone
                set_game();
                host = bits & 1, in_game = bits & 2, server_running_alone = bits & 4, allowed = bits & 8;
                test_skipped = bits & 16;
                menus_running = bits & 32;
                lobby_map = maps[m];
                usermap_installed = installed;
                game_pump(0);
                int32_t game_state = task_state;
                const char *game_error = task_error;
                bool game_failed_after_teardown = game_state == Game_Failed && teardowns > 0;
                char game_console[1024];
                snprintf(game_console, sizeof(game_console), "%s", console);
                // with the guard
                set_game();
                host = bits & 1, in_game = bits & 2, server_running_alone = bits & 4, allowed = bits & 8;
                test_skipped = bits & 16;
                menus_running = bits & 32;
                lobby_map = maps[m];
                usermap_installed = installed;
                capture_start();
                fake_LaunchGamePump(0);
                capture_end();
                combinations++;
                bool refused = game_steps == 0;
                refusals += refused;
                CHECK(refused == game_failed_after_teardown);
                CHECK(task_state == game_state && task_error == game_error);
                CHECK(strcmp(console, game_console) == 0);
                if (refused)
                    CHECK(teardowns == 0 && !memory_freed && menus_running == (bool)(bits & 32));
            }
    CHECK(combinations == 512 && refusals > 0 && refusals < combinations);
}

static void test_init(void) {
    // off: nothing hooked, the game's task as it is
    hooks = 0, hooked_at = hooked_with = NULL;
    memset(&game, 0, sizeof(game));
    capture_start();
    missing_map_guard_init(Base, 0);
    capture_end();
    CHECK(hooks == 0 && game.state == NULL && printed[0] == '\0');
    // on: the pump hooked, the game's own variables, functions and texts
    unsetenv("BO3MACFIX_MISSINGMAPDEBUG");
    capture_start();
    missing_map_guard_init(Base, 1);
    capture_end();
    CHECK(hooks == 1 && hooked_at == (void *)(Base + 0x108830b) && hooked_with == (void *)fake_LaunchGamePump);
    CHECK((uint64_t)(uintptr_t)game.state == Base + 0x447b394 && (uint64_t)(uintptr_t)game.error == Base + 0x447b4b8);
    CHECK((uint64_t)(uintptr_t)game.lobby_map_dvar == Base + 0x1283ece0);
    CHECK((uint64_t)(uintptr_t)game.gametype_dvar == Base + 0x1283ed88);
    CHECK((uint64_t)(uintptr_t)game.launch_allowed_dvar == Base + 0x118dfa88);
    CHECK((uint64_t)(uintptr_t)game.is_host == Base + 0xe3152b && (uint64_t)(uintptr_t)game.in_game == Base + 0xe71710);
    CHECK((uint64_t)(uintptr_t)game.server_without_menus == Base + 0xe7174f);
    CHECK((uint64_t)(uintptr_t)game.dvar_bool == Base + 0xfa0224);
    CHECK((uint64_t)(uintptr_t)game.dvar_string == Base + 0xfa0474);
    CHECK((uint64_t)(uintptr_t)game.select_usermap == Base + 0xe4a7c5);
    CHECK((uint64_t)(uintptr_t)game.map_test_skipped == Base + 0x127b0c2);
    CHECK((uint64_t)(uintptr_t)game.map_can_be_had == Base + 0x11558d7);
    CHECK((uint64_t)(uintptr_t)game.localize == Base + 0x109cdd4);
    CHECK((uint64_t)(uintptr_t)game.print_error == Base + 0xe7117f);
    CHECK((uint64_t)(uintptr_t)game.missing_map_key == Base + 0x16ad0ac);
    CHECK((uint64_t)(uintptr_t)game.disconnected_error == Base + 0x16c0c0b);
    CHECK((uint64_t)(uintptr_t)game.preload_failed_format == Base + 0x16fea24);
    CHECK(!debug && printed[0] == '\0');
    // debug from the environment: on for a number other than 0
    setenv("BO3MACFIX_MISSINGMAPDEBUG", "1", 1);
    capture_start();
    missing_map_guard_init(Base, 1);
    capture_end();
    CHECK(debug && count_in(printed, "missing map guard: on") == 1);
    setenv("BO3MACFIX_MISSINGMAPDEBUG", "0", 1);
    missing_map_guard_init(Base, 1);
    CHECK(!debug);
    unsetenv("BO3MACFIX_MISSINGMAPDEBUG");
    CHECK(hooks == 3);
}

int main(void) {
    test_target();
    test_verdict_order();
    test_verdict_usermap();
    test_refuse();
    test_hook_other_states();
    test_hook_missing_map();
    test_hook_map_there();
    test_hook_disconnected();
    test_same_answer_as_the_game();
    test_init();
    if (failures == 0)
        printf("test_missing_map_guard: all passed\n");
    return failures == 0 ? 0 : 1;
}
