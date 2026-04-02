#pragma once
#include "common.h"

// ── Timer abstraction ─────────────────────────────────────────────────────
// Currently backed by the 8253/8254 PIT (timer_pit.c).
//
// To add APIC timer support later:
//   1. Write timer_apic.c implementing the same three functions below.
//   2. In kmain, call timer_apic_init() instead of (or after disabling) PIT.
//   3. The scheduler and everything above this layer is unchanged.
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
