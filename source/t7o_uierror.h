// FixUIErrorOnRestart: the errors a UI model's subscribers throw (after All-around Enhancement's RESTART LEVEL) aren't
// shown, as T7Overcharged does on Windows. See t7o_uierror.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lua_State lua_State;

// whether the LUI error handler's call, by where it returns to (an offset from the game's base), is the
// model-subscription notifier's, and is to be kept off the screen: only once a mod has loaded T7Overcharged
bool t7o_uierror_hidden(uint64_t return_offset, bool t7o_loaded);

// whether the n-th error kept off the screen gets a line in log.txt: the first 16, then each power of two
bool t7o_uierror_logged(uint32_t n);

// the error's text up to its first line break, cut to fit size (a size of at least 1), for a log line; a stand-in
// when there is no text
void t7o_uierror_first_line(const char *message, char *out, size_t size);

// what the hook does with one call of the LUI error handler (context, Lua state), told where the call returns to:
// either the game's handler reports it, or the error is popped (as that handler does first) and only counted
void t7o_uierror_report(const char *context, lua_State *s, uint64_t return_offset);

void t7o_uierror_init(uint64_t game_base_address, int enabled);
