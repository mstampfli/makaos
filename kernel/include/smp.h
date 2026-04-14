#pragma once
#include "common.h"

// ── SMP primitives ───────────────────────────────────────────────────────
//
// Phase 1 of the SMP bring-up: real spinlocks, real atomics, real memory
// barriers.  Per-CPU accessors live in cpu.h.
//
// MakaOS still runs on one CPU today, but every primitive below is already
// correct for multiple CPUs — no "UP stub" fallbacks.  The only thing that
// changes when APs boot is that more than one cpu_t gets constructed and
// this_cpu() starts returning per-CPU pointers.
//
// Design rules:
//   - Locks are a LAST resort.  Before adding a spinlock_t, ask whether the
//     data can be per-CPU, RCU-protected, or seqlock-protected.
//   - When a lock is unavoidable, hold it as briefly as possible and never
//     across a sleep.
//   - IRQ-safe variants (spin_lock_irqsave / spin_unlock_irqrestore) are
//     required whenever the lock can be taken from interrupt context.

// ── Atomic operations ────────────────────────────────────────────────────
// GCC/Clang __atomic builtins lower to plain instructions on UP and to
// LOCK-prefixed ops on SMP.  Default ordering is SEQ_CST because it's
// easy to reason about; relaxed variants are provided where a call site
// can prove weaker ordering is safe.

#define atomic_load(ptr)           __atomic_load_n((ptr), __ATOMIC_SEQ_CST)
#define atomic_load_acq(ptr)       __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
#define atomic_load_relaxed(ptr)   __atomic_load_n((ptr), __ATOMIC_RELAXED)

#define atomic_store(ptr, val)     __atomic_store_n((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_store_rel(ptr, val) __atomic_store_n((ptr), (val), __ATOMIC_RELEASE)
#define atomic_store_relaxed(ptr, val) \
    __atomic_store_n((ptr), (val), __ATOMIC_RELAXED)

#define atomic_add(ptr, val)       __atomic_fetch_add((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_sub(ptr, val)       __atomic_fetch_sub((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_or(ptr, val)        __atomic_fetch_or((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_and(ptr, val)       __atomic_fetch_and((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_xchg(ptr, val)      __atomic_exchange_n((ptr), (val), __ATOMIC_SEQ_CST)

#define atomic_cas(ptr, oldp, newv) \
    __atomic_compare_exchange_n((ptr), (oldp), (newv), 0, \
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_cas_weak(ptr, oldp, newv) \
    __atomic_compare_exchange_n((ptr), (oldp), (newv), 1, \
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)

// Set/clear bit n in *word.  Returns the old word.
static inline uint32_t atomic_set_bit(volatile uint32_t* word, unsigned n) {
    return atomic_or(word, (uint32_t)(1u << n));
}
static inline uint32_t atomic_clear_bit(volatile uint32_t* word, unsigned n) {
    return atomic_and(word, (uint32_t)~(1u << n));
}
static inline int test_bit(uint32_t word, unsigned n) {
    return (word >> n) & 1u;
}

// ── Memory barriers ──────────────────────────────────────────────────────
// x86 has a strong memory model: store-load is the only reorder the HW
// actually performs.  That makes most barriers compiler-only, and only
// full smp_mb() needs an mfence.
static inline void smp_mb(void) {
    __asm__ volatile("mfence" ::: "memory");
}
static inline void smp_rmb(void) {
    __asm__ volatile("" ::: "memory");   // load-load already ordered on x86
}
static inline void smp_wmb(void) {
    __asm__ volatile("" ::: "memory");   // store-store already ordered on x86
}
// Pause hint for spin loops (avoids pipeline stalls).
static inline void cpu_relax(void) {
    __asm__ volatile("pause" ::: "memory");
}

// ── Spinlocks ────────────────────────────────────────────────────────────
// Basic test-and-set spinlock with a pause-based backoff.  Correct under
// SMP, trivially correct under UP (just sets a byte).  Keep critical
// sections tiny — holding a spinlock across I/O is a bug.

typedef struct {
    volatile uint32_t locked;  // 0 = free, 1 = held
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock_init(spinlock_t* l) {
    atomic_store_relaxed(&l->locked, 0u);
}

static inline void spin_lock(spinlock_t* l) {
    for (;;) {
        // Fast path: try to grab the lock.  Acquire ordering pairs with
        // the release in spin_unlock.
        uint32_t zero = 0;
        if (__atomic_compare_exchange_n(&l->locked, &zero, 1u, 0,
                                          __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        // Contended: spin with pause until the lock looks free again.
        while (atomic_load_relaxed(&l->locked)) cpu_relax();
    }
}

static inline int spin_trylock(spinlock_t* l) {
    uint32_t zero = 0;
    return __atomic_compare_exchange_n(&l->locked, &zero, 1u, 0,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void spin_unlock(spinlock_t* l) {
    atomic_store_rel(&l->locked, 0u);
}

// IRQ-safe: save flags, disable IRQs, then take the lock.  Returns the
// saved RFLAGS so spin_unlock_irqrestore can restore IF.  Use this for any
// lock that can be taken from interrupt context — the plain spin_lock
// would deadlock if the IRQ fires while we hold the lock.
static inline uint64_t spin_lock_irqsave(spinlock_t* l) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    spin_lock(l);
    return flags;
}
static inline void spin_unlock_irqrestore(spinlock_t* l, uint64_t flags) {
    spin_unlock(l);
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

// ── Ticket lock (FIFO-fair) ──────────────────────────────────────────────
// Use when starvation must be avoided under high contention.  Each acquirer
// takes a ticket, then waits for the "now serving" counter to reach it.
// Slightly slower than spin_lock on the uncontended path, but guarantees
// every waiter eventually gets the lock.

typedef struct {
    volatile uint32_t head;   // next ticket to issue
    volatile uint32_t tail;   // currently serving
} ticket_lock_t;

#define TICKET_LOCK_INIT { 0, 0 }

static inline void ticket_lock_init(ticket_lock_t* t) {
    atomic_store_relaxed(&t->head, 0u);
    atomic_store_relaxed(&t->tail, 0u);
}

static inline void ticket_lock(ticket_lock_t* t) {
    uint32_t my = atomic_add(&t->head, 1u);
    while (atomic_load_acq(&t->tail) != my) cpu_relax();
}

static inline void ticket_unlock(ticket_lock_t* t) {
    atomic_store_rel(&t->tail, atomic_load_relaxed(&t->tail) + 1u);
}

// ── CPU count ────────────────────────────────────────────────────────────
// Upper bound on the number of CPUs the kernel is compiled to support.
// Per-CPU arrays are sized to this.  Raising it only costs BSS — no
// architectural change is needed.
#define MAX_CPUS 64

// ── Convenience for legacy code ──────────────────────────────────────────
// Declares a per-CPU array of size MAX_CPUS and provides percpu_get(name)
// which hands back a pointer to the current CPU's slot.  New code should
// prefer placing fields directly in cpu_t instead.
#define PERCPU(type, name) static type name##_percpu[MAX_CPUS]
#define percpu_get(name)   (&name##_percpu[cpu_id()])

// cpu_id() and this_cpu() live in cpu.h (can't be inlined here because
// they depend on the full cpu_t definition).  See kernel/proc/cpu.h.
unsigned cpu_id(void);
unsigned num_cpus(void);
