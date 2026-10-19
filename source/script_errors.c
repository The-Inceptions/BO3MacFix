// Repeated script errors in console_mp.log.
//
// The Mac build runs the script engines with their developer flag on, so every script runtime error is printed: a
// "throwing script exception" line, then a report with its call stack, about 760 bytes. A mod script that hits the
// same error every frame writes that many times a second for as long as the game runs: All-around Enhancement's zombie
// health bars assign a vector component about 30 times a second, ~80 MB an hour. With QuietScriptErrors each message
// (its numbers left out) is shown in full 3 times a map; after that it's only counted, and once a minute a line says
// how many more there were. The errors themselves, and what the scripts do after them, don't change.
#include <os/lock.h>
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
#define JmpBufs_Size 0x2594
#define JmpBuf_Size 0x94

#define Shown_Times 3
#define Summary_Interval_Ns (60 * 1000000000ULL)
#define Error_Slots 256 // a power of 2
#define Error_Probes 16

static struct {
    uint64_t key;
    uint32_t shown;
    uint32_t hidden;   // since the last summary
    uint32_t pending;  // hidden errors whose report is still to come
    uint64_t summary_at;
} errors[Error_Slots];
static os_unfair_lock errors_lock = OS_UNFAIR_LOCK_INIT;

static uint8_t *var_pub(int inst) {
    return (uint8_t *)(base_address + ADDR_scrVarPub) + (size_t)inst * VarPub_Size;
}

static uint8_t *vm_pub(int inst) {
    return (uint8_t *)(base_address + ADDR_scrVmPub) + (size_t)inst * VmPub_Size;
}

// a message's key: which engine, and its text without the numbers in it (values and positions change between repeats)
static uint64_t error_key(int inst, const char *message) {
    uint64_t hash = 0xcbf29ce484222325ULL ^ (uint64_t)inst;
    for (; *message != '\0'; message++) {
        char c = *message;
        if ((c >= '0' && c <= '9') || c == '.' || c == '-')
            continue;
        hash = (hash ^ (uint8_t)c) * 0x100000001b3ULL;
    }
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

typedef void (*longjmp_t)(void *env, int value) __attribute__((noreturn));
typedef void (*Com_Printf_t)(const char *format, ...);

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
    uint64_t key = error_key(inst, message), now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
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
            errors[slot].pending++;
            if (now >= errors[slot].summary_at) {
                summary = errors[slot].hidden;
                errors[slot].hidden = 0;
                errors[slot].summary_at = now + Summary_Interval_Ns;
            }
        }
    }
    os_unfair_lock_unlock(&errors_lock);
    if (show) {
        orig_Scr_ErrorInternal(inst);
        return;
    }
    if (summary > 0)
        ((Com_Printf_t)(base_address + ADDR_Com_PrintScriptError))(
            "BO3MacFix: %u more script errors like \"%s\" in the last minute, not shown (QuietScriptErrors shows each "
            "message %d times)\n",
            summary, message, Shown_Times);
    int depth = *(int *)(base_address + ADDR_scrJmpBufDepth + (size_t)inst * 4);
    ((longjmp_t)(base_address + ADDR_Stub_longjmp))(
        (uint8_t *)(base_address + ADDR_scrJmpBufs) + (size_t)inst * JmpBufs_Size + (size_t)depth * JmpBuf_Size, -1);
}

// the error report with its call stack, which the script engine prints after catching the throw
install_hook_name(Scr_RuntimeErrorInternal, void, int inst, const char *code_pos, unsigned int index,
                  const char *message, const char *dialog_message) {
    if ((inst == 0 || inst == 1) && message != NULL) {
        uint8_t *vm = vm_pub(inst);
        bool hide = false;
        if (!vm[VmPub_TerminalError] && !vm[VmPub_AbortOnError] && !vm[VmPub_DebugCode]) {
            uint64_t key = error_key(inst, message);
            os_unfair_lock_lock(&errors_lock);
            for (int i = 0; i < Error_Probes; i++) {
                int slot = (int)((key + (uint64_t)i) & (Error_Slots - 1));
                if (errors[slot].key == key) {
                    if (errors[slot].pending > 0) {
                        errors[slot].pending--;
                        hide = true;
                    }
                    break;
                }
                if (errors[slot].key == 0)
                    break;
            }
            os_unfair_lock_unlock(&errors_lock);
        }
        if (hide)
            return;
    }
    orig_Scr_RuntimeErrorInternal(inst, code_pos, index, message, dialog_message);
}

void script_errors_new_map(void) {
    os_unfair_lock_lock(&errors_lock);
    memset(errors, 0, sizeof(errors));
    os_unfair_lock_unlock(&errors_lock);
}

static int env_override(const char *name, int value) {
    const char *env = getenv(name);
    return env != NULL ? atoi(env) : value;
}

void script_errors_init(uint64_t game_base_address, int quiet) {
    base_address = game_base_address;
    if (env_override("BO3MACFIX_QUIETSCRIPTERRORS", quiet) == 0)
        return;
    install_hook_Scr_ErrorInternal((void *)(base_address + ADDR_Scr_ErrorInternal));
    install_hook_Scr_RuntimeErrorInternal((void *)(base_address + ADDR_Scr_RuntimeErrorInternal));
    printf("repeated script errors are shown %d times, then counted\n", Shown_Times);
}
