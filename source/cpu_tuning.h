// CPU scheduling fixes for the Mac port's threads and job system.
#ifndef CPU_TUNING_H
#define CPU_TUNING_H

#include <stdbool.h>
#include <stdint.h>

// thread_qos: run the game's own threads at user-interactive QoS
// job_idle_fix: make idle job workers really wait instead of spinning
// yield_sleep0: make Sleep(0) a yield, like on Windows, instead of a ~6 us nanosleep
// fast_setjmp: point the game's setjmp/longjmp at _setjmp/_longjmp, which skip the signal mask system call
// each can be overridden at env (BO3MACFIX_THREADQOS, BO3MACFIX_JOBIDLEFIX, BO3MACFIX_YIELDSLEEP0, BO3MACFIX_FASTSETJMP)
// render_idle_fix: make the idle render thread really wait for work instead of spinning (BO3MACFIX_RENDERIDLEFIX)
// main_wait_fix: make the main thread really wait for the render thread instead of spinning (BO3MACFIX_MAINWAITFIX)
// fast_script_errors: stop script errors from retrying a script source file that isn't there (BO3MACFIX_FASTSCRIPTERRORS)
void cpu_tuning_init(uint64_t base_address, int thread_qos, int job_idle_fix, int yield_sleep0, int fast_setjmp,
                     int render_idle_fix, int main_wait_fix, int fast_script_errors);

typedef enum { Wait_Fix_Job, Wait_Fix_Render, Wait_Fix_Main } cpu_wait_fix_t;
// JobIdleFix, RenderIdleFix or MainWaitFix changed while the game runs; false if it needs a restart
bool cpu_tuning_set_wait_fix(cpu_wait_fix_t which, int on);

#endif
