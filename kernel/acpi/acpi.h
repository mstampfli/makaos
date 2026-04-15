#pragma once
#include "common.h"
#include "smp.h"     // for MAX_CPUS

// ── ACPI table discovery ──────────────────────────────────────────────────
// Parses RSDP → RSDT/XSDT → MADT to locate LAPIC and IOAPIC base addresses,
// ISA IRQ override mappings, and the list of every CPU in the system.
//
// We support both ACPI 1.0 (RSDT, 32-bit table pointers) and ACPI 2.0+
// (XSDT, 64-bit table pointers).  QEMU OVMF produces ACPI 2.0.

// Maximum ISA IRQ override entries we track (16 is the max possible).
#define ACPI_MAX_OVERRIDES 16

// One ISA interrupt source override (MADT type 2).
// Maps a legacy ISA IRQ line to a GSI (Global System Interrupt) number,
// and records polarity / trigger mode for the IOAPIC redirection entry.
typedef struct {
    uint8_t  bus;           // always 0 (ISA)
    uint8_t  isa_irq;       // source ISA IRQ (0–15)
    uint32_t gsi;           // target GSI in the IOAPIC
    uint16_t flags;         // MPS INTI flags: bits[1:0]=polarity, bits[3:2]=trigger
} acpi_override_t;

// One discovered CPU entry (MADT type 0: Processor Local APIC, or
// type 9: Processor Local x2APIC).  We keep them in discovery order.
typedef struct {
    uint32_t apic_id;        // LAPIC (or x2APIC) ID — the hardware identifier
    uint32_t acpi_proc_id;   // ACPI processor UID (informational)
    uint8_t  enabled;        // 1 = CPU is already enabled at boot
    uint8_t  online_capable; // 1 = CPU can be brought online (ACPI 6+)
} acpi_cpu_t;

typedef struct {
    uint64_t        lapic_phys;     // Local APIC physical base address
    uint64_t        ioapic_phys;    // I/O APIC physical base address
    uint32_t        ioapic_gsi_base;// first GSI handled by this IOAPIC
    acpi_override_t overrides[ACPI_MAX_OVERRIDES];
    uint8_t         override_count;

    // Per-CPU list populated from MADT.  The BSP is whichever entry has
    // its LAPIC ID matching the current CPU's LAPIC ID at runtime; the
    // ACPI tables themselves don't mark which one is the BSP.
    acpi_cpu_t      cpus[MAX_CPUS];
    uint32_t        cpu_count;      // number of entries populated

    uint8_t         ok;             // 1 if parsing succeeded
} acpi_info_t;

// Parse ACPI tables.  Must be called after the HHDM mapping is live.
// `rsdp_phys` is the physical address of the RSDP structure (from UEFI or
// BIOS).  Returns an acpi_info_t; check .ok before using the other fields.
//
// If rsdp_phys == 0 the function searches the legacy BIOS area (0xE0000–
// 0xFFFFF) for the "RSD PTR " signature — useful for SeaBIOS boots.
acpi_info_t acpi_parse(uint64_t rsdp_phys);

// Global ACPI info — populated by acpi_parse(), consumed by lapic/ioapic init.
extern acpi_info_t g_acpi;
