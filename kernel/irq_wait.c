#include "irq_wait.h"
#include "sched.h"
#include "process.h"

// One waiter + one pending counter per IRQ line.
// s_pending[irq] is incremented when irq_notify fires with no sleeping waiter,
// so the driver thread never misses an IRQ even if it was still processing.
#define IRQ_COUNT 16
static task_t* s_waiters[IRQ_COUNT];
static uint8_t s_pending[IRQ_COUNT];

// Sleep until irq fires.  Atomically checks the pending counter first so an
// IRQ that fired before we slept is not lost.  cli/sti guard the check-and-set
// against the IRQ firing between the check and the sleep (single CPU).
void irq_wait(uint8_t irq) {
    if (irq >= IRQ_COUNT) return;
    __asm__ volatile("cli");
    if (s_pending[irq]) {
        s_pending[irq]--;
        __asm__ volatile("sti");
        return;
    }
    s_waiters[irq] = g_current;
    sched_sleep();   // re-enables interrupts via do_switch → sti
}

// Called from ISR context (interrupts disabled).
// Wakes the sleeping driver thread, or records a pending count if nobody is
// waiting yet (driver still processing the previous IRQ).
void irq_notify(uint8_t irq) {
    if (irq >= IRQ_COUNT) return;
    task_t* p = s_waiters[irq];
    if (p) {
        s_waiters[irq] = NULL;
        sched_wake(p);
    } else {
        if (s_pending[irq] < 255) s_pending[irq]++;
    }
}
