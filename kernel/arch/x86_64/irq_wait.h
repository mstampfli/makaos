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
