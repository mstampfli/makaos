#pragma once
#include "common.h"

// ── Local APIC (xAPIC, MMIO mode) ────────────────────────────────────────
// Each logical CPU has one LAPIC.  In xAPIC mode it is memory-mapped at a
// 4 KiB-aligned physical address (default 0xFEE00000, overridable via ACPI).
//
// The LAPIC handles:
//   • Receiving MSI interrupts from PCI devices
//   • Receiving IOAPIC-routed interrupts (ISA keyboard, mouse, timer)
//   • Sending IPIs between CPUs (future SMP use)
//   • The LAPIC timer (replaces the PIT for scheduling)
//
// We use xAPIC (not x2APIC) because it is universally supported and simpler
// for a single-CPU kernel.  Switching to x2APIC later is a clean upgrade.

// IDT vector assignments for APIC-delivered interrupts.
// CPU exceptions occupy 0x00–0x1F.  We start APIC vectors at 0x20.
// PIC-routed ISA IRQs (keyboard 0x21, mouse 0x2C) keep their existing vectors
// because they go through the IOAPIC which delivers the same vector numbers.
#define VEC_LAPIC_SPURIOUS  0x30   // spurious vector (LAPIC requires one)
#define VEC_LAPIC_TIMER     0x31   // LAPIC periodic timer (replaces PIT IRQ0)
#define VEC_AHCI_MSI        0x32   // AHCI MSI
#define VEC_HDA_MSI         0x33   // HDA MSI
#define VEC_VIRTIO_NET      0x34   // virtio-net MSI (RX+TX shared vector)

// Initialise the Local APIC.
//   lapic_phys  — physical base from ACPI (or 0xFEE00000 as default)
//
// Sets up the spurious vector, enables the LAPIC, and calibrates the LAPIC
// timer against the TSC so it can fire at any requested frequency.
// Must be called after vmm_init() (needs MMIO mapping) and tsc_init().
void lapic_init(uint64_t lapic_phys);

// Program the LAPIC timer to fire periodically at `hz` interrupts per second.
// The timer IRQ is delivered at VEC_LAPIC_TIMER.
// Call after lapic_init().
void lapic_timer_start(uint32_t hz);

// Send End-of-Interrupt to the LAPIC.  Must be called at the end of every
// LAPIC-delivered interrupt handler (replaces pic_eoi() for APIC IRQs).
// Safe to call from any IRQ handler that is delivered via the LAPIC.
void lapic_eoi(void);

// Return the LAPIC ID of the current CPU (bits [27:24] of the ID register).
uint8_t lapic_id(void);

// Return the MMIO virtual base of the LAPIC (for drivers that need to write
// the MSI address register).  The physical address is 0xFEE00000 for most
// systems; the MSI address written to PCI config space is also 0xFEE00000.
uint64_t lapic_msi_addr(void);

// Build the 32-bit MSI data word for a given IDT vector.
// Delivery mode 000 (Fixed), level=0 (edge), trigger=0 (edge).
uint32_t lapic_msi_data(uint8_t vector);
