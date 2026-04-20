// ── POSIX pthread — MakaOS userland ───────────────────────────────────
//
// Kernel primitives used:
//   sys_thread(entry, stack_top, flags)       — spawn a task
//   SYS_EXIT (via _exit)                       — thread terminates
//   sys_nanosleep / sched_yield                — spin-yield backstop
//   sys_mmap anon                              — thread stacks
//   atomic compare-exchange                    — lock state
//
// The kernel has no futex, so contended mutex / cond waits spin with
// __builtin_ia32_pause + sched_yield.  Wayland and wlroots do most
// work on a single thread with epoll, so the lock traffic is light —
// this is a real tradeoff that pays for itself later when / if we
// add a kernel futex.
//
// Thread state lives in a bounded-size table shared across all
// threads (THREAD_SHARE_MM makes globals visible to every thread).
// Indexed by a 1:1 hash of tid mod TABLE_SIZE; collisions scan.
// Holds: tid, joined flag, retval, detached flag.  pthread_join
// spin-waits on the flag.

#include <pthread.h>
#include <makaos/syscall.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// Declared by pthread_trampoline.asm.
extern void pthread_trampoline(void);

// Kernel thread-creation flags (match kernel/proc/process.h).
#define THREAD_SHARE_MM     (1U << 0)
#define THREAD_SHARE_FILES  (1U << 1)

// ── Thread state table ───────────────────────────────────────────────

#define THR_TABLE_BITS 7
#define THR_TABLE_SZ   (1u << THR_TABLE_BITS)

typedef struct thr_slot {
    volatile int     tid;           // 0 = free
    volatile int     done;          // set when thread reaches pthread_exit
    volatile int     detached;      // auto-free on exit if set
    void*            retval;
    void*            stack;         // for munmap on join/detach-exit
    size_t           stack_size;
} thr_slot_t;

static thr_slot_t g_slots[THR_TABLE_SZ];
static volatile int g_table_lock;

static void lock_table(void) {
    while (__sync_lock_test_and_set(&g_table_lock, 1))
        __builtin_ia32_pause();
}
static void unlock_table(void) {
    __sync_lock_release(&g_table_lock);
}

static thr_slot_t* slot_alloc(int tid) {
    lock_table();
    for (unsigned i = 0; i < THR_TABLE_SZ; i++) {
        unsigned idx = (unsigned)(tid + i) & (THR_TABLE_SZ - 1);
        if (g_slots[idx].tid == 0) {
            g_slots[idx].tid      = tid;
            g_slots[idx].done     = 0;
            g_slots[idx].detached = 0;
            g_slots[idx].retval   = NULL;
            g_slots[idx].stack    = NULL;
            unlock_table();
            return &g_slots[idx];
        }
    }
    unlock_table();
    return NULL;
}

static thr_slot_t* slot_find(int tid) {
    for (unsigned i = 0; i < THR_TABLE_SZ; i++) {
        unsigned idx = (unsigned)(tid + i) & (THR_TABLE_SZ - 1);
        if (g_slots[idx].tid == tid) return &g_slots[idx];
        if (g_slots[idx].tid == 0)   return NULL;   // early stop: linear probe
    }
    return NULL;
}

static void slot_free(thr_slot_t* s) {
    lock_table();
    s->tid = 0;
    unlock_table();
}

// ── Thread lifecycle ─────────────────────────────────────────────────

int pthread_attr_init(pthread_attr_t* a) {
    if (!a) return EINVAL;
    a->kind = 0;
    a->stack_size = 256 * 1024;            // 256 KiB default
    a->stack = NULL;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t* a) { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t* a, size_t sz) {
    if (!a || sz < 16 * 1024) return EINVAL;
    a->stack_size = sz; return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t* a, size_t* sz) {
    if (!a || !sz) return EINVAL;
    *sz = a->stack_size; return 0;
}
int pthread_attr_setstack(pthread_attr_t* a, void* stack, size_t sz) {
    if (!a || !stack || sz < 16 * 1024) return EINVAL;
    a->stack = stack; a->stack_size = sz; return 0;
}

