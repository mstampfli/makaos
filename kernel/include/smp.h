#pragma once
#include "common.h"

// ── SMP-ready stub macros ────────────────────────────────────────────────
//
// MakaOS currently runs on a single CPU, but all new code is written as if
// SMP is already here.  Every global that will need locking under SMP is
// wrapped in these macros today, so bring-up becomes "implement the macros"
// rather than "find every global and add locking".
//
// Rules for new code:
//   - Every cross-task shared struct that will be touched from IRQs or
//     other CPUs gets a `spinlock_t lock` field.  Use spin_lock()/unlock()
//     around accesses.  Today the lock is a stub, tomorrow it's real.
//   - Atomic counters/bitmaps use the atomic_* helpers, never plain ops.
//   - Per-CPU state is declared via PERCPU(type, name) and accessed via
//     percpu_get(name).  Today that just gives you the [0] slot.
//   - cpu_id() returns 0 today.  Use it anywhere you'll eventually need
//     to know which CPU you're on.
//
// When SMP bring-up happens, the macros below grow real bodies; the call
// sites never change.

// ── Spinlocks ────────────────────────────────────────────────────────────
// Today: single byte, no actual locking — but the struct layout is fixed
// so adding contents later doesn't break ABI of containing structs.
typedef struct {
    uint8_t _locked;  // 0 = unlocked, 1 = locked
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock_init(spinlock_t* l) { l->_locked = 0; }

// UP stub: compiler barrier + set byte.  IRQ-safe variant disables IRQs.
static inline void spin_lock(spinlock_t* l) {
    (void)l;
    __asm__ volatile("" ::: "memory");
}
static inline void spin_unlock(spinlock_t* l) {
    (void)l;
    __asm__ volatile("" ::: "memory");
}

// IRQ-safe: saves flags, disables IRQs, takes lock.
// Returns previous IF state for spin_unlock_irqrestore.
static inline uint64_t spin_lock_irqsave(spinlock_t* l) {
    (void)l;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void spin_unlock_irqrestore(spinlock_t* l, uint64_t flags) {
    (void)l;
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

// ── Atomic operations ────────────────────────────────────────────────────
// Built on GCC/Clang __atomic builtins.  On UP these compile to plain
// instructions; on SMP they emit LOCK-prefixed ops.  Use seq_cst unless
// there's a measured reason to relax.

#define atomic_load(ptr)           __atomic_load_n((ptr), __ATOMIC_SEQ_CST)
#define atomic_store(ptr, val)     __atomic_store_n((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_add(ptr, val)       __atomic_fetch_add((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_sub(ptr, val)       __atomic_fetch_sub((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_or(ptr, val)        __atomic_fetch_or((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_and(ptr, val)       __atomic_fetch_and((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_xchg(ptr, val)      __atomic_exchange_n((ptr), (val), __ATOMIC_SEQ_CST)
#define atomic_cas(ptr, oldp, new) __atomic_compare_exchange_n((ptr), (oldp), (new), \
                                        0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

// Set bit n in *word (uint32_t).  Returns the old word.
static inline uint32_t atomic_set_bit(volatile uint32_t* word, unsigned n) {
    return atomic_or(word, (uint32_t)(1u << n));
}
// Clear bit n in *word.  Returns the old word.
static inline uint32_t atomic_clear_bit(volatile uint32_t* word, unsigned n) {
    return atomic_and(word, (uint32_t)~(1u << n));
}
// Test bit n.  Non-atomic read — use only for opportunistic checks.
static inline int test_bit(uint32_t word, unsigned n) {
    return (word >> n) & 1u;
}

// ── Per-CPU state ────────────────────────────────────────────────────────
// PERCPU(type, name) declares a per-CPU variable.  percpu_get(name) returns
// a pointer to the current CPU's slot.  Today CPU count is 1, so the slot
// is always [0].  When SMP arrives, MAX_CPUS grows and cpu_id() reads the
// APIC ID.
#define MAX_CPUS 64  // hard ceiling for SMP bring-up; currently only [0] is live

#define PERCPU(type, name) static type name##_percpu[MAX_CPUS]
#define percpu_get(name)   (&name##_percpu[cpu_id()])

static inline unsigned cpu_id(void) {
    // TODO SMP: read APIC ID here when MP is enabled.
    return 0u;
}

static inline unsigned num_cpus(void) {
    // TODO SMP: return detected CPU count.
    return 1u;
}

// ── Memory barriers ──────────────────────────────────────────────────────
// Compiler barriers today; grow to mfence/lfence/sfence on SMP as needed.
static inline void smp_mb(void)  { __asm__ volatile("" ::: "memory"); }
static inline void smp_rmb(void) { __asm__ volatile("" ::: "memory"); }
static inline void smp_wmb(void) { __asm__ volatile("" ::: "memory"); }
