// script_errors.c's QuietScriptErrors: which throws and reports it lets through, against made-up script engine memory
// laid out like the game's (the test includes the source itself). A throw longjmps back here, as the game's longjmps
// to the script VM's catch, and the test then reports it, as that catch does.
#include <setjmp.h>
#include <stdio.h>
#include <sys/mman.h>
#include "../source/script_errors.c"

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// the game's memory, from the jmp_bufs to scrVarPub
#define Memory_Start ADDR_scrJmpBufs
#define Memory_End (ADDR_scrVarPub + 2 * VarPub_Size)
static uint8_t *memory;
static uint64_t game(uint64_t address) {
    return (uint64_t)memory - Memory_Start + address;
}

// the game's side of a throw and its report
static jmp_buf vm_catch;
static int throws_shown, throws_hidden, reports_shown;
static char printed[1024];

static void game_throw(int inst) {
    (void)inst;
    throws_shown++;
    longjmp(vm_catch, 1);
}
__attribute__((noreturn)) static void quiet_throw(void *env, int value) {
    // where the hook throws to: the current depth's jmp_buf of that instance
    CHECK(value == -1);
    CHECK(env == (void *)game(ADDR_scrJmpBufs) || env == (void *)(game(ADDR_scrJmpBufs) + JmpBufs_Size));
    throws_hidden++;
    longjmp(vm_catch, 1);
}
static void game_report(int inst, const char *code_pos, unsigned int index, const char *message,
                        const char *dialog_message) {
    (void)inst, (void)code_pos, (void)index, (void)message, (void)dialog_message;
    reports_shown++;
}
static void game_print(const char *format, ...) {
    size_t used = strlen(printed);
    va_list args;
    va_start(args, format);
    vsnprintf(printed + used, sizeof(printed) - used, format, args);
    va_end(args);
}
static void game_eval_equality(int inst, script_value_t *a, script_value_t *b) {
    (void)inst, (void)a, (void)b;
}

int DobbyHook(void *address, void *fake_func, void **out_origin_func) {
    (void)fake_func;
    if ((uint64_t)address == game(ADDR_Scr_ErrorInternal))
        *out_origin_func = (void *)game_throw;
    else if ((uint64_t)address == game(ADDR_Scr_RuntimeErrorInternal))
        *out_origin_func = (void *)game_report;
    else if ((uint64_t)address == game(ADDR_Scr_EvalEquality))
        *out_origin_func = (void *)game_eval_equality;
    else
        CHECK(!"hooked something else");
    return 0;
}

// a non-terminal runtime error inside a script function, thrown at place (the VM's code position then), and its report
// from the VM's catch, which first puts the instruction's start back as the code position (here place - 2)
static void error_at(int inst, const char *message, uint64_t place) {
    *(const char **)(game(ADDR_scrVarPub) + (size_t)inst * VarPub_Size + VarPub_ErrorMessage) = message;
    *(uint64_t *)(game(ADDR_scrVmPos) + (size_t)inst * VmPos_Size) = place;
    if (setjmp(vm_catch) == 0) {
        fake_Scr_ErrorInternal(inst);
        CHECK(!"the throw came back");
    }
    *(uint64_t *)(game(ADDR_scrVmPos) + (size_t)inst * VmPos_Size) = place - 2;
    fake_Scr_RuntimeErrorInternal(inst, (const char *)(place - 2), 0, message, "");
}

static void reset_counts(void) {
    throws_shown = throws_hidden = reports_shown = 0;
    printed[0] = '\0';
}

static void test_places(void) {
    reset_counts();
    // the same error at the same place: shown 3 times, then the throw and its report are both hidden
    for (int i = 0; i < 5; i++)
        error_at(0, "cannot cast vector to int", 0x5000);
    CHECK(throws_shown == 3 && throws_hidden == 2 && reports_shown == 3);
    // the same message from somewhere else is a new error: shown 3 times too (keyed on the message alone, it was hidden)
    reset_counts();
    for (int i = 0; i < 4; i++)
        error_at(0, "cannot cast vector to int", 0x7000);
    CHECK(throws_shown == 3 && throws_hidden == 1 && reports_shown == 3);
    // its numbers aren't part of it: the same place with other values is the same error
    reset_counts();
    error_at(0, "entity 12 is not a player", 0x9000);
    error_at(0, "entity 345 is not a player", 0x9000);
    error_at(0, "entity 6 is not a player", 0x9000);
    error_at(0, "entity 78 is not a player", 0x9000);
    CHECK(throws_shown == 3 && throws_hidden == 1 && reports_shown == 3);
    // the client's script engine has its own errors and its own code position
    reset_counts();
    error_at(1, "cannot cast vector to int", 0x5000);
    CHECK(throws_shown == 1 && throws_hidden == 0 && reports_shown == 1);
}

