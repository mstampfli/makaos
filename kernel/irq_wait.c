#include "irq_wait.h"
#include "sched.h"
#include "process.h"

// One waiter per IRQ line.  Only the driver that owns the IRQ registers here.
#define IRQ_COUNT 16
static task_t* s_waiters[IRQ_COUNT] = {0};

void irq_wait(uint8_t irq) {
    if (irq >= IRQ_COUNT) return;
    s_waiters[irq] = g_current;
    sched_sleep();
}

void irq_notify(uint8_t irq) {
    if (irq >= IRQ_COUNT) return;
    task_t* p = s_waiters[irq];
    s_waiters[irq] = NULL;
    if (p) sched_wake(p);
}
