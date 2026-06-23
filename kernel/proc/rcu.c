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

// ── call_rcu (Phase 5B: fully asynchronous) ────────────────────────────
//
// call_rcu_head is the primitive: lock-free push of a caller-supplied
// rcu_head_t onto this CPU's pending list.  The rcu_gp_kthread
// (spawned from init_kthread) periodically snapshots every CPU's
// pending chain, blocks on synchronize_rcu(), then invokes each
// callback in process context.
//
// Invariants:
//   - call_rcu_head does ZERO allocation and is safe to call while
//     holding any lock, including g_pmm_lock, even from IRQ-disabled
//     code.  The rcu_head_t is caller-provided (typically embedded
//     in the freed object).
//   - The callback fires in the GP kthread's context: IRQs enabled,
//     preemptible, no locks held.  It can call kmalloc, synchronize_rcu
//     recursively, take sleeping locks, etc.  Never use the GP
//     kthread to invoke callbacks that would block forever — that
//     starves all further RCU-deferred reclaim.
//   - The callback runs at most one grace period after the call_rcu_head
//     returns.  Latency: typically ~10 ms on SMP, instant on UP.
//   - Between push and callback invocation, the memory backing the
//     rcu_head_t must remain valid.  Embedding the head in the object
//     being freed satisfies this trivially.

// Lock-free push of `head` onto a SPECIFIC cpu's pending list.  The CAS
// loop makes a cross-CPU push as safe as a local one, so rcu_barrier() can
// place a barrier on every CPU's list (not just the local one).  One shared
// mechanism; call_rcu_head is just this with c = this_cpu().
static void rcu_push_on_cpu(cpu_t* c, rcu_head_t* head,
                            rcu_func_t func, void* data) {
    head->func = func;
    head->data = data;
    rcu_head_t* old = (rcu_head_t*)__atomic_load_n(&c->rcu_pending_head,
                                                    __ATOMIC_RELAXED);
    do {
        head->next = old;
    } while (!__atomic_compare_exchange_n(
                 (rcu_head_t**)&c->rcu_pending_head,
                 &old, head,
                 0,
                 __ATOMIC_RELEASE,
                 __ATOMIC_RELAXED));
}

void call_rcu_head(rcu_head_t* head, rcu_func_t func, void* data) {
    rcu_push_on_cpu(this_cpu(), head, func, data);
}

// Layout compatibility check: pmm.h declares a bit-identical
// pmm_rcu_head_t so slab_header_t can embed an RCU head without
// pulling rcu.h into pmm.h.  If the layouts ever drift, the cast in
// pmm_slab_destroy_locked's call_rcu_head call becomes a strict-
// aliasing violation and the build must fail.
#include "pmm.h"
_Static_assert(sizeof(pmm_rcu_head_t) == sizeof(rcu_head_t),
               "pmm_rcu_head_t must match rcu_head_t size");
_Static_assert(__builtin_offsetof(pmm_rcu_head_t, next) ==
               __builtin_offsetof(rcu_head_t,     next),
               "rcu_head next offset mismatch");
_Static_assert(__builtin_offsetof(pmm_rcu_head_t, func) ==
               __builtin_offsetof(rcu_head_t,     func),
               "rcu_head func offset mismatch");
_Static_assert(__builtin_offsetof(pmm_rcu_head_t, data) ==
               __builtin_offsetof(rcu_head_t,     data),
               "rcu_head data offset mismatch");

void call_rcu_expedited(rcu_func_t func, void* data) {
    // Expedited is still synchronous by design — IPI-forced grace
    // period, intended for latency-sensitive user-syscall-return
    // paths that want the callback before they unblock.  Not used
    // by the pmm slab path.
    synchronize_rcu_expedited();
    func(data);
}

// ── rcu_gp_kthread — drains per-CPU pending callbacks ──────────────────
// Sleeps for RCU_GP_INTERVAL_NS between passes.  Each pass:
//   1. Atomically snapshots each CPU's pending head into a local chain
//      (single atomic_exchange per CPU — can't block remote pushers).
//   2. Stitches all CPU chains into one combined chain.
//   3. synchronize_rcu() — blocks until a grace period elapses.
//   4. Invokes every callback.
//
// Nothing races with the callback invocation: after synchronize_rcu
// returns, every pre-snapshot reader section has ended, so no reader
// is still holding a pointer into the memory the callback is about
// to free.

#define RCU_GP_INTERVAL_NS (10ULL * 1000 * 1000)   // 10 ms

extern uint64_t tsc_read_ns(void);
extern void     sched_sleep(void);
#include "process.h"
#include "sched.h"

