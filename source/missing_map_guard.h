#pragma once
#include <stdbool.h>
#include <stdint.h>

// MissingMapGuard: a map the lobby can't start (one that isn't installed or owned, or a workshop map that is gone) is
// refused before the menus are shut down, so the game goes back to the menus with its own message instead of
// crashing (see missing_map_guard.c)
void missing_map_guard_init(uint64_t game_base_address, int enabled);

// the lobby host's launch task: its state when it is about to preload the map, and the state whose next turn reports
// the failure (LOBBYTASK_HOSTLAUNCH_PRELOAD and its failed state)
#define Launch_Preload 0x1f
#define Launch_Failed 0x25

// what the launch task's preload stage asks the game, in the game's own functions and variables (the unit test
// gives made-up ones)
typedef struct {
    int32_t *state;                                  // the task's state
    const char **error;                              // the task's error, which its failure callback drops with
    void *const *lobby_map_dvar;                     // dvar_t *: the lobby's map
    void *const *gametype_dvar;                      // dvar_t *: the lobby's game type
    void *const *launch_allowed_dvar;                // dvar_t *: a bool the stage asks before it preloads
    bool (*is_host)(int session);                    // LobbyHost_IsHost
    bool (*in_game)(void);                           // Com_IsInGame
    bool (*server_without_menus)(void);              // sv_running and not Com_IsRunningUILevel
    bool (*dvar_bool)(void *dvar);                   // Dvar_GetBool
    const char *(*dvar_string)(void *dvar);          // Dvar_GetString
    const uint8_t *(*select_usermap)(const char *map); // makes the lobby's workshop map the current one, or none
    bool (*map_test_skipped)(void);                  // Com_PreloadMap's condition for not testing the map
    bool (*map_can_be_had)(const char *map);         // Com_PreloadMap's test (Engine.IsMapValid's too)
    const char *(*localize)(const char *key);
    void (*print_error)(int channel, const char *format, ...);
    const char *missing_map_key;                     // the game's "PLATFORM_MISSINGMAP"
    const char *disconnected_error;                  // the game's "EXE_DISCONNECTED"
    const char *preload_failed_format;               // the game's "Unable to begin preloading map %s gametype %s: %s\n"
} missing_map_game_t;

typedef enum {
    Launch_Pass,                // the game's own stage goes on
    Launch_Refuse_Missing_Map,  // Com_PreloadMap would refuse the map after the teardown: refuse it before
    Launch_Refuse_Disconnected, // the stage's other failure after the teardown: the same, with its error
} missing_map_verdict_t;

// the preload stage's own decisions, made before its teardown, in its order and asking only what it asks: the task's
// host and in-game tests first, then the map and game type, the workshop map made current, then the stage's two
// failures. *target and *gametype get the map name the game would preload and the game type, once they are read
missing_map_verdict_t missing_map_guard_verdict(const missing_map_game_t *game, const char **target,
                                                const char **gametype);
// the name the game preloads: a workshop map's own name in its entry, else the lobby's map as it is
const char *missing_map_guard_target(const char *map, const uint8_t *usermap);
// a refusal as the game's stage makes it, minus the teardown: its error, its console line, the failed state
void missing_map_guard_refuse(const missing_map_game_t *game, missing_map_verdict_t verdict, const char *target,
                              const char *gametype);
