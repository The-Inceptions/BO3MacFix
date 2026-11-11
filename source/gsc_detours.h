#pragma once
#include <stdint.h>

// AAEScriptPatches: loads All-around Enhancement 4's patch scripts with the scripts they patch and sends the calls
// of the functions they replace to the replacements (see gsc_detours.c)
void gsc_detours_init(uint64_t game_base_address, int enabled);
