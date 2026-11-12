#pragma once
#include <stdbool.h>
#include <stdint.h>

// QuietScriptErrors: repeated script errors are shown 3 times, then counted (see script_errors.c)
void script_errors_init(uint64_t game_base_address, int quiet);
// forgets which errors were shown, at a map load, and logs how many of each weren't
void script_errors_new_map(void);
// logs how many of each weren't shown, at quit
void script_errors_quit(void);
// the script errors thrown since the last call, server and client (counted while QuietScriptErrors is on, which
// it returns), for FrameStats' detail (BO3MACFIX_FRAMESTATSDETAIL=1)
bool script_errors_take_thrown(uint32_t *server, uint32_t *client);
// how many of a script instance's variables are in use and how many it has (0 the server's, 1 the client's), for
// FrameStats' detail. A game that runs out of them stops with a script error. All-around Enhancement's Windows
// plugin raises the server's from 130,000 to 524,288; here they stay as the game has them.
uint32_t script_variables_used(int inst, uint32_t *limit);
