#include "timer.h"
#include "lapic.h"
#include "idt.h"
#include "sched.h"
#include "common.h"

// ── Timer driver ──────────────────────────────────────────────────────────
// The scheduler tick is now driven by the LAPIC periodic timer, not the PIT.
// The PIT is used only for TSC calibration (see tsc.c) and is left alone
// afterwards (it continues running but its IRQ line is masked/unrouted).
//
// timer_init():
//   1. Registers the LAPIC timer vector in the IDT (VEC_LAPIC_TIMER = 0x31).
//   2. Asks the LAPIC to start firing at `hz` interrupts per second.
//
// lapic_init() must be called before timer_init().

extern void irq0_entry(void);   // assembly stub in irq_stubs.asm

static void (*s_tick_fn)(void) = NULL;

volatile uint32_t g_irq_count = 0;

void timer_register_tick(void (*fn)(void)) {
    s_tick_fn = fn;
}

// Called from irq0_entry (the LAPIC timer stub) after LAPIC EOI.
void timer_irq_handler(void) {
    g_irq_count++;
    if (s_tick_fn) s_tick_fn();
    sched_preempt();
}

void timer_init(uint32_t hz) {
    // Register the LAPIC timer vector.  irq0_entry now sends LAPIC EOI instead
    // of PIC EOI — see irq_stubs.asm.
    idt_irq_register(VEC_LAPIC_TIMER, (uint64_t)irq0_entry);

    // Start the LAPIC timer.
    lapic_timer_start(hz);
}
