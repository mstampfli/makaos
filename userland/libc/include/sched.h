#ifndef _MAKAOS_SCHED_H
#define _MAKAOS_SCHED_H 1
// POSIX sched.h — MakaOS has no pluggable scheduler policies; every
// thread runs under the same fair CFS-like scheduler.  The policies
// are defined so upstream code that probes for SCHED_FIFO/SCHED_RR
// compiles cleanly — they silently fall back to SCHED_OTHER behaviour
// at runtime.

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct sched_param {
    int sched_priority;
};

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2
#define SCHED_BATCH 3
#define SCHED_IDLE  5

int sched_yield(void);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);
int sched_getscheduler(pid_t pid);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param* p);
int sched_getparam(pid_t pid, struct sched_param* p);
int sched_setparam(pid_t pid, const struct sched_param* p);

#ifdef __cplusplus
}
#endif

#endif
