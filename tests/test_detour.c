// detour.h's thunks, against two made-up game functions written in assembly. Built for x86_64 only (the thunks are
// x86_64 code), so this one runs under Rosetta on an Apple CPU.
//
// What stands in for the pre-patched executable: each made-up function has, at its detoured place, the jump through
// a slot that a stub would do, and a label with the instructions that jump replaced followed by the jump back.
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

// the stubs' slots and the places' own instructions, by the address a detour is installed at
void *slot_registers, *slot_test, *slot_choice;
extern char site_registers[], moved_registers[], site_test[], moved_test[], yes_of_test[];
extern char site_choice[], moved_choice[], other_of_choice[];
static int DobbyHook(void *address, void *fake_func, void **out_origin_func) {
    if (address == (void *)site_registers) {
        *out_origin_func = moved_registers;
        slot_registers = fake_func;
    } else if (address == (void *)site_test) {
        *out_origin_func = moved_test;
        slot_test = fake_func;
    } else if (address == (void *)site_choice) {
        *out_origin_func = moved_choice;
        slot_choice = fake_func;
    } else {
        return -1;
    }
    return 0;
}
#include "../source/detour.h"

// ---- a place in the middle of a function, with every register in use

// what the function had in its registers after the place (and the flags, and what it keeps below its stack pointer)
struct seen {
    uint64_t rax, rcx, rdx, rbx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rflags, below_stack, rsp_before,
        rsp_after, rbp;
    uint8_t xmm[16][16];
} seen;
uint8_t xmm_values[16][16];

// game_registers(crooked): loads known values everywhere, goes through the place and writes down what it has then.
// With crooked set its stack pointer isn't a multiple of 16 at the place.
void game_registers(int crooked);
__asm__(".globl _game_registers, _site_registers, _moved_registers\n"
        "_game_registers:\n"
        "  push %rbp\n mov %rsp, %rbp\n push %rbx\n push %r12\n push %r13\n push %r14\n push %r15\n"
        "  sub $8, %rsp\n"                 // a multiple of 16 now
        "  test %edi, %edi\n jz 1f\n sub $8, %rsp\n 1:\n"
        "  lea _xmm_values(%rip), %rax\n"
        "  movups 0(%rax), %xmm0\n movups 16(%rax), %xmm1\n movups 32(%rax), %xmm2\n movups 48(%rax), %xmm3\n"
        "  movups 64(%rax), %xmm4\n movups 80(%rax), %xmm5\n movups 96(%rax), %xmm6\n movups 112(%rax), %xmm7\n"
        "  movups 128(%rax), %xmm8\n movups 144(%rax), %xmm9\n movups 160(%rax), %xmm10\n movups 176(%rax), %xmm11\n"
        "  movups 192(%rax), %xmm12\n movups 208(%rax), %xmm13\n movups 224(%rax), %xmm14\n movups 240(%rax), %xmm15\n"
        "  movabs $0x1122334455667788, %rax\n mov %rax, -8(%rsp)\n" // below the stack pointer
        "  mov %rsp, _seen+128(%rip)\n"    // rsp_before
        "  mov $0xa1, %rax\n mov $0xc1, %rcx\n mov $0xd1, %rdx\n mov $0xb1, %rbx\n mov $0x51, %rsi\n mov $0xd2, %rdi\n"
        "  mov $0x08, %r8\n mov $0x09, %r9\n mov $0x10, %r10\n mov $0x11, %r11\n mov $0x12, %r12\n mov $0x13, %r13\n"
        "  mov $0x14, %r14\n mov $0x15, %r15\n"
        "  stc\n"                          // a flag that has to come through
        "_site_registers:\n"
        "  jmp *_slot_registers(%rip)\n"
        "back_registers:\n"
        "  mov %rax, _seen+0(%rip)\n mov %rcx, _seen+8(%rip)\n mov %rdx, _seen+16(%rip)\n mov %rbx, _seen+24(%rip)\n"
        "  mov %rsi, _seen+32(%rip)\n mov %rdi, _seen+40(%rip)\n mov %r8, _seen+48(%rip)\n mov %r9, _seen+56(%rip)\n"
        "  mov %r10, _seen+64(%rip)\n mov %r11, _seen+72(%rip)\n mov %r12, _seen+80(%rip)\n mov %r13, _seen+88(%rip)\n"
        "  mov %r14, _seen+96(%rip)\n mov %r15, _seen+104(%rip)\n"
        "  mov -8(%rsp), %rax\n mov %rax, _seen+120(%rip)\n" // before the push below writes there (mov keeps the flags)
        "  pushfq\n pop %rax\n mov %rax, _seen+112(%rip)\n"
        "  mov %rsp, _seen+136(%rip)\n mov %rbp, _seen+144(%rip)\n"
        "  lea _seen+152(%rip), %rax\n"
        "  movups %xmm0, 0(%rax)\n movups %xmm1, 16(%rax)\n movups %xmm2, 32(%rax)\n movups %xmm3, 48(%rax)\n"
        "  movups %xmm4, 64(%rax)\n movups %xmm5, 80(%rax)\n movups %xmm6, 96(%rax)\n movups %xmm7, 112(%rax)\n"
        "  movups %xmm8, 128(%rax)\n movups %xmm9, 144(%rax)\n movups %xmm10, 160(%rax)\n movups %xmm11, 176(%rax)\n"
        "  movups %xmm12, 192(%rax)\n movups %xmm13, 208(%rax)\n movups %xmm14, 224(%rax)\n movups %xmm15, 240(%rax)\n"
        "  lea -40(%rbp), %rsp\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbx\n pop %rbp\n ret\n"
        // the place's own instruction, which the stub's jump replaced: it has to run after the handler, once
        "_moved_registers:\n"
        "  lea 0x100(%r11), %r11\n"
        "  jmp back_registers\n");

