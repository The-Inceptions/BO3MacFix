// FixUIErrorOnRestart: the errors a UI model's subscribers throw aren't shown, as T7Overcharged does on Windows.
//
// When one of the UI's data models changes, the game calls every Lua function subscribed to it (the notifier, see
// offsets.h), each with a protected call, and hands each one that fails to the LUI error handler. The handler pops the
// error off the UI VM's Lua stack and, with ui_error_report on (the game's default), turns it into "UI Error <number>"
// (a hash of the text) through Com_Error: the game's error dialog, which All-around Enhancement shows in its own
// top-left text during a match. The mod's RESTART LEVEL restarts the level and keeps the HUD's Lua, and when the
// restarted game fills the HUD's models again, the subscriptions whose functions are gone are called: "UI Error 74046
// / Attempt to call a nil value", 19 to 31 times in the first seconds, top left after every restart.
//
// T7Overcharged, which the mod loads on Windows, turns the notifier's report off with one byte (its
// FixUIErrorOnRestart: the "je" behind the protected call becomes a "jmp"), always, so Windows players never see
// these. On the Mac a byte patch can't be behind a setting (the pre-patched executable makes all of its byte patches,
// always), and the notifier has no place a detour fits, so this hooks the handler and acts only on the notifier's
// call, which it knows by where that call returns to. Every other report reaches the handler as before (twelve places
// call it: an event handler that fails, a root that can't be made, a file that can't be loaded...), as on Windows.
// Within the notifier every failure is kept off the screen, a subscriber's own error as much as a missing function,
// as T7Overcharged's jump does.
//
// One difference on purpose: T7Overcharged's jump skips the whole handler, the handler's pop of the error with it, so
// on Windows each of these errors stays behind on the UI VM's Lua stack. Here the error is popped as the handler pops
// it, and the game's state afterwards is what its own path leaves, only without the report. log.txt counts the
// errors in lines starting with "UI errors:", with each one's first line, the first 16 times and then at each power of
// two, so a modder still finds them. BO3MACFIX_UIERRORDEBUG=1 logs every one, and every report passed on to the game.
//
// Applies once a mod has loaded T7Overcharged (the stand-in's init has been asked for, see t7o_mod_folder), as on
// Windows; before that, without such a mod, and with the setting off, every error is reported as the game does.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dobby.h"
#include "havok.h"
#include "offsets.h"
#include "t7o_uierror.h"
#include "t7overcharged.h"

static int debug;
static uint64_t game_base;
static const char *(*hks_obj_tolstring)(lua_State *s, HksObject *object, size_t *length);
static atomic_uint hidden_errors;

bool t7o_uierror_hidden(uint64_t return_offset, bool t7o_loaded) {
    return return_offset == ADDR_LUI_NotifySubscriptions_ReportReturn && t7o_loaded;
}

bool t7o_uierror_logged(uint32_t n) {
    return n <= 16 || (n & (n - 1)) == 0;
}

void t7o_uierror_first_line(const char *message, char *out, size_t size) {
    // NULL when the error is neither a string nor a number, or when there is none
    if (message == NULL)
        message = "(no text)";
    size_t length = strcspn(message, "\r\n");
    if (length > size - 1)
        length = size - 1;
    memcpy(out, message, length);
    out[length] = '\0';
}

static bool t7o_loaded(void) {
    return t7o_mod_folder()[0] != '\0';
}

// the game's LUI error handler: void (const char *context, lua_State *). The context isn't looked at: the Mac's
// handler doesn't use it, and some of its callers leave whatever their register held (see offsets.h).
install_hook_name(LUI_ReportError, void, const char *context, lua_State *s) {
    t7o_uierror_report(context, s, (uint64_t)__builtin_return_address(0));
}

void t7o_uierror_report(const char *context, lua_State *s, uint64_t return_address) {
    uint64_t return_offset = return_address - game_base;
    if (!t7o_uierror_hidden(return_offset, t7o_loaded())) {
        if (debug)
            printf("UI errors: the game reports a UI error from its call returning to %#llx\n",
                   (unsigned long long)return_offset);
        orig_LUI_ReportError(context, s);
        return;
    }
    uint32_t n = atomic_fetch_add_explicit(&hidden_errors, 1, memory_order_relaxed) + 1;
    // the handler reads the error on top of the stack when there is one, and pops it either way
    HksObject *error = HKS_TOP(s) - 1;
    if (debug || t7o_uierror_logged(n)) {
        char line[160];
        t7o_uierror_first_line(error >= HKS_BASE(s) ? hks_obj_tolstring(s, error, NULL) : NULL, line, sizeof(line));
        printf("UI errors: a UI model's subscriber failed (\"%s\"); not shown, as T7Overcharged's FixUIErrorOnRestart "
               "does on Windows (%u so far)\n",
               line, n);
    }
    HKS_TOP(s) = error;
}

void t7o_uierror_init(uint64_t game_base_address, int enabled) {
    if (!enabled)
        return;
    const char *env = getenv("BO3MACFIX_UIERRORDEBUG");
    debug = env != NULL ? atoi(env) : 0;
    game_base = game_base_address;
    hks_obj_tolstring = (void *)(game_base_address + ADDR_hks_obj_tolstring);
    install_hook_LUI_ReportError((void *)(game_base_address + ADDR_LUI_ReportError));
    if (debug)
        printf("UI errors: FixUIErrorOnRestart on: a UI model subscriber's error is counted here instead of shown, "
               "from when a mod loads T7Overcharged\n");
}
