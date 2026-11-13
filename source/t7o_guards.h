#pragma once
#include <stdbool.h>
#include <stdint.h>

// T7OCrashGuards: two of T7Overcharged's fixes for crashes of the game itself, which All-around Enhancement's Windows
// players have once the mod has loaded T7Overcharged (see t7o_guards.c)
void t7o_guards_init(uint64_t game_base_address, int enabled);

// the weapon guard: the attachment entry the game is to read override bits from, given the one it loaded (entry,
// rax at the place), which word of the bits it reads (rcx there) and whether the mod has loaded T7Overcharged.
// The entry itself when it's there; a stand-in that overrides nothing in its place when it's missing, the mod is
// loaded and the word is one of the stand-in's; NULL (the game's own entry, which crashes it) otherwise.
const void *t7o_guards_attachment_entry(const void *entry, uint64_t word, bool t7o_loaded);

// the bounds guard: whether Com_LocalClient_GetControllerIndex may look local_client up in the game's table of the
// two local clients (0 and 1); for any other number it answers -1, no controller
bool t7o_guards_local_client_in_table(int32_t local_client);

// whether the n-th catch of a guard gets a line in the log: the first 16, then every power of two
bool t7o_guards_logged(uint32_t n);
