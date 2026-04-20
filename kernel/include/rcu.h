#pragma once
#include "common.h"
#include "smp.h"
#include "preempt.h"

// ── RCU — Read-Copy-Update ───────────────────────────────────────────────
//
// RCU provides a zero-cost reader-side synchronization mechanism for
// read-mostly data structures.  Readers do not block writers, writers
// never wait for readers on the data-plane, and the reader fast path
// takes zero atomic ops — just a compiler barrier and a preempt-depth
// bump.
//
// Quiescent-State-Based RCU (QSBR) flavor, modeled after Linux's "classic
// RCU":
//
//   rcu_read_lock()           preempt_disable (one `incl [mem]`)
//   p = rcu_dereference(ptr)  plain load + compiler barrier
//   ... use p ...
//   rcu_read_unlock()         preempt_enable (one `decl [mem]`)
//
// A "grace period" elapses when every CPU in the system has passed
// through a "quiescent state" (QS) since the grace period started.  A QS
// is any point at which the CPU is guaranteed not to be inside an RCU
// reader section.  MakaOS defines QS as:
//
//   - Every context switch (do_switch bumps the per-CPU QS counter)
//   - Every idle-loop iteration (CPU sitting in hlt is trivially in QS)
//
// Because rcu_read_lock == preempt_disable, a CPU cannot be context-
// switched while in a reader section.  So after every CPU has
// context-switched at least once, every reader that was active when the
// grace period started must have finished.
//
// ── Writer patterns ──────────────────────────────────────────────────────
//
//   // Synchronous:
//   new = kmalloc(sizeof *new);
//   *new = initial_state;
//   old = rcu_dereference(shared_ptr);
//   rcu_assign_pointer(shared_ptr, new);
//   synchronize_rcu();       // blocks until all pre-existing readers done
//   kfree(old);
//
//   // Asynchronous (caller doesn't block):
//   rcu_assign_pointer(shared_ptr, new);
//   call_rcu(kfree_callback, old);
//
// ── Rules ────────────────────────────────────────────────────────────────
//
//   - Never sleep inside rcu_read_lock / rcu_read_unlock.  The section
//     must be short.  If you need to sleep, copy what you need out and
//     release the RCU section first.
//   - synchronize_rcu() MUST be called with preempt_depth == 0.  Calling
//     it from inside a reader section or preempt-disabled critical
//     section is a self-deadlock (we'd wait for our own CPU to pass QS
//     but we're the one blocking it).  We panic loudly in that case.
//   - call_rcu()'s callback runs in a kthread context (not IRQ context).
//     It may allocate, free, grab locks, etc.

// ── Reader-side API ──────────────────────────────────────────────────────

// Enter an RCU reader section.  Disables preemption on the current CPU
// so the CPU cannot reach a quiescent state until rcu_read_unlock.
ALWAYS_INLINE void rcu_read_lock(void) {
    preempt_disable();
}

// Exit an RCU reader section.  Re-enables preemption.
ALWAYS_INLINE void rcu_read_unlock(void) {
    preempt_enable();
}

// Load an RCU-protected pointer.  The returned value is guaranteed to
// remain valid until the enclosing rcu_read_unlock.  On x86 this is a
// plain load; the __ATOMIC_CONSUME ordering gives us a compiler barrier
// against reordering dependent loads.
#define rcu_dereference(p) \
    __atomic_load_n(&(p), __ATOMIC_CONSUME)

// Publish a pointer with release semantics.  Any stores done to the new
// object before this call are visible to readers that subsequently
// dereference the pointer.  Pair with rcu_dereference on the read side.
#define rcu_assign_pointer(p, v) \
    __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

// ── Writer-side API ──────────────────────────────────────────────────────

// Block until every pre-existing RCU reader section has completed.
// After this returns, any object whose reference was removed from RCU-
// protected data structures before this call has no remaining readers
// and can be safely freed.
//
// May sleep (calls sched_yield internally on SMP).  On single-CPU this
// is a no-op plus a compiler barrier.
//
// Panics if called with preempt_depth > 0.
void synchronize_rcu(void);

// ── synchronize_rcu_expedited ────────────────────────────────────────────
// Same contract as synchronize_rcu() — returns only after every pre-
// existing reader section has ended — but faster on SMP.  Instead of
// waiting for each remote CPU to naturally pass through a quiescent
// state (next tick, context switch, or idle hlt), the expedited path
// IPIs every CPU with a no-op callback: the moment the IPI handler runs
// with preempt_depth==0, the target has provably left any reader
// section it was in when the grace period began.
//
// Latency: ~µs in the common "no reader active" case (just IPI RTT).
// Worst case falls back to the classic wait loop — same as
// synchronize_rcu().
//
// Use this from non-hot paths that care about latency (unveil / cred
// updates from a user-visible syscall, signal handler rebuild, etc.).
// Hot paths should prefer call_rcu() which is asynchronous entirely.
void synchronize_rcu_expedited(void);

typedef void (*rcu_func_t)(void* data);

// ── rcu_head_t — embed-able callback record ─────────────────────────────
// Include one of these in any struct you plan to free via RCU.  The
// head is lock-free pushed onto a per-CPU pending list by
// call_rcu_head; an RCU GP kthread periodically snapshots each CPU's
// list, waits for a grace period, then invokes every head's callback.
//
// Because the head is caller-supplied, call_rcu_head performs ZERO
// allocation and is safe to call from any context including inside
// g_pmm_lock or from IRQ disable.  Use this wherever the allocator
// path itself might defer (pmm slab destroy, fd close, etc.).
typedef struct rcu_head {
    struct rcu_head* next;
    rcu_func_t       func;
    void*            data;
} rcu_head_t;

// Asynchronous reclamation — caller-provided head.  After a grace
// period elapses, `func(data)` runs in the GP kthread's context
// (process context, IRQs enabled, preemptible — safe to kmalloc,
// take sleeping locks, etc.).
//
// The head must remain valid from the call until the callback fires.
// Typical use: embed the head in the object being freed; the object
// is alive until the callback runs, so the head is too.
//
// This is the ONLY asynchronous call_rcu path — no allocating
// wrapper.  Every async RCU site in the kernel embeds an rcu_head_t
// in the object being freed, matching Linux's call_rcu semantics.
void call_rcu_head(rcu_head_t* head, rcu_func_t func, void* data);

// Block until every call_rcu_head that happened-before this call has
// had its callback invoked.  Does NOT wait for callbacks enqueued
// after this call.  Implementation: enqueues a known callback on
// each CPU, waits for all of them to fire — by the time ours fires,
// everything queued before it on the same CPU has already fired.
// Mirrors Linux's rcu_barrier().
void rcu_barrier(void);

// Spawn the RCU GP kthread.  Called once from init_kthread after
// the scheduler is live.
void rcu_gp_kthread_start(void);

// Expedited variant of call_rcu — use only on user-syscall-return
// latency paths (munmap, brk-shrink, close of the last fd for a
// sock/shmem/vma).  Forces the grace period closed via IPI instead
// of waiting for every CPU's next natural quiescent state.  Don't
// use from hot paths or paths that already run in batches — the
// IPIs add per-remote-CPU cost that classic RCU's tick-piggyback
// avoids.  See rcu.c for the full trade-off.
void call_rcu_expedited(rcu_func_t func, void* data);

// ── Internal — called by the scheduler ───────────────────────────────────
// Not part of the public API, but exposed so sched.c can bump the QS
// counter from inside do_switch without pulling the full cpu_t header
// into the sched header chain.
void rcu_note_qs(void);
