#include "rcu.h"
#include "cpu.h"
#include "sched.h"
#include "common.h"
#include "ipi.h"

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
            // Busy-wait with pause.  We deliberately do NOT sched_yield
            // here: sched_yield on an otherwise-idle CPU drops into
            // do_switch's sti/hlt loop and never returns, so the while
            // predicate is never re-checked and this thread hangs
            // forever waiting for a counter it can't observe.
            //
            // cpu_relax() is the correct primitive: it's a single
            // `pause` that tells the CPU we're in a spin loop (so
            // store-buffer ordering and SMT co-scheduling can react),
            // and it lets timer IRQs and IPIs continue firing on this
            // CPU — so our own preempt check, sched_tick, and any
            // cross-CPU wake can still make progress.
            //
            // The target CPU will advance its counter via its own
            // timer tick (sched_preempt → do_switch → rcu_note_qs),
            // a context switch, or its idle loop — none of which
            // depend on us yielding.  So we just spin and watch.
            cpu_relax();
        }
    }

    // At this point, every CPU has context-switched (or equivalently
    // passed through an idle loop iteration) at least once since the
    // grace period began.  Any reader that was active when we entered
    // synchronize_rcu must have completed.
    smp_mb();
}

// ── synchronize_rcu_expedited ────────────────────────────────────────────
//
// Classic synchronize_rcu waits for every CPU to organically pass
// through a quiescent state (timer tick, context switch, idle loop) —
// that's up to ~1 ms per CPU on a busy system, more on idle.
//
// Expedited forces the question: send every CPU a no-op IPI.  When the
// IPI handler runs, if preempt_depth == 0 the target was provably
// outside any RCU reader at that instant — the IPI itself marks the
// end of the grace period for that CPU.  If preempt_depth > 0 the
// target is mid-reader, so we fall through to the classic wait for
// this CPU only; that's still cheap because the reader completes with
// a plain preempt_enable (no IPI needed).
//
// Correctness: rcu_read_lock == preempt_disable.  Any reader active
// before our snapshot was running with preempt_depth >= 1.  Either:
//   (a) the reader ends before our IPI arrives → preempt_depth == 0
//       when the callback runs → callback notes QS → done.
//   (b) our IPI interrupts the reader → preempt_depth > 0 → callback
//       returns without noting QS → we fall through to the counter
//       wait → reader's preempt_enable bumps rcu_qs_count eventually
//       (at the next sched_tick or voluntary yield).
// Either way, every reader active at snapshot time has completed
// before synchronize_rcu_expedited returns.
//
// Preempt_depth 0 implies the interrupted code was outside any RCU
// reader section.  IPI handlers never enter RCU reader sections
// themselves (they don't call rcu_read_lock), so the check is
// trivially correct.
static void rcu_expedited_cb(void* arg) {
    (void)arg;
    // This runs on the target CPU in IPI context.  If preempt_depth is
    // 0 right now, the interrupted code was outside any reader section
    // — this IPI itself is a quiescent state.  Bump the counter so any
    // waiter observes our QS without needing the target to context-
    // switch.
    //
    // If preempt_depth > 0 the target is mid-reader.  We can't claim
    // QS here; the sender will fall through to the classic spin,
    // waiting for the reader's eventual preempt_enable + tick.
    if (this_cpu()->preempt_depth == 0)
        this_cpu()->rcu_qs_count++;
}

void synchronize_rcu_expedited(void) {
    if (this_cpu()->preempt_depth > 0) {
        serial_puts_dbg("[rcu] PANIC: synchronize_rcu_expedited with preempt disabled\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    unsigned n = num_cpus();
    uint64_t snap[MAX_CPUS];

    for (unsigned i = 0; i < n; i++)
        snap[i] = atomic_load_relaxed(&g_cpus[i].rcu_qs_count);

    // Our own CPU is outside any reader (preempt_depth == 0, checked
    // above).  Note QS immediately so we never wait on ourselves.
    rcu_note_qs();

    // IPI every remote CPU.  Each target's rcu_expedited_cb runs
    // inline in its IPI handler, bumps its own counter if it was
    // outside a reader.  smp_call_function_single blocks until the
    // callback has executed (done=1), so after this loop returns
    // every CPU has either (a) advanced its counter, or (b) signalled
    // that it's mid-reader and will advance shortly.
    uint32_t me = this_cpu()->id;
    for (unsigned i = 0; i < n; i++) {
        if (i == me) continue;
        smp_call_function_single((uint32_t)i, rcu_expedited_cb, NULL);
    }

    // Fallback classic wait — handles case (b) above.  For CPUs where
    // rcu_expedited_cb already advanced the counter this is a single
    // load that fails the loop predicate on first check.
    for (unsigned i = 0; i < n; i++) {
        if (i == me) continue;
        while (atomic_load_relaxed(&g_cpus[i].rcu_qs_count) == snap[i])
            cpu_relax();
    }

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

// ── Deferred kfree — convenience wrapper ──────────────────────────────
// Used by anything that frees heap memory which may still be
// referenced by a concurrent RCU reader (wait_queue_t drainer,
// pid_ht lookup, vma walker, etc.).  Bypasses the per-caller boilerplate
// of writing a kfree trampoline every time.
#include "kheap.h"
static void kfree_rcu_cb(void* data) { kfree(data); }
void kfree_rcu(void* ptr) { call_rcu(kfree_rcu_cb, ptr); }
