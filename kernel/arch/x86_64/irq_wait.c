#include "irq_wait.h"
#include "sched.h"
#include "process.h"
#include "wait.h"
#include "cpu.h"
#include "smp.h"

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
// s_pending is still per-IRQ-slot, still manipulated under IRQ-disabled
// context.  When SMP lands we'll add an atomic increment; for now the
// IRQ handler runs on whichever CPU the IOAPIC steered the line to,
// and that's the only CPU that bumps s_pending, so no race.
//
// NOTE: a level-triggered "armed flag + atomic xchg" redesign was
// attempted (see git history commit 48d3fea...).  Architecturally
// cleaner — no cli/sti, no saturating counter desync, handles the
// SMP check-then-add race via the standard WAIT_EVENT register-then-
// recheck pattern.  But the timing perturbation (every notify does a
// wake_all xchg, vs the old "only if queue non-empty" guard) exposed
// a pre-existing AHCI zero-data/memory-corruption bug that produces
// occasional PF-KILLs at boot.  Reverted until the underlying AHCI
// bug is fixed — at which point the level-triggered version is a
// drop-in replacement that also fixes the SMP-race hole.

#define IRQ_COUNT 256

static wait_queue_t s_wq[IRQ_COUNT];
static uint8_t      s_pending[IRQ_COUNT];  // saturating count of missed IRQs

void irq_wait_init(void) {
    for (unsigned i = 0; i < IRQ_COUNT; i++) {
        wait_queue_init(&s_wq[i]);
        s_pending[i] = 0;
    }
}

void irq_wait(uint8_t irq) {
    // Disable IRQs while we check the pending count and add ourselves
    // to the queue.  This is ONLY to prevent the IRQ firing between
    // our "check pending" and "queue entry" — the wait queue itself
    // is lock-free and doesn't need cli.
    __asm__ volatile("cli");
    if (s_pending[irq]) {
        s_pending[irq]--;
        __asm__ volatile("sti");
        return;
    }

    task_we_t node;
    task_we_init(&node, g_current);
    task_we_add(&s_wq[irq], &node);

    sched_sleep();
    __asm__ volatile("sti");
    // Always remove — sched_sleep may return via wake_pending without
    // the drain having fired, leaving a dangling stack pointer on the
    // queue.
    task_we_remove(&s_wq[irq], &node);
}

void irq_drain(uint8_t irq) {
    __asm__ volatile("cli");
    s_pending[irq] = 0;
    __asm__ volatile("sti");
}

void irq_notify(uint8_t irq) {
    if (atomic_load_acq(&s_wq[irq].head)) {
        wait_queue_wake_all(&s_wq[irq]);
    } else {
        if (s_pending[irq] < 255) s_pending[irq]++;
    }
}
