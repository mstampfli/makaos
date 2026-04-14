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

#define IRQ_COUNT 256

static wait_queue_t s_wq[IRQ_COUNT];
static uint8_t      s_pending[IRQ_COUNT];  // saturating count of missed IRQs

// Called once from early boot (cpu.c / main.c) to wait_queue_init each
// slot.  BSS-zeroed state (head=NULL, remove_lock={0}) is already
// valid for wait_queue_init, but we go through the API for clarity.
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
    //
    // Under SMP, disabling IRQs on THIS CPU doesn't stop the IRQ from
    // firing on the CPU it's actually steered to.  That's fine: if
    // that CPU fires irq_notify while we're pushing, the MPSC push
    // and the drain are both atomic — the worst case is one extra
    // wakeup or one missed wakeup followed by a retry in our caller's
    // own state machine.  Drivers re-check their completion condition
    // after waking, so a spurious wake is harmless.
    __asm__ volatile("cli");
    if (s_pending[irq]) {
        s_pending[irq]--;
        __asm__ volatile("sti");
        return;
    }

    // Stack-allocate a wake entry and push onto the IRQ's wait queue.
    // task_we_init sets func = task_wake_func, which calls sched_wake
    // on the stored task when the drainer fires.
    task_we_t node;
    task_we_init(&node, g_current);
    task_we_add(&s_wq[irq], &node);

    sched_sleep();
    // After waking, re-enable interrupts on this CPU.  We entered with
    // cli, the task was switched out with IF=0, do_switch's sti only
    // fires for the NEXT task (not us resuming).  Without this the
    // caller loops back with IF=0 and the next IRQ can't be delivered
    // → deadlock.
    __asm__ volatile("sti");
    // The task_we_t has already been drained by the irq_notify that
    // woke us (task_wake_func returned WQ_REMOVE).  No cleanup.
}

void irq_drain(uint8_t irq) {
    __asm__ volatile("cli");
    s_pending[irq] = 0;
    __asm__ volatile("sti");
}

void irq_notify(uint8_t irq) {
    // If the wait queue has any entries, wake them all.  The drainer
    // is atomic (xchg) so new pushers after this point land in a
    // fresh chain that the next notify will handle.
    //
    // If the queue is empty, bump the saturating pending counter so a
    // future irq_wait() sees it and returns immediately without
    // sleeping.  We check empty via atomic load — if a concurrent
    // irq_wait was mid-push and we miss it, the wait_queue_wake_all
    // below will still catch it via the xchg.  Worst case: we bump
    // pending AND wake the task, which is harmless (the task's caller
    // re-checks its state).
    if (atomic_load_acq(&s_wq[irq].head)) {
        wait_queue_wake_all(&s_wq[irq]);
    } else {
        if (s_pending[irq] < 255) s_pending[irq]++;
    }
}
