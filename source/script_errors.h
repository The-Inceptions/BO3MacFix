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