static void rcu_gp_kthread_entry(void) {
    for (;;) {
        // Snapshot all CPUs' pending lists into a single local chain.
        rcu_head_t* collected = NULL;
        unsigned n = g_num_cpus;
        for (unsigned i = 0; i < n; i++) {
            rcu_head_t* cpu_chain = (rcu_head_t*)__atomic_exchange_n(
                (rcu_head_t**)&g_cpus[i].rcu_pending_head,
                NULL,
                __ATOMIC_ACQUIRE);
            if (!cpu_chain) continue;
            // The per-CPU list is a LIFO push-stack (call_rcu_head prepends).
            // Reverse it to FIFO order (oldest first) so callbacks fire in
            // the order they were queued ON THAT CPU.  rcu_barrier() relies
            // on this: its per-CPU barrier, pushed last, must be invoked
            // LAST for that CPU — after every callback queued before it.
            rcu_head_t* fifo = NULL;
            while (cpu_chain) {
                rcu_head_t* nx = cpu_chain->next;
                cpu_chain->next = fifo;
                fifo = cpu_chain;
                cpu_chain = nx;
            }
            // Append this CPU's FIFO chain to collected (find tail, link).
            rcu_head_t* tail = fifo;
            while (tail->next) tail = tail->next;
            tail->next = collected;
            collected  = fifo;
        }

        if (collected) {
            // Wait for a grace period to close.  Every pre-snapshot
            // reader must have passed through a QS by now.
            synchronize_rcu();

            // Invoke every callback.  Order doesn't matter —
            // callbacks are independent.
            while (collected) {
                rcu_head_t* next = collected->next;
                rcu_func_t  func = collected->func;
                void*       data = collected->data;
                // Invoke BEFORE reading next — safety: the callback
                // may free `collected` itself (common with embedded
                // heads).  But we already saved func/data/next, so
                // touching collected after would be UAF.
                func(data);
                collected = next;
            }
            continue;   // tight loop if work is pending
        }

        // Idle: sleep for RCU_GP_INTERVAL_NS.  New call_rcu pushes
        // during the sleep will be picked up on the next pass.
        uint64_t wake_ns = tsc_read_ns() + RCU_GP_INTERVAL_NS;
        while (tsc_read_ns() < wake_ns) {
            g_current->sleep_until_ns = wake_ns;
            sched_sleep();
            g_current->sleep_until_ns = 0;
        }
    }
}

void rcu_gp_kthread_start(void) {
    task_t* t = task_create_kthread(rcu_gp_kthread_entry, pid_alloc());
    if (t) sched_add(t);
}

// ── rcu_barrier — block until all already-queued callbacks have run ─────
// Implementation strategy: enqueue an rcu_head on each CPU's pending
// list whose callback increments a counter.  Wait until every CPU's
// barrier callback has fired.  FIFO property of the GP kthread
// guarantees every call_rcu_head that happened-before ours on that
// CPU fires first (same-CPU ordering) or at the latest together in
// the same batch drain.
//
// Per-CPU barrier heads live on the caller's stack — the caller is
// blocked in rcu_barrier() until all of them fire, so stack lifetime
// is guaranteed.

typedef struct {
    rcu_head_t        head;
    volatile uint32_t* done_count;
} rcu_barrier_t;

static void rcu_barrier_cb(void* data) {
    rcu_barrier_t* b = (rcu_barrier_t*)data;
    __atomic_fetch_add(b->done_count, 1, __ATOMIC_ACQ_REL);
}

void rcu_barrier(void) {
    unsigned n = g_num_cpus;
    if (n == 0) return;
    rcu_barrier_t    barriers[MAX_CPUS];
    volatile uint32_t done = 0;

    // Push one barrier onto EACH CPU's pending list (rcu_push_on_cpu's CAS
    // makes the cross-CPU push safe).  Combined with the GP kthread's
    // per-CPU FIFO invocation, CPU i's barrier is invoked after every
    // callback that was queued on CPU i before rcu_barrier() ran.  So once
    // all N barriers have fired, every pre-barrier callback on every CPU has
    // also fired — the correct rcu_barrier guarantee.
    //
    // The earlier design pushed all N barriers on the LOCAL CPU, which only
    // ordered against local-CPU callbacks: a callback queued on a different
    // CPU (e.g. a typesafe slab free from pmm_slab_shrink_all_locked running
    // elsewhere) could still be pending when rcu_barrier() returned, an
    // intermittent use-after-reclaim (caught as is_slab_ptr=1 after barrier).
    for (unsigned i = 0; i < n; i++) {
        barriers[i].done_count = &done;
        rcu_push_on_cpu(&g_cpus[i], &barriers[i].head,
                        rcu_barrier_cb, &barriers[i]);
    }

    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) < n) {
        sched_yield();
    }
}
