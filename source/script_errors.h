#pragma once
#include <stdint.h>

// QuietScriptErrors: repeated script errors are shown 3 times, then counted (see script_errors.c)
void script_errors_init(uint64_t game_base_address, int quiet);
// forgets which errors were shown, at a map load
void script_errors_new_map(void);
