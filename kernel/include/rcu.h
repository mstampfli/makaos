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
static inline void rcu_read_lock(void) {
    preempt_disable();
}

// Exit an RCU reader section.  Re-enables preemption.
static inline void rcu_read_unlock(void) {
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

// Asynchronous reclamation: after a grace period elapses, call
// func(data).  Phase 2 implementation is synchronous (equivalent to
// synchronize_rcu(); func(data);), which is correct but blocks the
// caller.  Phase 6 can upgrade to truly deferred per-CPU callback lists
// if profiling shows a hot call_rcu site.
typedef void (*rcu_func_t)(void* data);
void call_rcu(rcu_func_t func, void* data);

// ── Internal — called by the scheduler ───────────────────────────────────
// Not part of the public API, but exposed so sched.c can bump the QS
// counter from inside do_switch without pulling the full cpu_t header
// into the sched header chain.
void rcu_note_qs(void);
