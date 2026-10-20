// Repeated script errors in console_mp.log.
//
// The Mac build runs the script engines with their developer flag on, so every script runtime error is printed: a
// "throwing script exception" line, then a report with its call stack, about 760 bytes. A mod script that hits the
// same error every frame writes that many times a second for as long as the game runs: All-around Enhancement's zombie
// health bars assign a vector component about 30 times a second, ~80 MB an hour. With QuietScriptErrors each error (its
// message, numbers left out, at its place in the scripts) is shown in full 3 times a map; after that it's only counted,
// and once a minute a line says how many more there were. At a map change (and at quit, in log.txt) a line for each
// says how many weren't shown on that map. The errors themselves, and what the scripts do after them, don't change.
#include <os/lock.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dobby.h"
#include "offsets.h"
#include "script_errors.h"

static uint64_t base_address;

#define VarPub_Size 0x78
#define VarPub_Developer 0x8
#define VarPub_DeveloperScript 0x9
#define VarPub_ErrorMessage 0x10
#define VmPub_Size 0xa048
#define VmPub_FunctionCount 0x10
#define VmPub_DebugCode 0x28
#define VmPub_AbortOnError 0x2a
#define VmPub_TerminalError 0x2b
#define VmPos_Size 0x70
#define JmpBufs_Size 0x2594
#define JmpBuf_Size 0x94

#define Shown_Times 3
#define Summary_Interval_Ns (60 * 1000000000ULL)
#define Error_Slots 256 // a power of 2
#define Error_Probes 16

typedef struct {
    uint64_t key;
    uint32_t shown;
    uint32_t hidden;     // since the last summary
    uint32_t hidden_map; // on this map
    uint64_t summary_at;
    char message[96];    // the first hidden one's, for the map's summary
} error_slot_t;
static error_slot_t errors[Error_Slots];
// per script instance, the message key of the error just thrown without a word, whose report comes next: the throw
// longjmps straight to the VM's catch, which reports it before anything else runs on that instance
static uint64_t hidden_report[2];
static os_unfair_lock errors_lock = OS_UNFAIR_LOCK_INIT;
static bool quiet_on;

typedef void (*longjmp_t)(void *env, int value) __attribute__((noreturn));
typedef void (*Com_Printf_t)(const char *format, ...);
static Com_Printf_t print_script_error;
static longjmp_t script_longjmp;

static uint8_t *var_pub(int inst) {
    return (uint8_t *)(base_address + ADDR_scrVarPub) + (size_t)inst * VarPub_Size;
}

static uint8_t *vm_pub(int inst) {
    return (uint8_t *)(base_address + ADDR_scrVmPub) + (size_t)inst * VmPub_Size;
}

// a message's key: which engine, and its text without the numbers in it (values and positions change between repeats)
static uint64_t message_key(int inst, const char *message) {
    uint64_t hash = 0xcbf29ce484222325ULL ^ (uint64_t)inst;
    for (; *message != '\0'; message++) {
        char c = *message;
        if ((c >= '0' && c <= '9') || c == '.' || c == '-')
            continue;
        hash = (hash ^ (uint8_t)c) * 0x100000001b3ULL;
    }
    return hash != 0 ? hash : 1;
}

// an error's key: its message's, and its place, the code position the script engine had reached when it threw (the
// same each time for the same place: the instruction, and what its handler had read of it), so a message seen 3 times
// already still shows from somewhere else
static uint64_t error_key(uint64_t message, uint64_t place) {
    uint64_t hash = message;
    for (int i = 0; i < 8; i++, place >>= 8)
        hash = (hash ^ (place & 0xff)) * 0x100000001b3ULL;
    return hash != 0 ? hash : 1;
}

