// What the game's command line asks for at launch, read from the process's arguments. No game or macOS calls, so
// tests/test_launch_args.c tests it as is.
#pragma once
#include <stdbool.h>

// whether the command line starts a game rather than the title screen. The engine splits its command line at each
// '+' and holds back a line that begins with "map" or "devmap", which it runs once it has started up; "connect"
// joins a server. Each of those starts loading a map at launch, so the logo movies aren't just in front of the title
// screen there (see SkipIntro in bo3macfix.c).
bool launch_args_start_game(int argc, char *const argv[]);