static struct {
    int calls;
    detour_registers_t registers;
    uint64_t stack, frame;
    bool change;
} handled;

detour_with_registers(Registers) {
    handled.calls++;
    handled.registers = *registers;
    handled.stack = (uint64_t)detour_stack(registers);
    handled.frame = (uint64_t)__builtin_frame_address(0);
    // uses registers of its own
    volatile double busy = 1.5;
    for (int i = 0; i < 8; i++)
        busy = busy * 1.25 + (double)i;
    if (handled.change) {
        registers->rax = 0x7777;
        registers->r9 = 0x9999;
        registers->r14 = 0x1414;
        registers->rbx = 0xbbbb;
        memset(registers->xmm[7], 0x77, 16);
        memset(registers->xmm[13], 0x13, 16);
        registers->rflags &= ~(uint64_t)1; // the carry flag
    }
}

static void test_registers(void) {
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 16; k++)
            xmm_values[i][k] = (uint8_t)(0x10 * i + k + 1);
    install_detour_Registers(site_registers);
    CHECK(slot_registers != NULL && orig_Registers == (void *)moved_registers);
    for (int crooked = 0; crooked <= 1; crooked++) {
        for (int change = 0; change <= 1; change++) {
            memset(&seen, 0, sizeof(seen));
            handled.calls = 0, handled.change = change;
            game_registers(crooked);
            CHECK(handled.calls == 1);
            // the handler saw the game's registers
            const detour_registers_t *r = &handled.registers;
            CHECK(r->rax == 0xa1 && r->rcx == 0xc1 && r->rdx == 0xd1 && r->rbx == 0xb1 && r->rsi == 0x51 &&
                  r->rdi == 0xd2 && r->r8 == 0x08 && r->r9 == 0x09 && r->r10 == 0x10 && r->r11 == 0x11 &&
                  r->r12 == 0x12 && r->r13 == 0x13 && r->r14 == 0x14 && r->r15 == 0x15);
            CHECK(r->rbp == seen.rbp && (r->rflags & 1) == 1);
            CHECK(memcmp(r->xmm, xmm_values, sizeof(xmm_values)) == 0);
            CHECK(handled.stack == seen.rsp_before && seen.rsp_after == seen.rsp_before);
            CHECK((seen.rsp_before % 16 != 0) == (crooked != 0));
            // and was called as a function has to be
            CHECK(handled.frame % 16 == 0);
            // the game goes on with them, the moved instruction has run once, and what it keeps below its stack
            // pointer is still there
            CHECK(seen.rcx == 0xc1 && seen.rdx == 0xd1 && seen.rsi == 0x51 && seen.rdi == 0xd2 && seen.r8 == 0x08 &&
                  seen.r10 == 0x10 && seen.r11 == 0x111 && seen.r12 == 0x12 && seen.r13 == 0x13 && seen.r15 == 0x15);
            CHECK(seen.below_stack == 0x1122334455667788);
            uint8_t expected[16][16];
            memcpy(expected, xmm_values, sizeof(expected));
            if (change) {
                memset(expected[7], 0x77, 16);
                memset(expected[13], 0x13, 16);
            }
            CHECK(memcmp(seen.xmm, expected, sizeof(expected)) == 0);
            CHECK(seen.rax == (change ? 0x7777 : 0xa1) && seen.r9 == (change ? 0x9999 : 0x09) &&
                  seen.r14 == (change ? 0x1414 : 0x14) && seen.rbx == (change ? 0xbbbb : 0xb1));
            CHECK((seen.rflags & 1) == (change ? 0 : 1));
        }
    }
}

// ---- a place before a test

// game_test_user(): asks the made-up test with 2 and answers 100 + what it kept in a register when the test says
// yes, 200 + that when it says no
int game_test_user(void);
static struct {
    int calls, argument;
    bool answer;
    uint64_t frame;
} the_test;
bool game_test(int argument) {
    the_test.calls++;
    the_test.argument = argument;
    the_test.frame = (uint64_t)__builtin_frame_address(0);
    return the_test.answer;
}
__asm__(".globl _game_test_user, _site_test, _moved_test, _yes_of_test\n"
        "_game_test_user:\n"
        "  push %rbp\n mov %rsp, %rbp\n push %rbx\n sub $8, %rsp\n"
        "  mov $7, %ebx\n"
        "_site_test:\n"
        "  jmp *_slot_test(%rip)\n"
        "back_test:\n"
        "  call _game_test\n"
        "  test %al, %al\n"
        "  je 1f\n"
        "_yes_of_test:\n"
        "  lea 100(%rbx), %eax\n"
        "  jmp 2f\n"
        "1: lea 200(%rbx), %eax\n"
        "2: add $8, %rsp\n pop %rbx\n pop %rbp\n ret\n"
        "_moved_test:\n"
        "  mov $2, %edi\n"
        "  jmp back_test\n");