// the slot holding key, or a free one for it, or -1 if the table is full there
static int error_slot(uint64_t key) {
    for (int i = 0; i < Error_Probes; i++) {
        int slot = (int)((key + (uint64_t)i) & (Error_Slots - 1));
        if (errors[slot].key == key || errors[slot].key == 0) {
            errors[slot].key = key;
            return slot;
        }
    }
    return -1;
}

// Scr_ErrorInternal throws a runtime error to the script engine (a longjmp), printing it first while developer is on.
// For a repeat, throw it without printing, the way it does with developer off.
install_hook_name(Scr_ErrorInternal, void, int inst) {
    if (inst != 0 && inst != 1) {
        orig_Scr_ErrorInternal(inst);
        return;
    }
    uint8_t *var = var_pub(inst), *vm = vm_pub(inst);
    const char *message = *(const char **)(var + VarPub_ErrorMessage);
    // only the case where it prints "throwing script exception" and throws: not terminal, inside a script function
    if (message == NULL || !var[VarPub_Developer] || var[VarPub_DeveloperScript] || vm[VmPub_TerminalError] ||
        vm[VmPub_AbortOnError] || vm[VmPub_DebugCode] || *(int *)(vm + VmPub_FunctionCount) == 0) {
        orig_Scr_ErrorInternal(inst);
        return;
    }
    uint64_t text = message_key(inst, message), now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t key = error_key(text, *(const uint64_t *)(base_address + ADDR_scrVmPos + (size_t)inst * VmPos_Size));
    uint32_t summary = 0;
    bool show = true;
    os_unfair_lock_lock(&errors_lock);
    int slot = error_slot(key);
    if (slot >= 0) {
        if (errors[slot].shown < Shown_Times) {
            if (++errors[slot].shown == Shown_Times)
                errors[slot].summary_at = now + Summary_Interval_Ns;
        } else {
            show = false;
            errors[slot].hidden++;
            errors[slot].hidden_map++;
            if (errors[slot].message[0] == '\0')
                strlcpy(errors[slot].message, message, sizeof(errors[slot].message));
            if (now >= errors[slot].summary_at) {
                summary = errors[slot].hidden;
                errors[slot].hidden = 0;
                errors[slot].summary_at = now + Summary_Interval_Ns;
            }
        }
    }
    hidden_report[inst] = show ? 0 : text;
    os_unfair_lock_unlock(&errors_lock);
    if (show) {
        orig_Scr_ErrorInternal(inst);
        return;
    }
    if (summary > 0)
        print_script_error("BO3MacFix: %u more script errors like \"%s\" in the last minute, not shown (QuietScriptErrors "
                           "shows each error %d times at each place)\n",
                           summary, message, Shown_Times);
    int depth = *(int *)(base_address + ADDR_scrJmpBufDepth + (size_t)inst * 4);
    script_longjmp((uint8_t *)(base_address + ADDR_scrJmpBufs) + (size_t)inst * JmpBufs_Size + (size_t)depth * JmpBuf_Size,
                   -1);
}

// the error report with its call stack, which the script engine prints after catching the throw
install_hook_name(Scr_RuntimeErrorInternal, void, int inst, const char *code_pos, unsigned int index,
                  const char *message, const char *dialog_message) {
    if (inst == 0 || inst == 1) {
        // the report that comes after a hidden throw is that throw's, the same message (checked anyway); any report
        // ends the wait for one, so a throw that somehow isn't reported can't hide another error's
        os_unfair_lock_lock(&errors_lock);
        uint64_t hidden = hidden_report[inst];
        hidden_report[inst] = 0;
        os_unfair_lock_unlock(&errors_lock);
        uint8_t *vm = vm_pub(inst);
        if (hidden != 0 && message != NULL && !vm[VmPub_TerminalError] && !vm[VmPub_AbortOnError] &&
            !vm[VmPub_DebugCode] && message_key(inst, message) == hidden)
            return;
    }
    orig_Scr_RuntimeErrorInternal(inst, code_pos, index, message, dialog_message);
}

static void print_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

