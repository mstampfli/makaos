// ── semaphore.c — POSIX unnamed semaphores over the kernel futex ────
//
// sem_wait sleeps in FUTEX_WAIT while the count is zero; sem_post
// pays a FUTEX_WAKE only after a zero→positive transition.  (The
// original spin-and-yield version burned a core per blocked waiter.)

#include <semaphore.h>
#include <errno.h>
#include <stdarg.h>
#include <makaos/syscall.h>

static inline long _futex(volatile void* uaddr, int op, unsigned val,
                          unsigned long timeout_ns) {
    return (long)syscall4(SYS_FUTEX, (uint64_t)uaddr, (uint64_t)op,
                          (uint64_t)val, (uint64_t)timeout_ns);
}

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
    if (__atomic_fetch_add(&s->value, 1, __ATOMIC_RELEASE) == 0)
        _futex(&s->value, FUTEX_OP_WAKE, 1, 0);
    return 0;
}

int sem_wait(sem_t* s) {
    if (!s) { errno = EINVAL; return -1; }
    for (;;) {
        int v = __atomic_load_n(&s->value, __ATOMIC_ACQUIRE);
        if (v > 0) {
            if (__atomic_compare_exchange_n(
                    &s->value, &v, v - 1, 0,
                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
            continue;       // raced; reload
        }
        _futex(&s->value, FUTEX_OP_WAIT, (unsigned)v, 0);
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
        long long rel = (abs->tv_sec - now.tv_sec) * 1000000000LL
                      + (abs->tv_nsec - now.tv_nsec);
        if (rel <= 0) { errno = ETIMEDOUT; return -1; }
        _futex(&s->value, FUTEX_OP_WAIT, (unsigned)(v <= 0 ? v : 0),
               (unsigned long)rel);
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
