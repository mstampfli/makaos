#pragma once
#include "common.h"

// ── 8259A Programmable Interrupt Controller ───────────────────────────────
// Two cascaded PICs: master handles IRQs 0-7, slave handles IRQs 8-15.
// At reset, master maps to vectors 8-15 (colliding with CPU exceptions).
// pic_init() remaps them so IRQ0 → 0x20, IRQ8 → 0x28 (standard for BIOS).
//
// When an APIC (LAPIC + IOAPIC) is available, the 8259A is disabled after
// remapping by calling pic_disable().  The PIC must be remapped first so
// its vectors don't collide with CPU exceptions even when masked.

#define PIC1_CMD  0x20   // master PIC: command port
#define PIC1_DATA 0x21   // master PIC: data / IMR (interrupt mask register)
#define PIC2_CMD  0xA0   // slave  PIC: command port
#define PIC2_DATA 0xA1   // slave  PIC: data / IMR
#define PIC_EOI   0x20   // End-of-Interrupt command byte

// Remap both PICs.  Call once before enabling any hardware IRQ.
// Typical args: base_master=0x20 (IRQ0→vec32), base_slave=0x28 (IRQ8→vec40).
void pic_init(uint8_t base_master, uint8_t base_slave);

// Send EOI for the given IRQ line (0-15).
// For IRQs 8-15 (slave), sends EOI to both slave and master.
void pic_eoi(uint8_t irq);

// Mask (suppress) a specific IRQ line so the CPU never sees it.
void pic_mask(uint8_t irq);

// Unmask (enable) a specific IRQ line.
void pic_unmask(uint8_t irq);

// Disable the 8259A PICs entirely by masking all lines.
// Call this after the IOAPIC and LAPIC are initialised and all ISA IRQs have
// been routed through the IOAPIC.  The PIC hardware remains present but will
// never deliver another interrupt to the CPU.
void pic_disable(void);
