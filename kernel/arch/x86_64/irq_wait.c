#include "irq_wait.h"
#include "sched.h"
#include "process.h"
#include "wait.h"
#include "cpu.h"
#include "smp.h"
#include "preempt.h"

// ── Per-IRQ waiter queues — lock-free MPSC ──────────────────────────────
//
// Each IRQ slot owns a wait_queue_t.  Drivers sleep on the queue via
// irq_wait(); the IRQ handler fires wait_queue_wake_all() from
// irq_notify().
//
// Why this is SMP-correct:
//   - wq_add (push) is lock-free via CAS.  Multiple CPUs could in
//     principle call irq_wait for the same slot concurrently; the CAS
//     loop handles that.
//   - wait_queue_wake_all drains the chain with one atomic xchg.  Even
//     under cross-CPU races (CPU A adds while CPU B notifies), the
//     drain is atomic — the adder either wins (entry is on the new
//     chain and will be drained by the next wake) or loses (entry is
//     in the drainer's private chain and gets woken now).  Either way,
//     correct.
//   - Stack-allocated wake_entry is safe because the task stays in
//     sched_sleep until woken, so the stack frame is live for the
//     duration of the drain.
//
// s_pending is a saturating per-IRQ credit counter, updated with atomic
// CAS on both sides: irq_notify can run on a different CPU each delivery
// (IOAPIC steering) and waiters CAS-decrement concurrently, so plain ++/--
// under local `cli` loses counts.  The waiter uses the standard
// register-then-recheck pattern (enqueue first, then re-consume pending)
// so a notify landing in the check→enqueue gap is never lost — this
// closed the SMP lost-wakeup hole an earlier revision of this file
// documented but deferred (the AHCI corruption that reverting hid has
// since been fixed at the root: frame refcount chokepoint + TLB-shootdown
// fence + i8042 serialization).

#define IRQ_COUNT 256

static wait_queue_t s_wq[IRQ_COUNT];
static uint8_t      s_pending[IRQ_COUNT];  // saturating count of missed IRQs

void irq_wait_init(void) {
    for (unsigned i = 0; i < IRQ_COUNT; i++) {
        wait_queue_init(&s_wq[i]);
        s_pending[i] = 0;
    }
}

// Atomically consume one pending-IRQ credit.  Returns 1 if consumed.
// CAS loop because irq_notify on ANOTHER CPU increments concurrently —
// `cli` is local-only and cannot order cross-CPU RMWs.
static inline int pending_consume(uint8_t irq) {
    uint8_t cur = __atomic_load_n(&s_pending[irq], __ATOMIC_ACQUIRE);
    while (cur) {
        if (__atomic_compare_exchange_n(&s_pending[irq], &cur,
                                        (uint8_t)(cur - 1), 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 1;
        // cur reloaded by the failed CAS; loop until 0 or consumed.
    }
    return 0;
}

void irq_wait(uint8_t irq) {
    // Lost-wake-free register-then-recheck (same pattern as WAIT_EVENT):
    //
    //   waiter                          irq_notify (other CPU)
    //   ────────────────────────        ───────────────────────────
    //   1. fast-path consume? no
    //                                   sees queue empty → bump pending
    //   2. enqueue on s_wq[irq]
    //   3. re-check pending  ←───────── catches the bump from the gap
    //   4. sched_sleep()
    //                                   sees queue non-empty → wake_all
    //                                   (wake_pending protection in
    //                                    sched_sleep covers this side)
    //
    // Any notify before the enqueue is observed by step 3; any notify
    // after the enqueue sees a non-empty queue and wakes us.  The old
    // version guarded the check-then-enqueue with `cli` only, which does
    // nothing against a notify running on another CPU — the classic
    // SMP lost wakeup this file's header used to apologize for.
    if (pending_consume(irq)) return;

    // Use g_current->sleep_we (persistent) rather than a stack local so
    // a drain firing after sched_sleep returns via wake_pending and our
    // frame pops can never dereference freed memory.
    task_we_init(&g_current->sleep_we, g_current);
    task_we_add(&s_wq[irq], &g_current->sleep_we);

    if (pending_consume(irq)) {
        task_we_remove(&s_wq[irq], &g_current->sleep_we);
        return;
    }

    sched_sleep();
    task_we_remove(&s_wq[irq], &g_current->sleep_we);
}

void irq_drain(uint8_t irq) {
    __atomic_store_n(&s_pending[irq], 0, __ATOMIC_RELEASE);
}

void irq_notify(uint8_t irq) {
    // Phase 9A: mix TSC-at-IRQ into the kernel entropy pool.  Each
    // IRQ's exact arrival time carries jitter from interrupt
    // controllers + DRAM + bus contention — a real entropy source.
    // Lockless (per-CPU fast-mix slot); zero overhead on the IRQ
    // hot path beyond one tsc_read_ns() + a few XORs.
    extern uint64_t tsc_read_ns(void);
    extern void kcsprng_mix_irq(uint8_t, uint64_t);
    kcsprng_mix_irq(irq, tsc_read_ns());

    // preempt_disable around wake_all: this is called from ISR context
    // (keyboard/mouse/hda/ac97/virtio-net/...).  Without this, wake_all's
    // rcu_read_unlock → preempt_enable → sched_preempt → do_switch → `sti`
    // re-enables IRQs inside the ISR and lets a fresh interrupt of the
    // same vector nest, racing on per-driver state.  Direct dec at the
    // end — preempt_enable() would itself re-trigger that path.
    if (atomic_load_acq(&s_wq[irq].head)) {
        preempt_disable();
        wait_queue_wake_all(&s_wq[irq]);
        preempt_enable_no_resched();
    } else {
        // Atomic saturating increment: the same IRQ line can be delivered
        // to a different CPU each time (IOAPIC), and a waiter on a third
        // CPU CAS-decrements concurrently — a plain ++ here loses counts.
        uint8_t cur = __atomic_load_n(&s_pending[irq], __ATOMIC_RELAXED);
        while (cur < 255 &&
               !__atomic_compare_exchange_n(&s_pending[irq], &cur,
                                            (uint8_t)(cur + 1), 0,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED))
            ; // cur reloaded by failed CAS
    }
}
