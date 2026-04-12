#include "irq_wait.h"
#include "sched.h"
#include "process.h"

// Per-IRQ waiter list: up to MAX_IRQ_WAITERS tasks can sleep on a single
// IRQ line simultaneously.  When the IRQ fires, ALL waiters are woken
// (broadcast) — each driver re-checks its own completion condition.
#define IRQ_COUNT 16
#define MAX_IRQ_WAITERS 16

static task_t* s_waiters[IRQ_COUNT][MAX_IRQ_WAITERS];
static uint8_t s_nwaiters[IRQ_COUNT];
static uint8_t s_pending[IRQ_COUNT];

void irq_wait(uint8_t irq) {
    if (irq >= IRQ_COUNT) return;
    __asm__ volatile("cli");
    if (s_pending[irq]) {
        s_pending[irq]--;
        __asm__ volatile("sti");
        return;
    }
    if (s_nwaiters[irq] < MAX_IRQ_WAITERS) {
        s_waiters[irq][s_nwaiters[irq]++] = g_current;
    }
    sched_sleep();
    // Re-enable interrupts after waking.  We entered with cli; the task was
    // switched out with IF=0 and the do_switch sti only fires at the very end.
    // Without this, the caller (issue_one) loops back with IF=0 and the next
    // AHCI completion IRQ can't be delivered, causing a deadlock.
    __asm__ volatile("sti");
}

void irq_drain(uint8_t irq) {
    if (irq >= IRQ_COUNT) return;
    __asm__ volatile("cli");
    s_pending[irq] = 0;
    __asm__ volatile("sti");
}

void irq_notify(uint8_t irq) {
    if (irq >= IRQ_COUNT) return;
    uint8_t n = s_nwaiters[irq];
    if (n) {
        s_nwaiters[irq] = 0;
        for (uint8_t i = 0; i < n; i++) {
            sched_wake(s_waiters[irq][i]);
            s_waiters[irq][i] = NULL;
        }
    } else {
        if (s_pending[irq] < 255) s_pending[irq]++;
    }
}