int pthread_create(pthread_t* tid_out, const pthread_attr_t* attr,
                    void* (*start)(void*), void* arg) {
    if (!tid_out || !start) return EINVAL;

    size_t stack_size = attr ? attr->stack_size : 256 * 1024;
    void*  stack      = attr ? attr->stack      : NULL;
    if (!stack) {
        stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack == MAP_FAILED) return ENOMEM;
    }

    // Stack top must carry (start_fn, arg) for the asm trampoline to
    // pop.  Layout (high to low):
    //     [top - 8]   start_fn
    //     [top - 16]  arg
    // Trampoline does: pop rax (start_fn); pop rdi (arg).
    uint8_t* top = (uint8_t*)stack + stack_size;
    top = (uint8_t*)((uintptr_t)top & ~(uintptr_t)15);   // 16-byte align
    uint64_t* slot = (uint64_t*)top;
    *(--slot) = (uint64_t)arg;       // pushed 2nd → popped 2nd (rdi)
    *(--slot) = (uint64_t)start;     // pushed 1st → popped 1st (rax)

    long tid = (long)syscall3(SYS_THREAD,
                                (uint64_t)(void*)pthread_trampoline,
                                (uint64_t)slot,
                                THREAD_SHARE_MM | THREAD_SHARE_FILES);
    if (tid < 0) {
        if (!attr || !attr->stack) munmap(stack, stack_size);
        errno = (int)-tid; return (int)-tid;
    }

    thr_slot_t* s = slot_alloc((int)tid);
    if (s) { s->stack = stack; s->stack_size = stack_size; }

    *tid_out = (pthread_t)tid;
    return 0;
}

pthread_t pthread_self(void) {
    return (pthread_t)syscall0(SYS_GETPID);
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

__attribute__((noreturn))
void pthread_exit(void* retval) {
    pthread_t me = pthread_self();
    thr_slot_t* s = slot_find((int)me);
    if (s) {
        s->retval = retval;
        __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
        if (s->detached && s->stack) {
            // Can't munmap our own stack and still run code — leak it.
            // The kernel reclaims on exit; this is only userland tracking.
        }
    }
    // SYS_EXIT terminates just this thread (TASK_FLAG_THREAD path).
    syscall1(SYS_EXIT, (uint64_t)(uintptr_t)retval);
    __builtin_unreachable();
}

int pthread_join(pthread_t tid, void** retval) {
    thr_slot_t* s = slot_find((int)tid);
    if (!s)            return ESRCH;
    if (s->detached)   return EINVAL;

    // Spin-wait with sched_yield.  No futex, so this is the best we've got.
    while (!__atomic_load_n(&s->done, __ATOMIC_ACQUIRE)) {
        struct timespec ts = { 0, 1000000 };   // 1 ms
        nanosleep(&ts, NULL);
    }
    if (retval) *retval = s->retval;
    if (s->stack) munmap(s->stack, s->stack_size);
    slot_free(s);
    return 0;
}

int pthread_detach(pthread_t tid) {
    thr_slot_t* s = slot_find((int)tid);
    if (!s) return ESRCH;
    s->detached = 1;
    return 0;
}

int pthread_cancel(pthread_t tid) {
    // MakaOS has no clean cancel-at-safe-point semantics.  We SIGKILL
    // the target and let the kernel tear the task down; the caller
    // loses any cleanup handlers (we don't run them).  Most wayland
    // / wlroots code avoids cancel, so this is acceptable for now.
    return (int)__syscall_ret(syscall2(SYS_KILL, (uint64_t)tid, 9 /* SIGKILL */));
}

// ── Mutex ────────────────────────────────────────────────────────────

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    if (!m) return EINVAL;
    m->locked = 0;
    m->owner  = 0;
    m->kind   = a ? a->kind : PTHREAD_MUTEX_NORMAL;
    m->depth  = 0;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    if (m->locked) return EBUSY;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    pthread_t me = pthread_self();
    if (m->kind == PTHREAD_MUTEX_RECURSIVE && m->owner == me) {
        m->depth++;
        return 0;
    }
    if (__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
        m->owner = me;
        m->depth = 1;
        return 0;
    }
    return EBUSY;
}

int pthread_mutex_lock(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    pthread_t me = pthread_self();
    if (m->kind == PTHREAD_MUTEX_RECURSIVE && m->owner == me) {
        m->depth++;
        return 0;
    }
    if (m->kind == PTHREAD_MUTEX_ERRORCHECK && m->owner == me)
        return EDEADLK;
    // Spin briefly, then yield.  500 pause instructions ≈ 1–2 μs on
    // modern CPUs before we give up and let the scheduler run someone
    // else.
    for (int spin = 0; spin < 500; spin++) {
        if (__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
            m->owner = me;
            m->depth = 1;
            return 0;
        }
        __builtin_ia32_pause();
    }
    while (!__sync_bool_compare_and_swap(&m->locked, 0, 1))
        sched_yield();
    m->owner = me;
    m->depth = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    pthread_t me = pthread_self();
    if (m->kind == PTHREAD_MUTEX_ERRORCHECK && m->owner != me)
        return EPERM;
    if (m->kind == PTHREAD_MUTEX_RECURSIVE && --m->depth > 0)
        return 0;
    m->owner = 0;
    m->depth = 0;
    __sync_lock_release(&m->locked);
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* abs) {
    // Spin-yield until abs reached.
    while (1) {
        if (pthread_mutex_trylock(m) == 0) return 0;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec  > abs->tv_sec ||
           (now.tv_sec == abs->tv_sec && now.tv_nsec >= abs->tv_nsec))
            return ETIMEDOUT;
        sched_yield();
    }
}

