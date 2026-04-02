#include "timer.h"
#include "pic.h"
#include "idt.h"

// ── PIT (8253/8254) timer driver ──────────────────────────────────────────
// The PIT has a fixed input clock of 1,193,182 Hz.
// To get a tick frequency of `hz`, write divisor = 1193182 / hz to channel 0.
//
// Channel 0 is connected to IRQ0 on the master PIC.
// After pic_init(), IRQ0 maps to IDT vector 0x20.

#define PIT_CHANNEL0 0x40   // channel 0 data port
#define PIT_CMD      0x43   // mode/command register
#define PIT_BASE_HZ  1193182UL

static void (*s_tick_fn)(void) = NULL;

volatile uint32_t g_irq_count = 0;  // debug: incremented on every IRQ0

void timer_register_tick(void (*fn)(void)) {
    s_tick_fn = fn;
}

// Called from irq0_entry in irq_stubs.asm after EOI is sent.
void timer_irq_handler(void) {
    g_irq_count++;
    if (s_tick_fn) s_tick_fn();
}

// Assembly stub for IRQ0; defined in irq_stubs.asm.
extern void irq0_entry(void);

void timer_init(uint32_t hz) {
    // Program channel 0: lobyte/hibyte access, mode 2 (rate generator).
    // Mode 2 fires once per divisor cycles and reloads automatically.
    uint32_t divisor = (uint32_t)(PIT_BASE_HZ / hz);
    outb(PIT_CMD,      0x34);                          // ch0, lo/hi, mode 2
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    // Register the IRQ0 handler at vector 0x20.
    idt_irq_register(0x20, (uint64_t)irq0_entry);

    // Unmask IRQ0 on the master PIC so interrupts can reach the CPU.
    pic_unmask(0);
}
