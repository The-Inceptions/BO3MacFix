// CPU scheduling fixes for the Mac port's threads and job system.
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dobby.h"
#include "dyld-interposing.h"
#include "offsets.h"
#include "cpu_tuning.h"
#include "prepatch.h"

static uint64_t base_address = 0;
static int thread_qos_enabled = 0;

// Aspyr's CreateThread makes plain pthreads and its SetThreadPriority/SetThreadAffinityMask do nothing,
// so every game thread runs at the default QoS, below Aspyr's own shader-cache warming queue
// (user-interactive). Each thread names itself right after it starts ("Main", "Win (WinID n)",
// "JQ Worker n (Processor m)"), so raise the game's own threads there. Steam, audio and
// "Non-ASL" threads are left alone.
static int is_game_thread(const char *name) {
    return strcmp(name, "Main") == 0 || strncmp(name, "Win (WinID", 10) == 0 || strncmp(name, "JQ Worker", 9) == 0;
}

int pthread_setname_np_new(const char *name) {
    if (thread_qos_enabled && name != NULL && is_game_thread(name))
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    return pthread_setname_np(name);
}
DYLD_INTERPOSE(pthread_setname_np_new, pthread_setname_np);

// Aspyr's Sleep(0) is nanosleep(1 ns), which really sleeps (~6 us under Rosetta), where on Windows it
// only gives up the rest of the time slice. The job system calls it while waiting on job dependencies
// and right after a worker wakes up for new work.
install_hook_name(ASL_SleepEx, uint32_t, uint32_t milliseconds, int alertable) {
    if (milliseconds == 0 && !alertable) {
        sched_yield();
        return 0;
    }
    return orig_ASL_SleepEx(milliseconds, alertable);
}

// replaces the code at address with patch, only if it's still the code we expect
static int cpu_checked_code_patch(uint32_t address, const uint8_t *expected, uint8_t *patch, uint32_t size) {
    if (memcmp(prepatch_original_code((void *)(base_address + address), size), expected, size) != 0) {
        printf("unexpected code at %08x, not patching\n", address);
        return 0;
    }
    DobbyCodePatch((void *)(base_address + address), patch, size);
    return 1;
}

// Idle job workers wait for work with WaitForSingleObject(event, 1 ms), but Aspyr's event wait doesn't
// wait at all for timeouts under 2 ms, it only polls. So every idle worker spins on the event's mutex,
// which also slows down the threads queueing jobs (they take that mutex to set the event).
// Queueing a job always sets the event, which wakes every waiting worker, so a real 2 ms wait
// doesn't delay any work, it only stops the spinning.
static void job_idle_wait_patch(void) {
    static const uint8_t mov_esi_1[] = { 0xBE, 0x01, 0x00, 0x00, 0x00 };
    uint8_t mov_esi_2[] = { 0xBE, 0x02, 0x00, 0x00, 0x00 };
    if (cpu_checked_code_patch(ADDR_JQ_IdleWait_TimeoutInstr, mov_esi_1, mov_esi_2, sizeof(mov_esi_2)))
        printf("job workers wait when idle\n");
}

// The render thread waits for work the same way: WaitForSingleObject(event, 1 ms), then Sleep(0) (a ~6 us nanosleep)
// if nothing came, over and over, so it spins whenever it's idle. A 2 ms wait really blocks, and the main thread
// sets the event when it hands over a frame, so the render thread still wakes straight away.
static void render_idle_wait_patch(void) {
    static const uint8_t mov_edi_1[] = { 0xBF, 0x01, 0x00, 0x00, 0x00 };
    uint8_t mov_edi_2[] = { 0xBF, 0x02, 0x00, 0x00, 0x00 };
    if (cpu_checked_code_patch(ADDR_RenderThread_WaitTimeoutInstr, mov_edi_1, mov_edi_2, sizeof(mov_edi_2)))
        printf("render thread waits when idle\n");
}

// The game's setjmp and longjmp also save and restore the signal mask, which is a system call each time: about
// 200 ns under Rosetta, against 7 ns for _setjmp, which only saves registers. The GSC script VM calls setjmp for
// every script thread it runs, and HavokScript for every protected call. Nothing in the game longjmps out of a
// signal handler (its only handlers, SDL's, just set a flag), so it doesn't need the mask: point its setjmp and
// longjmp imports at _setjmp and _longjmp. That writes its lazy symbol pointers, which are data, so the
// pre-patched executable keeps its ahead-of-time translation.
static void **import_pointer(uint32_t stub) {
    const uint8_t *code = (const uint8_t *)(base_address + stub);
    if (code[0] != 0xFF || code[1] != 0x25) // jmp qword ptr [rip + pointer]
        return NULL;
    int32_t disp;
    memcpy(&disp, code + 2, sizeof(disp));
    return (void **)(code + 6 + disp);
}

static void fast_setjmp_patch(void) {
    void **setjmp_pointer = import_pointer(ADDR_Stub_setjmp);
    void **longjmp_pointer = import_pointer(ADDR_Stub_longjmp);
    // both or neither, a jmp_buf from _setjmp has no signal mask for longjmp to restore
    if (setjmp_pointer == NULL || longjmp_pointer == NULL) {
        printf("unexpected code at %08x or %08x, not changing setjmp\n", ADDR_Stub_setjmp, ADDR_Stub_longjmp);
        return;
    }
    *setjmp_pointer = (void *)_setjmp;
    *longjmp_pointer = (void *)_longjmp;
    printf("game setjmp/longjmp skip the signal mask\n");
}

static int env_override(const char *name, int value) {
    const char *env = getenv(name);
    return env != NULL ? atoi(env) : value;
}

void cpu_tuning_init(uint64_t game_base_address, int thread_qos, int job_idle_fix, int yield_sleep0, int fast_setjmp,
                     int render_idle_fix) {
    base_address = game_base_address;
    thread_qos_enabled = env_override("BO3MACFIX_THREADQOS", thread_qos) != 0;
    if (thread_qos_enabled)
        printf("game threads use user-interactive QoS\n");
    if (env_override("BO3MACFIX_JOBIDLEFIX", job_idle_fix) != 0)
        job_idle_wait_patch();
    if (env_override("BO3MACFIX_RENDERIDLEFIX", render_idle_fix) != 0)
        render_idle_wait_patch();
    if (env_override("BO3MACFIX_YIELDSLEEP0", yield_sleep0) != 0) {
        install_hook_ASL_SleepEx((void *)(base_address + ADDR_ASL_SleepEx));
        printf("Sleep(0) yields\n");
    }
    if (env_override("BO3MACFIX_FASTSETJMP", fast_setjmp) != 0)
        fast_setjmp_patch();
}
