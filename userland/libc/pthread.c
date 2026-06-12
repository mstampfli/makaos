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
#include <signal.h>

// Raw futex.  Returns 0, or -errno (no errno global games — callers
// branch on the raw value).  timeout_ns: relative, 0 = forever.
#define _FUTEX_WAIT 0
#define _FUTEX_WAKE 1
static inline long _futex(volatile void* uaddr, int op, unsigned val,
                          unsigned long timeout_ns) {
    return (long)syscall4(114 /*SYS_FUTEX*/, (uint64_t)uaddr,
                          (uint64_t)op, (uint64_t)val,
                          (uint64_t)timeout_ns);
}

// Declared by pthread_trampoline.asm.
extern void pthread_trampoline(void);

// Kernel thread-creation flags (match kernel/proc/process.h).
#define THREAD_SHARE_MM     (1U << 0)
#define THREAD_SHARE_FILES  (1U << 1)

// ── Thread descriptors (pointer-as-handle, no lookup table) ──────────
//
// pthread_t IS a pointer to the thread's descriptor (struct __pthread),
// exactly like glibc/NPTL — so there is NOTHING to look up.  join,
// detach, kill and equal dereference the handle directly: every
// operation is O(1) by construction, no scan, no hash, no shared table
// to contend on.  Each running thread also caches its own descriptor in
// %fs (g_self), so pthread_self() is a single TLS read.
//
// The descriptor is heap-allocated by pthread_create and freed exactly
// once: by the joiner (joinable) or by a later create's reaper after a
// detached thread has died (a detached thread cannot free the stack/TLS
// it is still running on, so it hands its descriptor to a lock-free
// reap stack on the way out).
struct __pthread {
    int    tid;                  // kernel pid of the task
    volatile int done;           // kernel cleartid word: 1 when task is dead
    int    detached;
    void*  retval;
    void*  stack;                // mmap'd thread stack (munmap on reap)
    size_t stack_size;
    void*  tls_block;            // %fs block (free on reap)
    struct __pthread* reap_next; // lock-free reap stack link (detached)
};

// This thread's own descriptor + cached kernel tid — TLS, O(1) access.
static __thread struct __pthread* g_self;
static __thread int               g_self_tid;

// Lock-free LIFO of dead detached descriptors awaiting reap.
static struct __pthread* _Atomic g_reap_head;

static void reap_detached(void) {
    // Detach the whole list with one swap, free the dead, re-push the
    // not-yet-dead.  O(pending) and only detached threads ever appear.
    struct __pthread* head =
        __atomic_exchange_n(&g_reap_head, (struct __pthread*)0, __ATOMIC_ACQ_REL);
    while (head) {
        struct __pthread* next = head->reap_next;
        if (__atomic_load_n(&head->done, __ATOMIC_ACQUIRE)) {
            if (head->stack)     munmap(head->stack, head->stack_size);
            if (head->tls_block) free(head->tls_block);
            free(head);
        } else {
            // Not dead yet — push back for a later reap.
            struct __pthread* old =
                __atomic_load_n(&g_reap_head, __ATOMIC_RELAXED);
            do { head->reap_next = old; }
            while (!__atomic_compare_exchange_n(&g_reap_head, &old, head, 0,
                                                __ATOMIC_RELEASE, __ATOMIC_RELAXED));
        }
        head = next;
    }
}

// Called from the trampoline once TLS is live: cache self + register the
// cleartid join word so the kernel stores 1 + futex-wakes it at exit.
void __pthread_thread_setup(struct __pthread* self) {
    g_self     = self;
    g_self_tid = (int)syscall0(SYS_GETPID);
    self->tid  = g_self_tid;
    syscall1(SYS_SET_CLEARTID, (uint64_t)(uintptr_t)&self->done);
}

// Current thread's kernel tid (for mutex ownership, kill, etc.) — TLS
// read for spawned threads, getpid() once for the main thread.
static int self_tid(void) {
    if (g_self_tid) return g_self_tid;
    g_self_tid = (int)syscall0(SYS_GETPID);
    return g_self_tid;
}

// ── Thread lifecycle ─────────────────────────────────────────────────