static void test_reports(void) {
    reset_counts();
    for (int i = 0; i < 4; i++)
        error_at(0, "undefined is not an object", 0x11000);
    CHECK(throws_hidden == 1 && reports_shown == 3);
    // a report that doesn't follow a hidden throw is shown: one that comes another way, the other engine's, and one
    // for another message
    reset_counts();
    *(const char **)(game(ADDR_scrVarPub) + VarPub_ErrorMessage) = "undefined is not an object";
    *(uint64_t *)game(ADDR_scrVmPos) = 0x11000;
    if (setjmp(vm_catch) == 0)
        fake_Scr_ErrorInternal(0);
    CHECK(throws_hidden == 1);
    fake_Scr_RuntimeErrorInternal(1, (const char *)0x11000, 0, "undefined is not an object", "");
    CHECK(reports_shown == 1);
    fake_Scr_RuntimeErrorInternal(0, (const char *)0x11000, 0, "another message", "");
    CHECK(reports_shown == 2);
    // and that one ended the wait: the next report of the hidden message is shown too
    fake_Scr_RuntimeErrorInternal(0, (const char *)0x11000, 0, "undefined is not an object", "");
    CHECK(reports_shown == 3);
    // a terminal error's report is the game's, even after a hidden throw
    reset_counts();
    *(uint64_t *)game(ADDR_scrVmPos) = 0x11000;
    if (setjmp(vm_catch) == 0)
        fake_Scr_ErrorInternal(0);
    CHECK(throws_hidden == 1);
    uint8_t *vm = (uint8_t *)game(ADDR_scrVmPub);
    vm[VmPub_TerminalError] = 1;
    fake_Scr_RuntimeErrorInternal(0, (const char *)0x11000, 0, "undefined is not an object", "");
    vm[VmPub_TerminalError] = 0;
    CHECK(reports_shown == 1);
}

static void test_map_summary(void) {
    script_errors_new_map();
    reset_counts();
    for (int i = 0; i < 5; i++)
        error_at(0, "pair 'a' and 'b' has unmatching types", 0x20000);
    for (int i = 0; i < 4; i++)
        error_at(0, "pair 'a' and 'b' has unmatching types", 0x21000);
    CHECK(throws_hidden == 3);
    printed[0] = '\0';
    script_errors_new_map();
    // a line for each place's hidden errors
    CHECK(strstr(printed, "2 script errors like \"pair 'a' and 'b' has unmatching types\" not shown on the last map") != NULL);
    CHECK(strstr(printed, "1 script errors like \"pair 'a' and 'b' has unmatching types\" not shown on the last map") != NULL);
    // and the new map shows them again
    reset_counts();
    error_at(0, "pair 'a' and 'b' has unmatching types", 0x20000);
    CHECK(throws_shown == 1 && reports_shown == 1);
}

int main(void) {
    memory = mmap(NULL, Memory_End - Memory_Start, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) {
        printf("FAIL: no memory\n");
        return 1;
    }
    setenv("BO3MACFIX_QUIETSCRIPTERRORS", "1", 1);
    script_errors_init(game(0), 1);
    print_script_error = game_print;
    script_longjmp = quiet_throw;
    // both engines with developer on, inside a script function
    for (int inst = 0; inst < 2; inst++) {
        ((uint8_t *)game(ADDR_scrVarPub))[(size_t)inst * VarPub_Size + VarPub_Developer] = 1;
        *(int *)(game(ADDR_scrVmPub) + (size_t)inst * VmPub_Size + VmPub_FunctionCount) = 1;
    }
    test_places();
    test_reports();
    test_map_summary();
    if (failures == 0)
        printf("test_script_errors: all passed\n");
    return failures != 0;
}
