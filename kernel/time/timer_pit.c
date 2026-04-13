#include "timer.h"
#include "lapic.h"
#include "idt.h"
#include "sched.h"
#include "common.h"

extern void irq0_entry(void);   // assembly stub in irq_stubs.asm

static void (*s_tick_fn)(void) = NULL;

volatile uint32_t g_irq_count = 0;

void timer_register_tick(void (*fn)(void)) {
    s_tick_fn = fn;
}

// Called from irq0_entry (LAPIC timer stub) after LAPIC EOI.
void timer_irq_handler(void) {
    g_irq_count++;
    if (s_tick_fn) s_tick_fn();
    sched_preempt();
}

void timer_init(uint32_t hz) {
    idt_irq_register(VEC_LAPIC_TIMER, (uint64_t)irq0_entry);
    lapic_timer_start(hz);
}
