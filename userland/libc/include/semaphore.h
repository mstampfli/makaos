#ifndef _MAKAOS_SEMAPHORE_H
#define _MAKAOS_SEMAPHORE_H 1
// POSIX named + unnamed semaphores implemented atop our atomic ops +
// sched_yield.  Contended waits spin-and-yield — good enough for
// foot's output pump, which has very light semaphore traffic.  A
// futex-backed implementation can replace this once the kernel
// exposes one.

#include <time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile int value;       // current count
    volatile int waiters;     // informational: wake hint
} sem_t;

#define SEM_FAILED ((sem_t*)0)

int sem_init   (sem_t* s, int pshared, unsigned value);
int sem_destroy(sem_t* s);
int sem_post   (sem_t* s);
int sem_wait   (sem_t* s);
int sem_trywait(sem_t* s);
int sem_timedwait(sem_t* s, const struct timespec* abs);
int sem_getvalue(sem_t* s, int* sval);

// Named semaphores — MakaOS has no shared /dev/shm yet; these fail
// with ENOSYS so ports fall back to sem_init on an unnamed sem_t.
sem_t* sem_open (const char* name, int oflag, ...);
int    sem_close(sem_t* s);
int    sem_unlink(const char* name);

#ifdef __cplusplus
}
#endif

#endif
