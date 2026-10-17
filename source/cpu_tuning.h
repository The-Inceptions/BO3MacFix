// CPU scheduling fixes for the Mac port's threads and job system.
#ifndef CPU_TUNING_H
#define CPU_TUNING_H

#include <stdint.h>

// thread_qos: run the game's own threads at user-interactive QoS
// job_idle_fix: make idle job workers really wait instead of spinning
// yield_sleep0: make Sleep(0) a yield, like on Windows, instead of a ~6 us nanosleep
// fast_setjmp: point the game's setjmp/longjmp at _setjmp/_longjmp, which skip the signal mask system call
// each can be overridden at env (BO3MACFIX_THREADQOS, BO3MACFIX_JOBIDLEFIX, BO3MACFIX_YIELDSLEEP0, BO3MACFIX_FASTSETJMP)
void cpu_tuning_init(uint64_t base_address, int thread_qos, int job_idle_fix, int yield_sleep0, int fast_setjmp);

#endif