int pthread_mutexattr_init(pthread_mutexattr_t* a)              { if (!a) return EINVAL; a->kind = PTHREAD_MUTEX_DEFAULT; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t* a)           { (void)a; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t* a, int k)    { if (!a) return EINVAL; a->kind = k; return 0; }
int pthread_mutexattr_gettype(const pthread_mutexattr_t* a, int* k) { if (!a || !k) return EINVAL; *k = a->kind; return 0; }

// ── Condition variable ──────────────────────────────────────────────
// Strategy: each signal/broadcast bumps c->seq.  cond_wait samples
// seq before releasing m, then polls seq while the mutex is unlocked;
// wakes when seq changes.  Spurious wakes are allowed by POSIX so
// polling cadence is non-critical.

int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* a) {
    (void)a; if (!c) return EINVAL;
    c->seq = 0; c->bound_mutex = NULL; return 0;
}
int pthread_cond_destroy(pthread_cond_t* c) { (void)c; return 0; }

int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    if (!c || !m) return EINVAL;
    unsigned seen = c->seq;
    c->bound_mutex = m;
    pthread_mutex_unlock(m);
    while (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == seen) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                            const struct timespec* abs) {
    if (!c || !m || !abs) return EINVAL;
    unsigned seen = c->seq;
    pthread_mutex_unlock(m);
    while (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == seen) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > abs->tv_sec ||
            (now.tv_sec == abs->tv_sec && now.tv_nsec >= abs->tv_nsec)) {
            pthread_mutex_lock(m);
            return ETIMEDOUT;
        }
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t* c) {
    if (!c) return EINVAL;
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    return 0;
}
int pthread_cond_broadcast(pthread_cond_t* c) { return pthread_cond_signal(c); }

