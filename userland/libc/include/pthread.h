#ifndef _MAKAOS_PTHREAD_H
#define _MAKAOS_PTHREAD_H 1
// POSIX threads — MakaOS userland.
//
// Backed by the kernel thread(entry, stack_top, flags) syscall.
// Mutex/cond are implemented with atomic ops + sched_yield; there's
// no futex, so contended locks spin-and-yield.  Good enough for
// wayland/wlroots/hyprland which use mutexes sparingly.

#include <sys/types.h>
#include <stddef.h>
#include <time.h>
// POSIX allows pthread.h to not include sched.h, but glibc does and
// upstream code (SDL3's systhread among others) relies on pthread.h
// transitively providing SCHED_OTHER/RR/FIFO + struct sched_param +
// sched_get_priority_{min,max}.  Match glibc for compatibility.
#include <sched.h>

typedef int pthread_t;              // tid = kernel pid of the thread task
typedef struct {
    int    kind;                    // reserved for attribute future-proofing
    size_t stack_size;
    void*  stack;
    int    detachstate;             // PTHREAD_CREATE_{JOINABLE,DETACHED}
    int    schedpolicy;             // SCHED_OTHER / SCHED_FIFO / SCHED_RR
    int    schedpriority;           // 0 for SCHED_OTHER
} pthread_attr_t;

// Detach state
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

typedef struct {
    volatile int locked;            // 0 = free, 1 = held
    pthread_t    owner;             // owning tid (debug + recursive)
    int          kind;              // PTHREAD_MUTEX_NORMAL / RECURSIVE / ERRORCHECK
    int          depth;             // recursion count (kind == RECURSIVE)
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0, 0 }

typedef struct {
    int kind;
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

typedef struct {
    volatile unsigned seq;          // bumped on each wake
    pthread_mutex_t*  bound_mutex;  // last mutex passed to cond_wait (debug)
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER { 0, 0 }

typedef struct { int clock; } pthread_condattr_t;

typedef struct { volatile int done; pthread_mutex_t m; } pthread_once_t;
#define PTHREAD_ONCE_INIT { 0, PTHREAD_MUTEX_INITIALIZER }

typedef unsigned int pthread_key_t;
#define PTHREAD_KEYS_MAX 128

typedef struct {
    volatile int readers;
    volatile int writer;
    pthread_mutex_t m;
} pthread_rwlock_t;
#define PTHREAD_RWLOCK_INITIALIZER { 0, 0, PTHREAD_MUTEX_INITIALIZER }
typedef struct { int kind; } pthread_rwlockattr_t;

typedef struct { volatile int flag; } pthread_spinlock_t;

// ── Attr ─────────────────────────────────────────────────────────────
int pthread_attr_init(pthread_attr_t* a);
int pthread_attr_destroy(pthread_attr_t* a);
int pthread_attr_setstacksize(pthread_attr_t* a, size_t sz);
int pthread_attr_getstacksize(const pthread_attr_t* a, size_t* sz);
int pthread_attr_setstack(pthread_attr_t* a, void* stack, size_t sz);
int pthread_attr_setdetachstate(pthread_attr_t* a, int state);
int pthread_attr_getdetachstate(const pthread_attr_t* a, int* state);
int pthread_attr_setschedpolicy(pthread_attr_t* a, int policy);
int pthread_attr_getschedpolicy(const pthread_attr_t* a, int* policy);
struct sched_param;
int pthread_attr_setschedparam(pthread_attr_t* a, const struct sched_param* p);
int pthread_attr_getschedparam(const pthread_attr_t* a, struct sched_param* p);

// ── Thread lifecycle ─────────────────────────────────────────────────
int  pthread_create(pthread_t* tid, const pthread_attr_t* attr,
                     void* (*start)(void*), void* arg);
int  pthread_join(pthread_t tid, void** retval);
int  pthread_detach(pthread_t tid);
void pthread_exit(void* retval) __attribute__((__noreturn__));
pthread_t pthread_self(void);
int  pthread_equal(pthread_t a, pthread_t b);
int  pthread_cancel(pthread_t tid);

// ── Mutex ────────────────────────────────────────────────────────────
int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a);
int pthread_mutex_destroy(pthread_mutex_t* m);
int pthread_mutex_lock(pthread_mutex_t* m);
int pthread_mutex_trylock(pthread_mutex_t* m);
int pthread_mutex_unlock(pthread_mutex_t* m);
int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* abs);

int pthread_mutexattr_init(pthread_mutexattr_t* a);
int pthread_mutexattr_destroy(pthread_mutexattr_t* a);
int pthread_mutexattr_settype(pthread_mutexattr_t* a, int kind);
int pthread_mutexattr_gettype(const pthread_mutexattr_t* a, int* kind);

// ── Condition variable ──────────────────────────────────────────────
int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a);
int pthread_cond_destroy(pthread_cond_t* c);
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m);
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                            const struct timespec* abs);
int pthread_cond_signal(pthread_cond_t* c);
int pthread_cond_broadcast(pthread_cond_t* c);

int pthread_condattr_init(pthread_condattr_t* a);
int pthread_condattr_destroy(pthread_condattr_t* a);
int pthread_condattr_setclock(pthread_condattr_t* a, clockid_t clk);

// ── Once ─────────────────────────────────────────────────────────────
int pthread_once(pthread_once_t* once, void (*init)(void));

// ── TLS keys ─────────────────────────────────────────────────────────
int   pthread_key_create(pthread_key_t* key, void (*dtor)(void*));
int   pthread_key_delete(pthread_key_t key);
void* pthread_getspecific(pthread_key_t key);
int   pthread_setspecific(pthread_key_t key, const void* val);

// ── RWLock ───────────────────────────────────────────────────────────
int pthread_rwlock_init(pthread_rwlock_t* l, const pthread_rwlockattr_t* a);
int pthread_rwlock_destroy(pthread_rwlock_t* l);
int pthread_rwlock_rdlock(pthread_rwlock_t* l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t* l);
int pthread_rwlock_wrlock(pthread_rwlock_t* l);
int pthread_rwlock_trywrlock(pthread_rwlock_t* l);
int pthread_rwlock_unlock(pthread_rwlock_t* l);

// ── Spinlock ─────────────────────────────────────────────────────────
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1
int pthread_spin_init(pthread_spinlock_t* s, int pshared);
int pthread_spin_destroy(pthread_spinlock_t* s);
int pthread_spin_lock(pthread_spinlock_t* s);
int pthread_spin_trylock(pthread_spinlock_t* s);
int pthread_spin_unlock(pthread_spinlock_t* s);

// ── Misc ─────────────────────────────────────────────────────────────
int sched_yield(void);
int pthread_setname_np(pthread_t tid, const char* name);
int pthread_getname_np(pthread_t tid, char* name, size_t n);

// Per-thread signal mask — MakaOS has a single process-wide mask
// so this aliases sigprocmask.  Forward-decl the sigset_t from
// signal.h to avoid pulling the whole header here.
#ifndef _MAKAOS_SIGNAL_H
typedef unsigned long sigset_t;
#endif
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old);

// Scheduling parameters — MakaOS has no pluggable scheduler policy,
// so these accept/report reasonable defaults and the kernel runs
// every thread at the same priority.  Provided so upstream libs
// that probe for real-time priorities don't explode at link time.
int pthread_setschedparam(pthread_t tid, int policy, const struct sched_param* p);
int pthread_getschedparam(pthread_t tid, int* policy, struct sched_param* p);

#endif
