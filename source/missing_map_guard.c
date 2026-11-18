// MissingMapGuard: a map the lobby can't start (one that isn't installed or owned, or a workshop map that is gone) is
// refused before the menus are shut down, so the game goes back to the menus with its own message instead of
// crashing.
//
// What the game does (wip/night/crash-journey.md has the addresses and how each was found): the lobby host's launch
// task, at its stage "preload", first shuts the menus' level down, which frees the per-local-client memory (the local
// clients' connections, the client game state and more: their pointers are NULL from then on), and only then lets
// Com_PreloadMap test whether the map can be had at all, the test Engine.IsMapValid answers to the menus. For a map
// that can't, the stage stores "You do not have this map ..." and fails; at the task's next turn, in the next frame,
// its failure callback would drop the game back to the menus with that message. It never gets there: the rest of the
// frame and the next frame's start read the freed memory (the client's resend check, then the frame's own frame-time
// cap), and the game crashes. The stage's other failure, before that test, has the same window. The Windows game has
// the same stage.
//
// The guard runs the stage's own decisions before the teardown instead, in the same order and with the game's own
// functions: when the stage would fail after it, the task fails at once as the stage would have made it fail (its
// error, its console line, the failed state) and nothing is shut down. At the next turn the game's failure callback
// drops to the menus from a running menus' level; the drop's handler shuts it down and starts it anew inside the
// one error handler, so no frame runs with that memory gone. When the stage would go on, the game's stage runs as it
// is. Every input of the two tests is the same before and after the teardown (crash-journey.md, section 13), so the
// guard refuses only what the game would have refused. It applies with or without a mod.
#include <stdio.h>
#include <stdlib.h>
#include "dobby.h"
#include "missing_map_guard.h"
#include "offsets.h"

#define Usermap_Name 0x64 // in a workshop map's entry: the name the game loads it by
#define Lobby_Host_Channel 0xb // the console channel the launch task prints on (LOBBYHOST)

static bool debug;
static missing_map_game_t game;

const char *missing_map_guard_target(const char *map, const uint8_t *usermap) {
    return usermap != NULL ? (const char *)(usermap + Usermap_Name) : map;
}

missing_map_verdict_t missing_map_guard_verdict(const missing_map_game_t *game, const char **target,
                                                const char **gametype) {
    // the stage's own first two tests: a client launch or a game already running fail or end it without a teardown
    if (!game->is_host(1) || game->in_game())
        return Launch_Pass;
    // what the stage reads before its teardown, in its order (it sets a dvar from the game type in between, which
    // none of the tests reads: left to the stage itself)
    const char *map = game->dvar_string(*game->lobby_map_dvar);
    *gametype = game->dvar_string(*game->gametype_dvar);
    *target = missing_map_guard_target(map, game->select_usermap(map));
    // the stage's failures after the teardown, in its order
    if (game->server_without_menus() || !game->dvar_bool(*game->launch_allowed_dvar))
        return Launch_Refuse_Disconnected;
    if (!game->map_test_skipped() && !game->map_can_be_had(*target))
        return Launch_Refuse_Missing_Map;
    return Launch_Pass;
}

void missing_map_guard_refuse(const missing_map_game_t *game, missing_map_verdict_t verdict, const char *target,
                              const char *gametype) {
    if (verdict == Launch_Refuse_Missing_Map) {
        // as Com_PreloadMap stores its error and the stage prints it
        *game->error = game->localize(game->missing_map_key);
        game->print_error(Lobby_Host_Channel, game->preload_failed_format, target, gametype, *game->error);
    } else {
        *game->error = game->disconnected_error;
    }
    *game->state = Launch_Failed;
}

// the launch task's pump: int (task *), called by the task manager once a frame while the task runs; 0 go on,
// 1 done, 2 failed
install_hook_name(LaunchGamePump, int, uint64_t task) {
    if (*game.state != Launch_Preload)
        return orig_LaunchGamePump(task);
    const char *target = NULL, *gametype = NULL;
    missing_map_verdict_t verdict = missing_map_guard_verdict(&game, &target, &gametype);
    if (verdict == Launch_Pass) {
        if (debug && target != NULL)
            printf("missing map guard: the lobby starts %s (%s): it can be had, the game's own preload goes on\n",
                   target, gametype);
        return orig_LaunchGamePump(task);
    }
    missing_map_guard_refuse(&game, verdict, target, gametype);
    if (verdict == Launch_Refuse_Missing_Map)
        printf("missing map guard: the lobby's start of %s (%s) is refused before the menus are shut down: %s; "
               "the game goes back to the menus with that message instead of crashing\n", target, gametype,
               *game.error);
    else
        printf("missing map guard: the lobby's start of %s (%s) is refused before the menus are shut down with the "
               "game's own %s, instead of crashing after it\n", target, gametype, *game.error);
    // what the stage returns after it has set the failed state
    return 0;
}

void missing_map_guard_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    const char *env = getenv("BO3MACFIX_MISSINGMAPDEBUG");
    debug = env != NULL && atoi(env) != 0;
    uint64_t base = game_base_address;
    game = (missing_map_game_t){
        .state = (int32_t *)(base + ADDR_LaunchGame_State),
        .error = (const char **)(base + ADDR_LaunchGame_Error),
        .lobby_map_dvar = (void *const *)(base + ADDR_dvar_LobbyMap),
        .gametype_dvar = (void *const *)(base + ADDR_dvar_LobbyGametype),
        .launch_allowed_dvar = (void *const *)(base + ADDR_dvar_LaunchAllowed),
        .is_host = (void *)(base + ADDR_LobbyHost_IsHost),
        .in_game = (void *)(base + ADDR_Com_IsInGame),
        .server_without_menus = (void *)(base + ADDR_Com_ServerWithoutUILevel),
        .dvar_bool = (void *)(base + ADDR_Dvar_GetBool),
        .dvar_string = (void *)(base + ADDR_Dvar_GetString),
        .select_usermap = (void *)(base + ADDR_Usermap_SelectByName),
        .map_test_skipped = (void *)(base + ADDR_Com_PreloadMap_SkipsTest),
        .map_can_be_had = (void *)(base + ADDR_Com_MapCanBeHad),
        .localize = (void *)(base + ADDR_SEH_Localize),
        .print_error = (void *)(base + ADDR_Com_PrintError),
        .missing_map_key = (const char *)(base + ADDR_String_PLATFORM_MISSINGMAP),
        .disconnected_error = (const char *)(base + ADDR_String_EXE_DISCONNECTED),
        .preload_failed_format = (const char *)(base + ADDR_String_UnableToPreload),
    };
    install_hook_LaunchGamePump((void *)(base + ADDR_LaunchGamePump));
    if (debug)
        printf("missing map guard: on, a lobby start that the game's preload would refuse is refused before the "
               "menus are shut down\n");
}
