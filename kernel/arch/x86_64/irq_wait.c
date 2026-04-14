#include "irq_wait.h"
#include "sched.h"
#include "process.h"

// Per-IRQ waiter list: intrusive singly-linked list of irq_waiter_t nodes.
// Each node is stack-allocated by the sleeping thread so no heap allocation
// is needed.  When the IRQ fires, ALL waiters are woken (broadcast) — each
// driver re-checks its own hardware completion condition.
//
// IRQ_COUNT covers the 16 legacy PIC lines plus 16 MSI/MSI-X logical slots
// assigned by drivers (e.g. virtio-net at slot 4, HDA at its PCI IRQ, etc.).
// The range is large enough to never need increasing.

#define IRQ_COUNT 256

typedef struct irq_waiter {
    task_t*           task;
    struct irq_waiter* next;
} irq_waiter_t;

static irq_waiter_t* s_head[IRQ_COUNT];  // head of waiter list per IRQ
static uint8_t       s_pending[IRQ_COUNT]; // saturating count of unhandled IRQs

void irq_wait(uint8_t irq) {
    // irq is uint8_t so always < 256 == IRQ_COUNT; no bounds check needed.
    __asm__ volatile("cli");
    if (s_pending[irq]) {
        s_pending[irq]--;
        __asm__ volatile("sti");
        return;
    }
    // Stack-allocate the waiter node and push onto the IRQ's list.
    irq_waiter_t node;
    node.task = g_current;
    node.next = s_head[irq];
    s_head[irq] = &node;

    sched_sleep();
    // Re-enable interrupts after waking.  We entered with cli; the task was
    // switched out with IF=0 and do_switch's sti only fires at the very end.
    // Without this the caller loops back with IF=0 and the next IRQ cannot
    // be delivered, causing a deadlock.
    __asm__ volatile("sti");
    // node is now off the list (irq_notify removed it); no cleanup needed.
}

void irq_drain(uint8_t irq) {
    __asm__ volatile("cli");
    s_pending[irq] = 0;
    __asm__ volatile("sti");
}

void irq_notify(uint8_t irq) {
    // Wake and drain the entire waiter list for this IRQ line.
    irq_waiter_t* list = s_head[irq];
    s_head[irq] = NULL;
    if (list) {
        irq_waiter_t* w = list;
        while (w) {
            irq_waiter_t* nx = w->next;
            sched_wake(w->task);
            w = nx;
        }
    } else {
        if (s_pending[irq] < 255) s_pending[irq]++;
    }
}
