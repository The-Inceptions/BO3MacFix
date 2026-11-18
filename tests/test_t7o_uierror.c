// t7o_uierror.c: which of the LUI error handler's calls are kept off the screen, what the Lua stack looks like after
// one (popped as the game's handler pops it, nothing else touched), the log's rule and what init installs, against a
// made-up Lua state and a made-up handler (the test includes the source itself).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#include "../source/t7o_uierror.c"

// ---- what the code asks of the rest of BO3MacFix

static char mod_folder[64]; // "" until a mod has loaded T7Overcharged
const char *t7o_mod_folder(void) {
    return mod_folder;
}
static void *hooked_address, *hooked_handler;
static int hooks;
int BO3MacFix_Hook(void *address, void *fake_func, void **out_origin_func) {
    (void)out_origin_func;
    hooked_address = address;
    hooked_handler = fake_func;
    hooks++;
    return 0;
}

// ---- the made-up game

#define Game_Base 0x100000000ULL
// where each of the game's twelve calls of its LUI error handler returns to (offsets from the base); the fifth is the
// model-subscription notifier's
static const uint64_t report_returns[] = { 0xebede5,  0xebfc31,  0xec288e,  0xec43d2,  0x1036875, 0x11c8f96,
                                           0x140696f, 0x1408953, 0x1408a07, 0x1409afb, 0x152965e, 0x1529769 };
#define Notifier_Return 0x1036875

// a Lua state: only its top (+0x48) and base (+0x50) are read, through havok.h's macros
static _Alignas(16) uint8_t state_bytes[0x100];
static lua_State *const state = (lua_State *)state_bytes;
static HksObject stack[8];

// fills the stack with values that tell themselves apart and puts base and top at those slots
static void set_stack(int base, int top) {
    for (int i = 0; i < 8; i++) {
        stack[i].t = HKS_TSTRING;
        stack[i].v.ptr = (void *)(uintptr_t)(0x1000 + i);
    }
    HKS_BASE(state) = &stack[base];
    HKS_TOP(state) = &stack[top];
}
static bool stack_untouched(void) {
    for (int i = 0; i < 8; i++)
        if (stack[i].t != HKS_TSTRING || stack[i].v.ptr != (void *)(uintptr_t)(0x1000 + i))
            return false;
    return true;
}

// the game's handler: counts its calls and what it got
static int handler_calls;
static const char *handler_context;
static lua_State *handler_state;
static void game_handler(const char *context, lua_State *s) {
    handler_calls++;
    handler_context = context;
    handler_state = s;
}

// the game's hks_obj_tolstring: the error's text, as the game's protected call left it for a missing function
static int tolstring_calls;
static HksObject *tolstring_object;
static lua_State *tolstring_state;
static size_t *tolstring_length;
static const char *tolstring_answer = "Attempt to call a nil value\nstack traceback:";
static const char *game_tolstring(lua_State *s, HksObject *object, size_t *length) {
    tolstring_calls++;
    tolstring_state = s;
    tolstring_object = object;
    tolstring_length = length;
    return tolstring_answer;
}

static void reset_calls(void) {
    handler_calls = tolstring_calls = 0;
    handler_context = NULL;
    handler_state = NULL;
    tolstring_object = NULL;
    tolstring_state = NULL;
    tolstring_length = (size_t *)1;
}

static void test_which_calls(void) {
    // only the notifier's call, and only once a mod has loaded T7Overcharged
    CHECK(t7o_uierror_hidden(Notifier_Return, true));
    CHECK(!t7o_uierror_hidden(Notifier_Return, false));
    int hidden = 0;
    for (size_t i = 0; i < sizeof(report_returns) / sizeof(report_returns[0]); i++)
        hidden += t7o_uierror_hidden(report_returns[i], true);
    CHECK(hidden == 1);
    CHECK(!t7o_uierror_hidden(Notifier_Return - 1, true) && !t7o_uierror_hidden(Notifier_Return + 1, true));
    CHECK(!t7o_uierror_hidden(Notifier_Return + Game_Base, true) && !t7o_uierror_hidden(0, true));
}

