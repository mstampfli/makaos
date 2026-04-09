#pragma once
#include "common.h"

// ── Timer abstraction ─────────────────────────────────────────────────────
// Backed by the LAPIC periodic timer (timer_pit.c).
// The PIT is used only for TSC calibration at boot; scheduling ticks are
// delivered at VEC_LAPIC_TIMER (0x31) by the LAPIC.
//
// The abstraction is intentionally minimal: one tick callback, one init call.

// Initialise the timer to fire at `hz` interrupts per second.
// Registers the appropriate IDT entry and unmasks the IRQ.
void timer_init(uint32_t hz);

// Register the function called on every timer tick (typically sched_tick).
// Only one callback is supported — the scheduler owns it.
void timer_register_tick(void (*fn)(void));

// Called by the IRQ0 assembly stub (irq_stubs.asm).  Do not call directly.
void timer_irq_handler(void);
