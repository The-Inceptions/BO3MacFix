// Tests for launch_args.c: which command lines start a game at launch (SkipIntro only acts when none does).
#include <stdio.h>
#include "../source/launch_args.h"

static int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define ARGS(...) (int)(sizeof((char *[]){ __VA_ARGS__ }) / sizeof(char *)), (char *[]){ __VA_ARGS__ }
#define EXE "/Game/CoDBlkOps3.app/Contents/MacOS/CoDBlkOps3_Exe.bo3macfix"

int main(void) {
    // the title screen: no arguments, a mod, the launcher's own path passed on, other settings
    CHECK(!launch_args_start_game(ARGS(EXE)));
    CHECK(!launch_args_start_game(ARGS(EXE, "+set", "fs_game", "2631943123")));
    CHECK(!launch_args_start_game(ARGS(EXE, "/Game/CoDBlkOps3.app/Contents/MacOS/CoDBlkOps3", "+set", "fs_game",
                                       "2631943123")));
    CHECK(!launch_args_start_game(ARGS(EXE, "+seta", "r_fullscreen", "1", "+exec", "my.cfg")));
    CHECK(!launch_args_start_game(ARGS(EXE, "+set fs_game 2631943123")));
    // a map name without "+map" isn't a command
    CHECK(!launch_args_start_game(ARGS(EXE, "+set", "mapname", "zm_zod")));
    CHECK(!launch_args_start_game(ARGS(EXE, "map", "zm_zod")));
    // the game's argv[0] isn't part of its command line
    CHECK(!launch_args_start_game(ARGS("/tmp/+map/CoDBlkOps3_Exe")));
    CHECK(!launch_args_start_game(0, (char *[]){ NULL }));

    // a map or a server: as separate arguments (the tests, Steam's launch options), or as one
    CHECK(launch_args_start_game(ARGS(EXE, "+set", "fs_game", "2631943123", "+map", "zm_zod")));
    CHECK(launch_args_start_game(ARGS(EXE, "+map", "zm_theater")));
    CHECK(launch_args_start_game(ARGS(EXE, "+devmap", "zm_cosmodrome")));
    CHECK(launch_args_start_game(ARGS(EXE, "+set fs_game 2631943123 +map zm_tomb")));
    CHECK(launch_args_start_game(ARGS(EXE, "+MAP", "zm_zod")));
    CHECK(launch_args_start_game(ARGS(EXE, "+connect", "192.168.1.2")));
    CHECK(launch_args_start_game(ARGS(EXE, "+set", "fs_game", "2631943123", "+set", "sv_cheats", "0", "+devmap",
                                      "zm_zod")));

    if (failures == 0)
        printf("test_launch_args: all passed\n");
    return failures == 0 ? 0 : 1;
}