static void test_log_rule(void) {
    // the first 16, then 32, 64, 128...
    int lines = 0;
    for (uint32_t n = 1; n <= 100000; n++)
        lines += t7o_uierror_logged(n);
    CHECK(lines == 16 + 12);
    CHECK(t7o_uierror_logged(1) && t7o_uierror_logged(16) && !t7o_uierror_logged(17) && t7o_uierror_logged(32) &&
          !t7o_uierror_logged(48) && t7o_uierror_logged(65536) && !t7o_uierror_logged(65537));
}

static void test_first_line(void) {
    char line[160];
    t7o_uierror_first_line("Attempt to call a nil value\nstack traceback:", line, sizeof(line));
    CHECK(strcmp(line, "Attempt to call a nil value") == 0);
    t7o_uierror_first_line("one\r\ntwo", line, sizeof(line));
    CHECK(strcmp(line, "one") == 0);
    t7o_uierror_first_line("", line, sizeof(line));
    CHECK(strcmp(line, "") == 0);
    t7o_uierror_first_line(NULL, line, sizeof(line));
    CHECK(strcmp(line, "(no text)") == 0);
    // cut to fit, with its end in the buffer: a text of 159 fits whole, 160 loses its last letter
    char text[400];
    memset(text, 'a', sizeof(text));
    text[159] = '\0';
    t7o_uierror_first_line(text, line, sizeof(line));
    CHECK(strlen(line) == 159);
    text[159] = 'b';
    text[160] = '\0';
    t7o_uierror_first_line(text, line, sizeof(line));
    CHECK(strlen(line) == 159 && line[158] == 'a');
    text[160] = 'a';
    text[399] = '\0';
    char small[4];
    t7o_uierror_first_line(text, small, sizeof(small));
    CHECK(strcmp(small, "aaa") == 0);
    t7o_uierror_first_line(text, small, 1);
    CHECK(small[0] == '\0');
}

