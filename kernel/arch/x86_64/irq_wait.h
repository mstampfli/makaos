#pragma once
#include "common.h"

// ── IRQ wait ─────────────────────────────────────────────────────────────
// Drivers sleep on an IRQ slot via irq_wait(); the IRQ handler wakes all
// waiters via irq_notify().  Waiter nodes are stack-allocated by the caller
// so no heap allocation occurs in the hot path.
//
// Slot numbers: 0–15 = legacy PIC IRQ lines.  16–255 = logical slots
// assigned freely by drivers (e.g. virtio-net at slot 4, HDA at its PCI
// IRQ number, AHCI at its PCI IRQ, etc.).  No fixed cap on waiters per slot.

// Sleep until the given IRQ slot fires (or a pending count is available).
void irq_wait(uint8_t irq);

// Drain any accumulated pending counts for the given IRQ slot.
// Call before issuing a new command to avoid consuming stale IRQs.
void irq_drain(uint8_t irq);

// Wake all tasks sleeping on the given IRQ slot (broadcast).
// Called from IRQ handlers after EOI.
void irq_notify(uint8_t irq);

// One-time init for the per-IRQ wait queues.  Call from kmain before
// any driver registers an IRQ handler.
void irq_wait_init(void);

// Return the per-slot wait_queue backing `irq`, so a device file can hang
// its poll/epoll waiters on the very queue irq_notify() already drains.
// The queue is woken preempt-safely from irq_notify() in ISR context, so a
// pollable device (e.g. /dev/dsp) gets its POLLOUT/POLLIN wakeups for free
// with no second wake site -- the DMA-completion ISR is the single source.
struct wait_queue_t;
struct wait_queue_t* irq_waitq(uint8_t irq);
