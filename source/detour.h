// Detours at places in the middle of the game's functions.
//
// A hook (dobby.h) replaces a function: the game jumps to ours at the function's first instruction, and ours calls
// the original when it wants to. The pre-patched executable's stubs don't care that the place is a function's start,
// though: any place with 5 to 10 bytes of whole instructions that don't refer to their own address can be given one
// (prepatch.c's hook_defs). What the stub jumps to then can't be a C function, because the game is in the middle of
// something: its registers hold values, and nothing was called, so there is no return address. So the stub jumps to
// a few instructions of assembly (a thunk) made by the macros here, which call a C function and then jump on: to the
// instructions the stub's jump replaced and from there back into the game, or to another place in the game.
//
// Only x86_64 has the thunks. On anything else (the unit tests are built for the Mac's own CPU) the macros leave the
// C functions, which the tests call themselves.
//
// These only work from the pre-patched executable: the hooking library would have to move the instructions at the
// place itself, and it takes 14 bytes when it can't put its own code near, more than these places have.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// what the game's registers held at the detoured place. The handler may change them.
typedef struct {
    uint8_t xmm[16][16];
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax, rflags;
} detour_registers_t;

// the game's stack pointer at the place, for what the function keeps on its stack
#define Detour_Red_Zone 128
static inline uint8_t *detour_stack(const detour_registers_t *registers) {
    return (uint8_t *)(registers + 1) + Detour_Red_Zone;
}

#if defined(__x86_64__)

// Saves every register, calls the handler with them, puts them back (as the handler left them) and goes on with the
// game's own instructions. The stack below the game's stack pointer is left alone for 128 bytes, in case the game's
// function keeps something there, and the handler is called with the stack aligned as a call needs it.
#define Detour_Save_All                                                                                                \
    "lea -128(%rsp), %rsp\n pushfq\n"                                                                                  \
    "push %rax\n push %rcx\n push %rdx\n push %rbx\n push %rbp\n push %rsi\n push %rdi\n"                              \
    "push %r8\n push %r9\n push %r10\n push %r11\n push %r12\n push %r13\n push %r14\n push %r15\n"                    \
    "sub $256, %rsp\n"                                                                                                 \
    "movups %xmm0, 0(%rsp)\n movups %xmm1, 16(%rsp)\n movups %xmm2, 32(%rsp)\n movups %xmm3, 48(%rsp)\n"               \
    "movups %xmm4, 64(%rsp)\n movups %xmm5, 80(%rsp)\n movups %xmm6, 96(%rsp)\n movups %xmm7, 112(%rsp)\n"             \
    "movups %xmm8, 128(%rsp)\n movups %xmm9, 144(%rsp)\n movups %xmm10, 160(%rsp)\n movups %xmm11, 176(%rsp)\n"        \
    "movups %xmm12, 192(%rsp)\n movups %xmm13, 208(%rsp)\n movups %xmm14, 224(%rsp)\n movups %xmm15, 240(%rsp)\n"      \
    "mov %rsp, %rdi\n mov %rsp, %rbx\n and $-16, %rsp\n"
#define Detour_Restore_All                                                                                             \
    "mov %rbx, %rsp\n"                                                                                                 \
    "movups 0(%rsp), %xmm0\n movups 16(%rsp), %xmm1\n movups 32(%rsp), %xmm2\n movups 48(%rsp), %xmm3\n"               \
    "movups 64(%rsp), %xmm4\n movups 80(%rsp), %xmm5\n movups 96(%rsp), %xmm6\n movups 112(%rsp), %xmm7\n"             \
    "movups 128(%rsp), %xmm8\n movups 144(%rsp), %xmm9\n movups 160(%rsp), %xmm10\n movups 176(%rsp), %xmm11\n"        \
    "movups 192(%rsp), %xmm12\n movups 208(%rsp), %xmm13\n movups 224(%rsp), %xmm14\n movups 240(%rsp), %xmm15\n"      \
    "add $256, %rsp\n"                                                                                                 \
    "pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %r11\n pop %r10\n pop %r9\n pop %r8\n"                            \
    "pop %rdi\n pop %rsi\n pop %rbp\n pop %rbx\n pop %rdx\n pop %rcx\n pop %rax\n"                                     \
    "popfq\n lea 128(%rsp), %rsp\n"

// detour_with_registers(name) { ... registers ... }: the handler for a place anywhere in a function. After it the
// game goes on as if nothing had happened, with the registers as the handler left them.
// install_detour_##name(address) points the place's stub at it.
#define detour_with_registers(name)                                                                                    \
    static void *orig_##name __attribute__((used));                                                                    \
    __attribute__((used)) static void handler_##name(detour_registers_t *registers);                                   \
    __attribute__((naked, used)) static void thunk_##name(void) {                                                      \
        __asm__(Detour_Save_All "call _handler_" #name "\n" Detour_Restore_All "jmp *_orig_" #name "(%rip)\n");        \
    }                                                                                                                  \
    static void install_detour_##name(void *address) {                                                                 \
        DobbyHook(address, (void *)thunk_##name, &orig_##name);                                                        \
    }                                                                                                                  \
    static void handler_##name(detour_registers_t *registers)

// detour_before_test(name) { ... return whether the game's test is to count as passed ... }: for a place where the
// game is about to call a test and to jump away if it says no,
//     mov edi, <argument> ; call <test> ; test al, al ; je <no>
// with the detour on the "mov". When the handler says yes the game goes on behind the "je" without calling its test;
// otherwise the game's own instructions run as they are. Every register a function may change is unused at such a
// place (the call is about to change them), so none is saved, and the stack is aligned for a call already.
// install_detour_##name(address, yes) takes the place and where the game goes on when the test passes.
#define detour_before_test(name)                                                                                       \
    static void *orig_##name __attribute__((used));                                                                    \
    static void *yes_##name __attribute__((used));                                                                     \
    __attribute__((used)) static bool handler_##name(void);                                                            \
    __attribute__((naked, used)) static void thunk_##name(void) {                                                      \
        __asm__("call _handler_" #name "\n test %al, %al\n jz 1f\n jmp *_yes_" #name "(%rip)\n"                        \
                "1: jmp *_orig_" #name "(%rip)\n");                                                                    \
    }                                                                                                                  \
    static void install_detour_##name(void *address, void *yes) {                                                      \
        yes_##name = yes;                                                                                              \
        DobbyHook(address, (void *)thunk_##name, &orig_##name);                                                        \
    }                                                                                                                  \
    static bool handler_##name(void)

#else

// without the thunks: the handlers, and install functions that hand the handler itself to the hooking function, so a
// test sees what would be detoured
#define detour_with_registers(name)                                                                                    \
    static void *orig_##name;                                                                                          \
    static void handler_##name(detour_registers_t *registers);                                                         \
    __attribute__((unused)) static void install_detour_##name(void *address) {                                         \
        DobbyHook(address, (void *)handler_##name, &orig_##name);                                                      \
    }                                                                                                                  \
    static void handler_##name(detour_registers_t *registers)

#define detour_before_test(name)                                                                                       \
    static void *orig_##name, *yes_##name;                                                                             \
    static bool handler_##name(void);                                                                                  \
    __attribute__((unused)) static void install_detour_##name(void *address, void *yes) {                              \
        yes_##name = yes;                                                                                              \
        DobbyHook(address, (void *)handler_##name, &orig_##name);                                                      \
    }                                                                                                                  \
    static bool handler_##name(void)

#endif