static void test_report(void) {
    orig_LUI_ReportError = game_handler;
    hks_obj_tolstring = game_tolstring;
    game_base = Game_Base;
    debug = 0;
    const char *context = (const char *)(uintptr_t)0x10; // never read: some of the game's callers leave junk there

    // without the mod every error goes to the game's handler, with the stack as it is
    mod_folder[0] = '\0';
    set_stack(1, 4);
    reset_calls();
    t7o_uierror_report(context, state, Game_Base + Notifier_Return);
    CHECK(handler_calls == 1 && handler_context == context && handler_state == state);
    CHECK(HKS_TOP(state) == &stack[4] && HKS_BASE(state) == &stack[1] && stack_untouched() && tolstring_calls == 0);
    CHECK(atomic_load(&hidden_errors) == 0);

    // with it, every call but the notifier's still goes there
    strcpy(mod_folder, "/mods/aae/T7Overcharged");
    for (size_t i = 0; i < sizeof(report_returns) / sizeof(report_returns[0]); i++) {
        if (report_returns[i] == Notifier_Return)
            continue;
        reset_calls();
        t7o_uierror_report(context, state, Game_Base + report_returns[i]);
        CHECK(handler_calls == 1 && handler_context == context && handler_state == state);
        CHECK(HKS_TOP(state) == &stack[4] && stack_untouched() && tolstring_calls == 0);
    }
    CHECK(atomic_load(&hidden_errors) == 0);

    // the notifier's: not reported; the error on top is read (it is logged) and popped, nothing else changes
    reset_calls();
    t7o_uierror_report(context, state, Game_Base + Notifier_Return);
    CHECK(handler_calls == 0);
    CHECK(HKS_TOP(state) == &stack[3] && HKS_BASE(state) == &stack[1] && stack_untouched());
    CHECK(tolstring_calls == 1 && tolstring_object == &stack[3] && tolstring_state == state && tolstring_length == NULL);
    CHECK(atomic_load(&hidden_errors) == 1);

    // the same place relative to another base is another place
    game_base = Game_Base + 0x1000;
    reset_calls();
    t7o_uierror_report(context, state, Game_Base + Notifier_Return);
    CHECK(handler_calls == 1 && HKS_TOP(state) == &stack[3]);
    t7o_uierror_report(context, state, Game_Base + 0x1000 + Notifier_Return);
    CHECK(handler_calls == 1 && HKS_TOP(state) == &stack[2] && atomic_load(&hidden_errors) == 2);
    game_base = Game_Base;

    // the log: the error is only read for the errors that get a line, the first 16 and then each power of two; the
    // pop is the same for every one
    set_stack(0, 8);
    int read = 0;
    for (uint32_t n = 3; n <= 40; n++) {
        if (HKS_TOP(state) == &stack[0])
            set_stack(0, 8);
        HksObject *before = HKS_TOP(state);
        reset_calls();
        t7o_uierror_report(context, state, Game_Base + Notifier_Return);
        CHECK(HKS_TOP(state) == before - 1 && handler_calls == 0);
        CHECK(tolstring_calls == (t7o_uierror_logged(n) ? 1 : 0));
        read += tolstring_calls;
    }
    CHECK(read == 14 + 1 && atomic_load(&hidden_errors) == 40 && stack_untouched());
    // with the debug switch every one is read and logged
    debug = 1;
    set_stack(0, 8);
    reset_calls();
    t7o_uierror_report(context, state, Game_Base + Notifier_Return);
    CHECK(tolstring_calls == 1 && atomic_load(&hidden_errors) == 41 && HKS_TOP(state) == &stack[7]);
    debug = 0;

    // an error that isn't a text: the handler's read gives NULL, the line says so, the pop is the same
    tolstring_answer = NULL;
    set_stack(1, 3);
    reset_calls();
    debug = 1;
    t7o_uierror_report(context, state, Game_Base + Notifier_Return);
    debug = 0;
    CHECK(handler_calls == 0 && tolstring_calls == 1 && tolstring_object == &stack[2] && HKS_TOP(state) == &stack[2]);
    tolstring_answer = "Attempt to call a nil value\nstack traceback:";

    // nothing above the base: nothing is read, and the top goes down all the same, as the game's handler takes it down
    set_stack(1, 1);
    reset_calls();
    debug = 1;
    t7o_uierror_report(context, state, Game_Base + Notifier_Return);
    debug = 0;
    CHECK(handler_calls == 0 && tolstring_calls == 0 && HKS_TOP(state) == &stack[0] && stack_untouched());

    // the hook itself: called from here it returns into this program, which with a base of 0 is far from the
    // notifier's offset (the program is mapped above 4 GB), so the game's handler reports it
    game_base = 0;
    set_stack(1, 4);
    reset_calls();
    fake_LUI_ReportError(context, state);
    CHECK(handler_calls == 1 && handler_context == context && handler_state == state && HKS_TOP(state) == &stack[4]);
    game_base = Game_Base;
}

static void test_init(void) {
    // off: nothing is hooked, nothing set
    hooks = 0;
    game_base = 0;
    hks_obj_tolstring = NULL;
    t7o_uierror_init(Game_Base, 0);
    CHECK(hooks == 0 && game_base == 0 && hks_obj_tolstring == NULL);
    // on: the handler's start, with the hook; the game's own read of the error
    unsetenv("BO3MACFIX_UIERRORDEBUG");
    t7o_uierror_init(Game_Base, 1);
    CHECK(hooks == 1 && hooked_address == (void *)(uintptr_t)(Game_Base + ADDR_LUI_ReportError) &&
          hooked_handler == (void *)fake_LUI_ReportError);
    CHECK(game_base == Game_Base && (uintptr_t)hks_obj_tolstring == Game_Base + ADDR_hks_obj_tolstring && debug == 0);
    setenv("BO3MACFIX_UIERRORDEBUG", "1", 1);
    t7o_uierror_init(Game_Base, 1);
    CHECK(debug == 1);
    unsetenv("BO3MACFIX_UIERRORDEBUG");
    // the places it relies on (the hook's bytes are in prepatch.c's table, checked against the game's file)
    CHECK(ADDR_LUI_ReportError == 0x1528b3c && ADDR_LUI_NotifySubscriptions_ReportReturn == 0x1036875);
}

int main(void) {
    test_which_calls();
    test_log_rule();
    test_first_line();
    test_report();
    test_init();
    if (failures != 0) {
        printf("test_t7o_uierror: %d failures\n", failures);
        return 1;
    }
    printf("test_t7o_uierror: all passed\n");
    return 0;
}
