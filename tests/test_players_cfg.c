// Tests for players_cfg.c: which players folder a .cfg comes from, with made-up engine and game folders.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "../source/players_cfg.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char root[] = "/tmp/bo3mf_players_cfg_XXXXXX";
static char engine[1024], game[1024]; // the engine's players folder, and the game folder (ending in '/')

// a file saved at the given time (seconds since 1970)
static void save(const char *folder, const char *name, long when) {
    char path[2048];
    snprintf(path, sizeof(path), "%s%s", folder, name);
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        printf("FAIL: couldn't write %s\n", path);
        exit(1);
    }
    fputs("set something 1\n", file);
    fclose(file);
    struct timeval times[2] = { { when, 0 }, { when, 0 } };
    utimes(path, times);
}

// what the engine asks for: name in its players folder
static const char *asked(const char *name) {
    static char path[2048];
    snprintf(path, sizeof(path), "%s%s", engine, name);
    return path;
}

static bool redirected(const char *path, char *out, size_t out_len) {
    return players_cfg_in_game_folder(path, game, out, out_len);
}

int main(void) {
    if (mkdtemp(root) == NULL) {
        printf("FAIL: no temporary folder\n");
        return 1;
    }
    char path[2048], out[4096], expected[4096];
    snprintf(path, sizeof(path), "%s/Library", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/Library/Application Support", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/Library/Application Support/Call of Duty Black Ops III", root);
    mkdir(path, 0755);
    snprintf(engine, sizeof(engine), "%s/Library" Engine_Players_Folder, root);
    mkdir(engine, 0755);
    snprintf(game, sizeof(game), "%s/game/", root);
    mkdir(game, 0755);
    snprintf(path, sizeof(path), "%splayers", game);
    mkdir(path, 0755);
    char game_players[1100];
    snprintf(game_players, sizeof(game_players), "%splayers/", game);

    // AAE's options, saved from Lua in the game folder: exec gets that copy
    save(game_players, "AAEOptions_Default.cfg", 2000000000);
    snprintf(expected, sizeof(expected), "%sAAEOptions_Default.cfg", game_players);
    CHECK(redirected(asked("AAEOptions_Default.cfg"), out, sizeof(out)) && strcmp(out, expected) == 0);
    // in neither folder, or only the engine's: the engine's path
    CHECK(!redirected(asked("AAEOptions_Other.cfg"), out, sizeof(out)));
    save(engine, "bindings_1.cfg", 2000000000);
    CHECK(!redirected(asked("bindings_1.cfg"), out, sizeof(out)));

    // a players folder brought over from Windows, with an old copy of the bindings the engine keeps itself: the engine's
    // newer one stays
    save(engine, "bindings_0.cfg", 2000000000);
    save(game_players, "bindings_0.cfg", 1600000000);
    CHECK(!redirected(asked("bindings_0.cfg"), out, sizeof(out)));
    // whichever was saved last: an older copy in the engine's folder (put there by hand, say) doesn't hide the mod's
    save(engine, "AAECustomMutations_Default.cfg", 1600000000);
    save(game_players, "AAECustomMutations_Default.cfg", 2000000000);
    CHECK(redirected(asked("AAECustomMutations_Default.cfg"), out, sizeof(out)));
    // saved at the same time, as when both folders are the same one through a link: the game folder's
    save(engine, "Same.cfg", 2000000000);
    save(game_players, "Same.cfg", 2000000000);
    CHECK(redirected(asked("Same.cfg"), out, sizeof(out)));

    // only .cfg files, only in the players folder, nothing above it
    save(game_players, "config.ini", 2000000000);
    CHECK(!redirected(asked("config.ini"), out, sizeof(out)));
    CHECK(!redirected(asked(".cfg"), out, sizeof(out)));
    CHECK(!redirected(asked("../players/AAEOptions_Default.cfg"), out, sizeof(out)));
    snprintf(path, sizeof(path), "%s/Library/Application Support/AAEOptions_Default.cfg", root);
    CHECK(!redirected(path, out, sizeof(out)));
    snprintf(path, sizeof(path), "%szone/AAEOptions_Default.cfg", game);
    CHECK(!redirected(path, out, sizeof(out)));
    // a folder with a .cfg name isn't a file
    snprintf(path, sizeof(path), "%sFolder.cfg", game_players);
    mkdir(path, 0755);
    CHECK(!redirected(asked("Folder.cfg"), out, sizeof(out)));

    // the folder's name and the extension in any case
    save(game_players, "Upper.CFG", 2000000000);
    snprintf(path, sizeof(path), "%s/Library/APPLICATION SUPPORT/call of duty black ops iii/PLAYERS/Upper.CFG", root);
    snprintf(expected, sizeof(expected), "%sUpper.CFG", game_players);
    CHECK(redirected(path, out, sizeof(out)) && strcmp(out, expected) == 0);
    // backslashes below the players folder
    snprintf(path, sizeof(path), "%ssub", game_players);
    mkdir(path, 0755);
    save(game_players, "sub/Preset.cfg", 2000000000);
    snprintf(expected, sizeof(expected), "%ssub/Preset.cfg", game_players);
    CHECK(redirected(asked("sub\\Preset.cfg"), out, sizeof(out)) && strcmp(out, expected) == 0);
    // a game folder path that doesn't fit isn't used
    size_t needed = strlen(game_players) + strlen("AAEOptions_Default.cfg") + 1;
    CHECK(!redirected(asked("AAEOptions_Default.cfg"), out, needed - 1));
    CHECK(redirected(asked("AAEOptions_Default.cfg"), out, needed));

    snprintf(path, sizeof(path), "rm -rf '%s'", root);
    if (system(path) != 0)
        printf("couldn't remove %s\n", root);
    if (failures == 0)
        printf("test_players_cfg: all passed\n");
    return failures == 0 ? 0 : 1;
}