int pthread_condattr_init(pthread_condattr_t* a)          { if (!a) return EINVAL; a->clock = CLOCK_REALTIME; return 0; }
int pthread_condattr_destroy(pthread_condattr_t* a)       { (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t* a, clockid_t clk) { if (!a) return EINVAL; a->clock = (int)clk; return 0; }

// ── Once ─────────────────────────────────────────────────────────────

int pthread_once(pthread_once_t* once, void (*init)(void)) {
    if (!once || !init) return EINVAL;
    if (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&once->m);
    if (!once->done) {
        init();
        __atomic_store_n(&once->done, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once->m);
    return 0;
}

// ── TLS keys ─────────────────────────────────────────────────────────
// Simple map-based TLS: one entry per (tid, key) pair.  Tables stay
// small in practice (wayland uses <10 keys).

typedef struct tls_entry {
    int           tid;
    pthread_key_t key;
    void*         val;
} tls_entry_t;

#define TLS_MAX  1024
static tls_entry_t g_tls[TLS_MAX];
static void (*g_tls_dtors[PTHREAD_KEYS_MAX])(void*);
static volatile pthread_key_t g_tls_next = 1;
static volatile int g_tls_lock;

static void tls_lock(void)   { while (__sync_lock_test_and_set(&g_tls_lock, 1)) __builtin_ia32_pause(); }
static void tls_unlock(void) { __sync_lock_release(&g_tls_lock); }

int pthread_key_create(pthread_key_t* key, void (*dtor)(void*)) {
    if (!key) return EINVAL;
    tls_lock();
    if (g_tls_next >= PTHREAD_KEYS_MAX) { tls_unlock(); return EAGAIN; }
    pthread_key_t k = g_tls_next++;
    g_tls_dtors[k] = dtor;
    tls_unlock();
    *key = k;
    return 0;
}
int pthread_key_delete(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;
    tls_lock();
    g_tls_dtors[key] = NULL;
    tls_unlock();
    return 0;
}

void* pthread_getspecific(pthread_key_t key) {
    int me = (int)pthread_self();
    tls_lock();
    for (unsigned i = 0; i < TLS_MAX; i++) {
        if (g_tls[i].tid == me && g_tls[i].key == key) {
            void* v = g_tls[i].val;
            tls_unlock();
            return v;
        }
    }
    tls_unlock();
    return NULL;
}

int pthread_setspecific(pthread_key_t key, const void* val) {
    int me = (int)pthread_self();
    tls_lock();
    // Update existing or allocate fresh slot.
    int free_idx = -1;
    for (unsigned i = 0; i < TLS_MAX; i++) {
        if (g_tls[i].tid == me && g_tls[i].key == key) {
            g_tls[i].val = (void*)val;
            tls_unlock();
            return 0;
        }
        if (free_idx < 0 && g_tls[i].tid == 0) free_idx = (int)i;
    }
    if (free_idx < 0) { tls_unlock(); return ENOMEM; }
    g_tls[free_idx].tid = me;
    g_tls[free_idx].key = key;
    g_tls[free_idx].val = (void*)val;
    tls_unlock();
    return 0;
}

// ── RWLock ───────────────────────────────────────────────────────────

int pthread_rwlock_init(pthread_rwlock_t* l, const pthread_rwlockattr_t* a) {
    (void)a; if (!l) return EINVAL;
    l->readers = 0; l->writer = 0;
    return pthread_mutex_init(&l->m, NULL);
}
int pthread_rwlock_destroy(pthread_rwlock_t* l) { if (!l) return EINVAL; return pthread_mutex_destroy(&l->m); }

int pthread_rwlock_rdlock(pthread_rwlock_t* l) {
    if (!l) return EINVAL;
    while (1) {
        pthread_mutex_lock(&l->m);
        if (!l->writer) { l->readers++; pthread_mutex_unlock(&l->m); return 0; }
        pthread_mutex_unlock(&l->m);
        sched_yield();
    }
}
int pthread_rwlock_tryrdlock(pthread_rwlock_t* l) {
    if (!l) return EINVAL;
    pthread_mutex_lock(&l->m);
    if (l->writer) { pthread_mutex_unlock(&l->m); return EBUSY; }
    l->readers++;
    pthread_mutex_unlock(&l->m);
    return 0;
}
int pthread_rwlock_wrlock(pthread_rwlock_t* l) {
    if (!l) return EINVAL;
    while (1) {
        pthread_mutex_lock(&l->m);
        if (!l->writer && !l->readers) { l->writer = 1; pthread_mutex_unlock(&l->m); return 0; }
        pthread_mutex_unlock(&l->m);
        sched_yield();
    }
}
int pthread_rwlock_trywrlock(pthread_rwlock_t* l) {
    if (!l) return EINVAL;
    pthread_mutex_lock(&l->m);
    if (l->writer || l->readers) { pthread_mutex_unlock(&l->m); return EBUSY; }
    l->writer = 1;
    pthread_mutex_unlock(&l->m);
    return 0;
}
int pthread_rwlock_unlock(pthread_rwlock_t* l) {
    if (!l) return EINVAL;
    pthread_mutex_lock(&l->m);
    if (l->writer) l->writer = 0; else if (l->readers) l->readers--;
    pthread_mutex_unlock(&l->m);
    return 0;
}

// ── Spinlock ─────────────────────────────────────────────────────────

int pthread_spin_init(pthread_spinlock_t* s, int pshared)   { (void)pshared; if (!s) return EINVAL; s->flag = 0; return 0; }
int pthread_spin_destroy(pthread_spinlock_t* s)             { (void)s; return 0; }
int pthread_spin_lock(pthread_spinlock_t* s) {
    if (!s) return EINVAL;
    while (__sync_lock_test_and_set(&s->flag, 1)) __builtin_ia32_pause();
    return 0;
}
int pthread_spin_trylock(pthread_spinlock_t* s) {
    if (!s) return EINVAL;
    return __sync_lock_test_and_set(&s->flag, 1) ? EBUSY : 0;
}
int pthread_spin_unlock(pthread_spinlock_t* s) { if (!s) return EINVAL; __sync_lock_release(&s->flag); return 0; }

// ── Misc ─────────────────────────────────────────────────────────────

int sched_yield(void) {
    return (int)syscall0(SYS_SCHED_YIELD);
}

int pthread_setname_np(pthread_t tid, const char* name) {
    (void)tid; (void)name;
    return 0;                         // kernel doesn't expose a thread-name syscall yet
}
int pthread_getname_np(pthread_t tid, char* name, size_t n) {
    (void)tid;
    if (name && n) name[0] = '\0';
    return 0;
}
