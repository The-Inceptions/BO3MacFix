// What the game's command line asks for at launch (launch_args.h).
#include "launch_args.h"
#include <string.h>
#include <strings.h>

bool launch_args_start_game(int argc, char *const argv[]) {
    // argv[0] is the executable. Aspyr's layer joins the rest with spaces, so a '+' anywhere starts a line: one
    // argument each ("+map", "zm_zod") or all in one ("+set fs_game 2631943123 +map zm_zod").
    for (int i = 1; i < argc; i++) {
        if (argv[i] == NULL)
            continue;
        for (const char *plus = strchr(argv[i], '+'); plus != NULL; plus = strchr(plus + 1, '+')) {
            // like the engine, the start of the line: "map" also covers "map_restart", which is fine here
            const char *line = plus + 1;
            if (strncasecmp(line, "map", 3) == 0 || strncasecmp(line, "devmap", 6) == 0 ||
                strncasecmp(line, "connect", 7) == 0)
                return true;
        }
    }
    return false;
}
