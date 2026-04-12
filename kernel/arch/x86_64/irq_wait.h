#pragma once
#include "common.h"

// ── IRQ wait — one owner per IRQ line ────────────────────────────────────
// Only the driver that owns an IRQ line should call irq_wait().
// Other processes wait on higher-level abstractions the driver provides
// (e.g. the keyboard driver's ring buffer + its own waiter slot).

// Sleep until the given IRQ line fires.
void irq_wait(uint8_t irq);

// Drain any accumulated pending counts for the given IRQ line.
// Call before issuing a new command to avoid consuming stale IRQs.
void irq_drain(uint8_t irq);

// Wake the process sleeping on the given IRQ line.
// Called from IRQ handlers after EOI.
void irq_notify(uint8_t irq);