// empties the table, then says how many of each message weren't shown since it was last emptied: in console_mp.log
// with the errors at a map change, in log.txt at quit (the game may have closed its console log by then)
static void summarize_and_forget(bool quitting) {
    error_slot_t *copy = quiet_on ? malloc(sizeof(errors)) : NULL;
    os_unfair_lock_lock(&errors_lock);
    if (copy != NULL)
        memcpy(copy, errors, sizeof(errors));
    memset(errors, 0, sizeof(errors));
    memset(hidden_report, 0, sizeof(hidden_report));
    os_unfair_lock_unlock(&errors_lock);
    if (copy == NULL)
        return;
    Com_Printf_t print = quitting ? print_log : print_script_error;
    for (int i = 0; i < Error_Slots; i++) {
        const error_slot_t *e = &copy[i];
        if (e->hidden_map == 0)
            continue;
        if (e->hidden == e->hidden_map)
            print("BO3MacFix: %u script errors like \"%s\" not shown %s (QuietScriptErrors)\n", e->hidden_map,
                  e->message, quitting ? "before quitting" : "on the last map");
        else
            print("BO3MacFix: %u script errors like \"%s\" not shown %s, %u since the last summary (QuietScriptErrors)\n",
                  e->hidden_map, e->message, quitting ? "before quitting" : "on the last map", e->hidden);
    }
    free(copy);
}

void script_errors_new_map(void) {
    summarize_and_forget(false);
}

void script_errors_quit(void) {
    summarize_and_forget(true);
}

// GSC's == and != throw "pair '...' and '...' has unmatching types" for a string against a number here. All-around
// Enhancement depends on them not throwing, as they apparently don't on Windows: zmsavedata.gsc's check of its
// "disables the bank" options passes getdvarint() the string "tfoption_start_points" != 500, so on the Mac banks.gsc's
// init dies on it and the bank, the weapon locker and the careers never get their saved data ("New Save System NOT
// loaded!"). Answer those the way the === and !== wrappers answer any mismatch: free both values, then false (which
// != turns into true).
#define Var_String 2
#define Var_Float 6
#define Var_Int 7

typedef struct {
    union {
        int int_value;
        uint64_t raw;
    } u;
    int type;
} script_value_t;

static void (*Scr_FreeValuePair)(int inst, script_value_t *a, script_value_t *b);

static bool is_number(int type) {
    return type == Var_Int || type == Var_Float;
}

install_hook_name(Scr_EvalEquality, void, int inst, script_value_t *a, script_value_t *b) {
    if ((a->type == Var_String && is_number(b->type)) || (is_number(a->type) && b->type == Var_String)) {
        Scr_FreeValuePair(inst, a, b);
        a->type = Var_Int;
        a->u.int_value = 0;
        return;
    }
    orig_Scr_EvalEquality(inst, a, b);
}

static int env_override(const char *name, int value) {
    const char *env = getenv(name);
    return env != NULL ? atoi(env) : value;
}

void script_errors_init(uint64_t game_base_address, int quiet) {
    base_address = game_base_address;
    Scr_FreeValuePair = (void *)(base_address + ADDR_Scr_FreeValuePair);
    print_script_error = (Com_Printf_t)(base_address + ADDR_Com_PrintScriptError);
    script_longjmp = (longjmp_t)(base_address + ADDR_Stub_longjmp);
    install_hook_Scr_EvalEquality((void *)(base_address + ADDR_Scr_EvalEquality));
    if (env_override("BO3MACFIX_QUIETSCRIPTERRORS", quiet) == 0)
        return;
    install_hook_Scr_ErrorInternal((void *)(base_address + ADDR_Scr_ErrorInternal));
    install_hook_Scr_RuntimeErrorInternal((void *)(base_address + ADDR_Scr_RuntimeErrorInternal));
    quiet_on = true;
    printf("repeated script errors are shown %d times, then counted\n", Shown_Times);
}
