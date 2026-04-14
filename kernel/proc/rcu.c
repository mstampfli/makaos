#include "rcu.h"
#include "cpu.h"
#include "sched.h"
#include "common.h"

// ── RCU — QSBR implementation ────────────────────────────────────────────
//
// See rcu.h for the design and rules.  This file implements
// synchronize_rcu, call_rcu, and rcu_note_qs.
//
// The reader-side primitives are all in rcu.h as static inlines so the
// compiler can fold them into caller code: rcu_read_lock compiles to one
// `incl [this_cpu->preempt_depth]` and rcu_dereference is a plain load.

// Bump the current CPU's QS counter.  Called from:
//   - do_switch (every context switch)
//   - the idle loop (every hlt wakeup)
//
// Non-atomic because only the owning CPU writes its own counter.
// Readers on other CPUs use atomic_load_relaxed — torn reads just delay
// grace-period completion by at most one cycle, which is harmless.
void rcu_note_qs(void) {
    this_cpu()->rcu_qs_count++;
}

// ── synchronize_rcu ──────────────────────────────────────────────────────
//
// Strategy: snapshot every CPU's QS counter, then yield until every CPU
// has advanced past its snapshot.  On a single CPU (Phase 1-8) the
// calling thread IS the only thread that can still be in a reader
// section; since synchronize_rcu requires preempt_depth==0, the caller
// is not itself in a reader, so we just need to wait for any
// concurrently-running reader on our own CPU to finish — which already
// happened by the time we got here.  A compiler barrier is enough.
//
// The code below is written for SMP correctness and will work unchanged
// once APs come online.

void synchronize_rcu(void) {
    // Safety rail: calling this with preempt disabled would self-deadlock.
    // The caller's own CPU could never reach a quiescent state because
    // its preempt_depth is stuck > 0.
    if (this_cpu()->preempt_depth > 0) {
        serial_puts_dbg("[rcu] PANIC: synchronize_rcu with preempt disabled\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    unsigned n = num_cpus();
    uint64_t snap[MAX_CPUS];

    // Snapshot every CPU's current QS counter.  A relaxed load is fine —
    // even if we see a stale value, we only risk waiting slightly
    // longer, never completing too early.
    for (unsigned i = 0; i < n; i++)
        snap[i] = atomic_load_relaxed(&g_cpus[i].rcu_qs_count);

    // Wait for each CPU's counter to advance past the snapshot.  Our own
    // CPU has already passed QS by virtue of being outside any reader
    // section — we note that explicitly so we don't spin-wait on
    // ourselves.
    rcu_note_qs();

    for (unsigned i = 0; i < n; i++) {
        if (i == cpu_id()) continue;  // already handled above
        while (atomic_load_relaxed(&g_cpus[i].rcu_qs_count) == snap[i]) {
            // Yield so our own CPU can make forward progress and also
            // so the target CPU has a chance to run.  On UP (num_cpus==1)
            // this loop never executes.
            sched_yield();
        }
    }

    // At this point, every CPU has context-switched (or equivalently
    // passed through an idle loop iteration) at least once since the
    // grace period began.  Any reader that was active when we entered
    // synchronize_rcu must have completed.
    smp_mb();
}

// ── call_rcu ──────────────────────────────────────────────────────────────
//
// Phase 2 implementation is synchronous.  This is correct and simple.
// Phase 6 may upgrade to deferred per-CPU callback lists drained by an
// rcu_gp_kthread if profiling shows that callers frequently block on
// call_rcu and the system has many free cycles on idle CPUs.
//
// Even synchronous, this is cheap on UP because synchronize_rcu() is
// just a compiler barrier there.

void call_rcu(rcu_func_t func, void* data) {
    synchronize_rcu();
    func(data);
}