int pthread_attr_init(pthread_attr_t* a) {
    if (!a) return EINVAL;
    a->kind          = 0;
    a->stack_size    = 1024 * 1024;           // 1 MiB default — fontconfig's
                                              // XML parse blew through 256K
                                              // (no guard pages yet: overflow
                                              // scribbled the neighbour map)
    a->stack         = NULL;
    a->detachstate   = PTHREAD_CREATE_JOINABLE;
    a->schedpolicy   = 0;  // SCHED_OTHER
    a->schedpriority = 0;
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
int pthread_attr_setdetachstate(pthread_attr_t* a, int state) {
    if (!a) return EINVAL;
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    a->detachstate = state; return 0;
}
int pthread_attr_getdetachstate(const pthread_attr_t* a, int* state) {
    if (!a || !state) return EINVAL;
    *state = a->detachstate; return 0;
}
int pthread_attr_setschedpolicy(pthread_attr_t* a, int policy) {
    if (!a) return EINVAL;
    a->schedpolicy = policy; return 0;
}
int pthread_attr_getschedpolicy(const pthread_attr_t* a, int* policy) {
    if (!a || !policy) return EINVAL;
    *policy = a->schedpolicy; return 0;
}
// struct sched_param is defined in <sched.h>, which <pthread.h>
// pulls in transitively (glibc-compatible behaviour) — no local
// redefinition needed.
int pthread_attr_setschedparam(pthread_attr_t* a, const struct sched_param* p) {
    if (!a || !p) return EINVAL;
    a->schedpriority = p->sched_priority; return 0;
}
int pthread_attr_getschedparam(const pthread_attr_t* a, struct sched_param* p) {
    if (!a || !p) return EINVAL;
    p->sched_priority = a->schedpriority; return 0;
}

// Per-thread scheduling on MakaOS is a no-op — the kernel runs every
// thread under its fair CFS-like scheduler.  Report defaults and
// accept any priority silently so upstream code that probes for
// SCHED_FIFO/RR at startup doesn't abort.
int pthread_setschedparam(pthread_t tid, int policy, const struct sched_param* p) {
    (void)tid; (void)policy; (void)p; return 0;
}
int pthread_getschedparam(pthread_t tid, int* policy, struct sched_param* p) {
    (void)tid;
    if (policy) *policy = 0;       // SCHED_OTHER
    if (p) p->sched_priority = 0;
    return 0;
}

int pthread_create(pthread_t* tid_out, const pthread_attr_t* attr,
                    void* (*start)(void*), void* arg) {
    if (!tid_out || !start) return EINVAL;

    size_t stack_size = attr ? attr->stack_size : 1024 * 1024;
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
    // Per-thread TLS block — the trampoline installs it via SYS_SET_FS
    // before start() runs, so errno and every other __thread variable
    // is private from the first instruction of user code.
    extern size_t __makaos_tls_block_size(void);
    extern void*  __makaos_tls_setup_block(void*);
    void* tls_block = malloc(__makaos_tls_block_size());
    if (!tls_block) {
        if (!attr || !attr->stack) munmap(stack, stack_size);
        return ENOMEM;
    }
    void* tls_tp = __makaos_tls_setup_block(tls_block);

    // The descriptor IS the handle.  Allocate it before the thread runs
    // and hand its pointer to the trampoline (4th stack slot) so the
    // thread can cache it in %fs and register its cleartid word.  Both
    // sides see the same descriptor with no lookup.
    struct __pthread* d = malloc(sizeof(*d));
    if (!d) {
        if (!attr || !attr->stack) munmap(stack, stack_size);
        free(tls_block);
        return ENOMEM;
    }
    d->tid = 0; d->done = 0;
    d->detached = (attr && attr->detachstate == PTHREAD_CREATE_DETACHED);
    d->retval = NULL;
    d->stack = stack; d->stack_size = stack_size;
    d->tls_block = tls_block; d->reap_next = NULL;

    // Opportunistically reap any dead detached threads (amortized O(1)).
    reap_detached();

    uint8_t* top = (uint8_t*)stack + stack_size;
    top = (uint8_t*)((uintptr_t)top & ~(uintptr_t)15);   // 16-byte align
    uint64_t* slot = (uint64_t*)top;
    *(--slot) = (uint64_t)d;         // popped 4th → __pthread_thread_setup
    *(--slot) = (uint64_t)tls_tp;    // popped 3rd (SYS_SET_FS)
    *(--slot) = (uint64_t)arg;       // popped 2nd (rdi)
    *(--slot) = (uint64_t)start;     // popped 1st

    long tid = (long)syscall3(SYS_THREAD,
                                (uint64_t)(void*)pthread_trampoline,
                                (uint64_t)slot,
                                THREAD_SHARE_MM | THREAD_SHARE_FILES);
    if (tid < 0) {
        if (!attr || !attr->stack) munmap(stack, stack_size);
        free(tls_block);
        free(d);
        errno = (int)-tid; return (int)-tid;
    }
    *tid_out = (pthread_t)d;
    return 0;
}

pthread_t pthread_self(void) {
    // Spawned threads cache their descriptor in %fs.  The main thread
    // has no descriptor; return a stable sentinel (its tid as a pointer)
    // so pthread_self()/equal stay consistent without a heap descriptor.
    if (g_self) return (pthread_t)g_self;
    return (pthread_t)(uintptr_t)self_tid();
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

// Defined with the TSS machinery below; needed by the exit-time walk.
static void (*g_tls_dtors[PTHREAD_KEYS_MAX])(void*);

__attribute__((noreturn))
void pthread_exit(void* retval) {
    // POSIX key-destructor walk — runs on the exiting thread's own TLS.
    for (int round = 0; round < 4; round++) {
        int again = 0;
        for (unsigned k = 0; k < PTHREAD_KEYS_MAX; k++) {
            void* v = pthread_getspecific(k);
            if (v && g_tls_dtors[k]) {
                pthread_setspecific(k, NULL);
                g_tls_dtors[k](v);
                again = 1;
            }
        }
        if (!again) break;
    }
    struct __pthread* self = g_self;
    if (self) {
        // Store ONLY the return value.  The done flag belongs to the
        // KERNEL (SYS_SET_CLEARTID): it stores 1 + futex-wakes at task
        // termination, when this thread provably cannot touch its stack
        // or TLS again.  A detached thread can't free the stack/TLS it
        // is running on, so it hands its descriptor to the reap stack;
        // the next pthread_create frees it once the kernel marks it dead.
        self->retval = retval;
        if (self->detached) {
            struct __pthread* old =
                __atomic_load_n(&g_reap_head, __ATOMIC_RELAXED);
            do { self->reap_next = old; }
            while (!__atomic_compare_exchange_n(&g_reap_head, &old, self, 0,
                                                __ATOMIC_RELEASE, __ATOMIC_RELAXED));
        }
    }
    // SYS_EXIT terminates just this thread (TASK_FLAG_THREAD path).
    syscall1(SYS_EXIT, (uint64_t)(uintptr_t)retval);
    __builtin_unreachable();
}

int pthread_join(pthread_t t, void** retval) {
    struct __pthread* d = (struct __pthread*)t;     // handle IS the descriptor
    if (!d || d->detached) return EINVAL;
    // Sleep on the kernel's cleartid word; it is set + futex-woken at
    // task death (when the thread's stack/TLS are provably idle).
    while (!__atomic_load_n(&d->done, __ATOMIC_ACQUIRE))
        _futex(&d->done, _FUTEX_WAIT, 0, 0);
    if (retval) *retval = d->retval;
    if (d->stack)     munmap(d->stack, d->stack_size);
    if (d->tls_block) free(d->tls_block);
    free(d);                                         // O(1) — no table
    return 0;
}

int pthread_detach(pthread_t t) {
    struct __pthread* d = (struct __pthread*)t;
    if (!d) return ESRCH;
    d->detached = 1;
    return 0;
}

int pthread_cancel(pthread_t t) {
    // MakaOS has no clean cancel-at-safe-point semantics.  We SIGKILL
    // the target and let the kernel tear the task down; the caller
    // loses any cleanup handlers (we don't run them).  Most wayland
    // / wlroots code avoids cancel, so this is acceptable for now.
    struct __pthread* d = (struct __pthread*)t;
    if (!d) return ESRCH;
    return (int)__syscall_ret(syscall2(SYS_KILL, (uint64_t)d->tid, 9 /* SIGKILL */));
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
    int me = self_tid();
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
    int me = self_tid();
    if (m->kind == PTHREAD_MUTEX_RECURSIVE && m->owner == me) {
        m->depth++;
        return 0;
    }
    if (m->kind == PTHREAD_MUTEX_ERRORCHECK && m->owner == me)
        return EDEADLK;
    // Three-state futex mutex (Drepper, "Futexes Are Tricky" §mutex2):
    // 0 = free, 1 = locked, 2 = locked with (possible) waiters.  The
    // unlock side only pays the FUTEX_WAKE syscall when state was 2.
    // A short pause-spin first — most wlroots/glib critical sections
    // are tens of nanoseconds.
    for (int spin = 0; spin < 100; spin++) {
        int expect = 0;
        if (__atomic_compare_exchange_n(&m->locked, &expect, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            m->owner = me;
            m->depth = 1;
            return 0;
        }
        __builtin_ia32_pause();
    }
    int c = __atomic_exchange_n(&m->locked, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        _futex(&m->locked, _FUTEX_WAIT, 2, 0);   // EAGAIN/EINTR → retry
        c = __atomic_exchange_n(&m->locked, 2, __ATOMIC_ACQUIRE);
    }
    m->owner = me;
    m->depth = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
    if (!m) return EINVAL;
    int me = self_tid();
    if (m->kind == PTHREAD_MUTEX_ERRORCHECK && m->owner != me)
        return EPERM;
    if (m->kind == PTHREAD_MUTEX_RECURSIVE && --m->depth > 0)
        return 0;
    m->owner = 0;
    m->depth = 0;
    if (__atomic_exchange_n(&m->locked, 0, __ATOMIC_RELEASE) == 2)
        _futex(&m->locked, _FUTEX_WAKE, 1, 0);
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
    unsigned seen = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    c->bound_mutex = m;
    pthread_mutex_unlock(m);
    // Sleep until the sequence moves.  EAGAIN means it already moved
    // (signal raced our unlock — exactly the wake we wanted); EINTR
    // re-checks.  Spurious wakeups are POSIX-legal.
    while (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == seen)
        _futex(&c->seq, _FUTEX_WAIT, seen, 0);
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                            const struct timespec* abs) {
    if (!c || !m || !abs) return EINVAL;
    unsigned seen = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    c->bound_mutex = m;
    pthread_mutex_unlock(m);
    int rc = 0;
    while (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == seen) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        long long rel = (abs->tv_sec - now.tv_sec) * 1000000000LL
                      + (abs->tv_nsec - now.tv_nsec);
        if (rel <= 0) { rc = ETIMEDOUT; break; }
        long fr = _futex(&c->seq, _FUTEX_WAIT, seen,
                         (unsigned long)rel);
        if (fr == -110 /*-ETIMEDOUT*/) { 
            // Deadline hit while asleep; one final seq check decides.
            if (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == seen)
                rc = ETIMEDOUT;
            break;
        }
        // 0 / -EAGAIN / -EINTR all re-check the predicate.
    }
    pthread_mutex_lock(m);
    return rc;
}

int pthread_cond_signal(pthread_cond_t* c) {
    if (!c) return EINVAL;
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    _futex(&c->seq, _FUTEX_WAKE, 1, 0);
    return 0;
}
int pthread_cond_broadcast(pthread_cond_t* c) {
    if (!c) return EINVAL;
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    _futex(&c->seq, _FUTEX_WAKE, 0x7fffffff, 0);
    return 0;
}

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

// Per-thread slot array in REAL TLS (%fs) — pthread_getspecific and
// pthread_setspecific are lock-free and async-signal-safe.  The old
// global (tid,key) table took a raw spin lock on every access; a
// SIGCHLD handler that landed while its own thread held the lock
// (glib calls g_private_get from everywhere, including signal-time
// logging) spun forever — sway main deadlocked exactly this way the
// moment its child exec'd.
static __thread void* g_tss[PTHREAD_KEYS_MAX];
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
    if (key >= PTHREAD_KEYS_MAX) return NULL;
    return g_tss[key];
}

int pthread_setspecific(pthread_key_t key, const void* val) {
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;
    g_tss[key] = (void*)val;
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

// Remaining sched.h functions — MakaOS has a single CFS-like policy,
// so we report fixed bounds and accept any configuration without
// error.  Priorities only matter once we expose real-time policies
// (deferred with the rest of the scheduler work).
int sched_get_priority_min(int policy) { (void)policy; return 0; }
int sched_get_priority_max(int policy) { (void)policy; return 99; }
int sched_getscheduler(pid_t pid) { (void)pid; return 0; }
int sched_setscheduler(pid_t pid, int policy, const struct sched_param* p) {
    (void)pid; (void)policy; (void)p; return 0;
}
int sched_getparam(pid_t pid, struct sched_param* p) {
    (void)pid; if (p) p->sched_priority = 0; return 0;
}
int sched_setparam(pid_t pid, const struct sched_param* p) {
    (void)pid; (void)p; return 0;
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

// pthread_sigmask — per-thread signal mask.  MakaOS has no per-thread
// mask yet; forward to process-level sigprocmask.
// TODO(scalability-debt-ledger): real per-thread sigmask when the
// kernel gains per-task pending-signal tables.
extern int sigprocmask(int how, const sigset_t* set, sigset_t* old);
int pthread_sigmask(int how, const sigset_t* set, sigset_t* old) {
    return sigprocmask(how, set, old);
}

int pthread_setcancelstate(int state, int* oldstate) {
    (void)state; if (oldstate) *oldstate = 0; return 0;
}
int pthread_setcanceltype(int type, int* oldtype) {
    (void)type; if (oldtype) *oldtype = 0; return 0;
}
void pthread_testcancel(void) { }

int pthread_atfork(void (*prep)(void), void (*parent)(void), void (*child)(void)) {
    (void)prep; (void)parent; (void)child;
    return 0;
}