static struct {
    int calls;
    bool answer;
    uint64_t frame;
} asked;
detour_before_test(Test) {
    asked.calls++;
    asked.frame = (uint64_t)__builtin_frame_address(0);
    volatile double busy = 2.5; // uses registers of its own
    for (int i = 0; i < 8; i++)
        busy = busy * 1.5 + (double)i;
    return asked.answer;
}

static void test_before_test(void) {
    install_detour_Test(site_test, yes_of_test);
    CHECK(slot_test != NULL && orig_Test == (void *)moved_test && yes_Test == (void *)yes_of_test);
    // the handler says yes: the game's test isn't asked, and the game goes on where a yes takes it
    asked.calls = the_test.calls = 0;
    asked.answer = true, the_test.answer = false;
    CHECK(game_test_user() == 107 && asked.calls == 1 && the_test.calls == 0 && asked.frame % 16 == 0);
    // the handler says no: the game's own instructions run, the test is asked with its argument and decides
    asked.answer = false, the_test.answer = false;
    CHECK(game_test_user() == 207 && asked.calls == 2 && the_test.calls == 1 && the_test.argument == 2 &&
          the_test.frame % 16 == 0);
    the_test.answer = true;
    CHECK(game_test_user() == 107 && asked.calls == 3 && the_test.calls == 2 && the_test.argument == 2);
}

// ---- a place right behind a call, with a choice of where to go on

// game_choice_user(flag): keeps values in the registers a call leaves alone, calls something, and then, at the
// place, tests the flag: 300 + what it kept when the flag is set, 400 + that when it isn't. The other place it can
// be sent to answers 500 + what it kept.
int game_choice_user(int flag);
static int called_before;
void game_before_choice(void) {
    called_before++;
}
__asm__(".globl _game_choice_user, _site_choice, _moved_choice, _other_of_choice\n"
        "_game_choice_user:\n"
        "  push %rbp\n mov %rsp, %rbp\n push %rbx\n push %r13\n"
        "  mov %edi, %ebx\n"               // the flag, kept across the call
        "  movabs $0x1313131313131313, %r13\n"
        "  call _game_before_choice\n"
        "_site_choice:\n"
        "  jmp *_slot_choice(%rip)\n"
        "back_choice:\n"                   // behind the moved "cmp ; jne"
        "  lea 400(%r13d), %eax\n"
        "  jmp 3f\n"
        "choice_set:\n"
        "  lea 300(%r13d), %eax\n"
        "  jmp 3f\n"
        "_other_of_choice:\n"
        "  lea 500(%r13d), %eax\n"
        "3: pop %r13\n pop %rbx\n pop %rbp\n ret\n"
        // the place's own instructions: a test and a short jump, which a stub makes a near one
        "_moved_choice:\n"
        "  cmp $0, %bl\n"
        "  jne choice_set\n"
        "  jmp back_choice\n");

static struct {
    int calls;
    bool answer;
    uint64_t first, second, frame;
} chosen;
detour_choice(Choice, "mov %r13, %rdi\n mov %rbp, %rsi\n") {
    chosen.calls++;
    chosen.first = first, chosen.second = second;
    chosen.frame = (uint64_t)__builtin_frame_address(0);
    volatile double busy = 3.5; // uses registers of its own
    for (int i = 0; i < 8; i++)
        busy = busy * 1.5 + (double)i;
    return chosen.answer;
}

static void test_choice(void) {
    install_detour_Choice(site_choice, other_of_choice);
    CHECK(slot_choice != NULL && orig_Choice == (void *)moved_choice && other_Choice == (void *)other_of_choice);
    const int kept = 0x13131313;
    // the handler says no: the game's own test decides, either way
    chosen.answer = false, chosen.calls = called_before = 0;
    CHECK(game_choice_user(0) == 400 + kept && game_choice_user(1) == 300 + kept && chosen.calls == 2 &&
          called_before == 2);
    // it got the two values the game keeps, and was called as a function has to be
    CHECK(chosen.first == 0x1313131313131313 && chosen.second != 0 && chosen.second % 16 == 0 && chosen.frame % 16 == 0);
    // yes: the game goes on at the other place, whatever its own test would say
    chosen.answer = true;
    CHECK(game_choice_user(0) == 500 + kept && game_choice_user(1) == 500 + kept && chosen.calls == 4);
}

int main(void) {
    test_registers();
    test_before_test();
    test_choice();
    if (failures != 0) {
        printf("test_detour: %d failed\n", failures);
        return 1;
    }
    printf("test_detour: all passed\n");
    return 0;
}
