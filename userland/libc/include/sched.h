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

// CPU affinity sets (Linux-compatible). MakaOS has no affinity control, so
// sched_getaffinity reports every online CPU as available (see sched.c).
#define CPU_SETSIZE 1024
#define __NCPUBITS  (8 * sizeof(unsigned long))
typedef struct {
    unsigned long __bits[CPU_SETSIZE / __NCPUBITS];
} cpu_set_t;

#define CPU_ZERO(s)     __builtin_memset((s), 0, sizeof(cpu_set_t))
#define CPU_SET(c, s)   ((s)->__bits[(c) / __NCPUBITS] |=  (1UL << ((c) % __NCPUBITS)))
#define CPU_CLR(c, s)   ((s)->__bits[(c) / __NCPUBITS] &= ~(1UL << ((c) % __NCPUBITS)))
#define CPU_ISSET(c, s) (((s)->__bits[(c) / __NCPUBITS] >> ((c) % __NCPUBITS)) & 1UL)
int CPU_COUNT(const cpu_set_t* s);
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t* mask);
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t* mask);

int sched_yield(void);
int sched_getcpu(void);            // current CPU; MakaOS hint -> always 0
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
