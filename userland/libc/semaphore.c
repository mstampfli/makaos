// ── semaphore.c — POSIX unnamed semaphores via atomic + sched_yield ──
//
// Spin-and-yield implementation.  Contended waits sched_yield(), so
// we don't need a futex.  Foot uses a single semaphore for its
// render pump and never hits deep contention, so this is fine for
// the first cut.  When we expose futex the wait path can switch
// over; the public API stays the same.

#include <semaphore.h>
#include <errno.h>
#include <stdarg.h>

extern int sched_yield(void);

int sem_init(sem_t* s, int pshared, unsigned value) {
    (void)pshared;
    if (!s) { errno = EINVAL; return -1; }
    s->value   = (int)value;
    s->waiters = 0;
    return 0;
}

int sem_destroy(sem_t* s) {
    (void)s; return 0;
}

int sem_post(sem_t* s) {
    if (!s) { errno = EINVAL; return -1; }
    __atomic_fetch_add(&s->value, 1, __ATOMIC_RELEASE);
    return 0;
}

int sem_wait(sem_t* s) {
    if (!s) { errno = EINVAL; return -1; }
    for (;;) {
        int v = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);
        if (v > 0 && __atomic_compare_exchange_n(
                &s->value, &v, v - 1, 0,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
        sched_yield();
    }
}

int sem_trywait(sem_t* s) {
    if (!s) { errno = EINVAL; return -1; }
    int v = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);
    if (v <= 0 || !__atomic_compare_exchange_n(
            &s->value, &v, v - 1, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        errno = EAGAIN; return -1;
    }
    return 0;
}

extern int clock_gettime(int, struct timespec*);
#define CLOCK_REALTIME 0

int sem_timedwait(sem_t* s, const struct timespec* abs) {
    if (!s || !abs) { errno = EINVAL; return -1; }
    for (;;) {
        int v = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);
        if (v > 0 && __atomic_compare_exchange_n(
                &s->value, &v, v - 1, 0,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > abs->tv_sec ||
            (now.tv_sec == abs->tv_sec && now.tv_nsec >= abs->tv_nsec)) {
            errno = ETIMEDOUT; return -1;
        }
        sched_yield();
    }
}

int sem_getvalue(sem_t* s, int* sval) {
    if (!s || !sval) { errno = EINVAL; return -1; }
    *sval = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);
    return 0;
}

// Named semaphores — no /dev/shm yet.
sem_t* sem_open(const char* name, int oflag, ...) {
    (void)name; (void)oflag;
    errno = ENOSYS; return SEM_FAILED;
}
int sem_close(sem_t* s)           { (void)s; errno = ENOSYS; return -1; }
int sem_unlink(const char* name)  { (void)name; errno = ENOSYS; return -1; }
