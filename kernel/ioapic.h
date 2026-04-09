#pragma once
#include "common.h"
#include "acpi.h"

// ── I/O APIC ─────────────────────────────────────────────────────────────
// The IOAPIC routes ISA/PCI INTx lines to LAPIC vectors.
// Each IOAPIC has a base GSI; its redirection table covers
// [gsi_base, gsi_base + n_entries).
//
// We support one IOAPIC (sufficient for single-socket systems with ≤ 24 IRQs).
// Future SMP work may add a second IOAPIC for additional slots.

// Initialise the IOAPIC.
//   info — parsed ACPI data (IOAPIC physical address, GSI base, overrides)
//
// Programs the redirection table for the ISA IRQs we use:
//   ISA IRQ 0 (PIT)        → VEC_LAPIC_TIMER  (masked; LAPIC timer replaces it)
//   ISA IRQ 1 (keyboard)   → 0x21
//   ISA IRQ 12 (mouse)     → 0x2C
// All entries are edge-triggered, active-high, fixed delivery to BSP LAPIC (ID 0).
// IRQs not listed above are left masked.
void ioapic_init(const acpi_info_t* info);

// Mask or unmask a GSI in the IOAPIC redirection table.
// Use this instead of pic_mask/pic_unmask for IOAPIC-routed interrupts.
void ioapic_mask(uint32_t gsi);
void ioapic_unmask(uint32_t gsi);

// Map a legacy ISA IRQ to its GSI, applying any ACPI override.
// Most ISA IRQs map 1:1 (GSI == IRQ), but some (e.g. IRQ 0 → GSI 2 on
// some chipsets) are remapped by ACPI interrupt source overrides.
uint32_t ioapic_isa_to_gsi(uint8_t isa_irq);
