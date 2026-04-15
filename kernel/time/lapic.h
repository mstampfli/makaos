#pragma once
#include "common.h"

// ── Local APIC — x2APIC mode only ────────────────────────────────────────
//
// Every logical CPU has a Local APIC.  We run it in **x2APIC mode**: all
// register access goes through MSRs (0x800..0x83F) rather than MMIO at
// 0xFEE00000.  This gives us:
//
//   - 32-bit APIC IDs (vs 8-bit for legacy xAPIC), so systems with more
//     than 255 logical CPUs are first-class.
//   - Single-instruction atomic IPI send: one `wrmsr` to the ICR (0x830)
//     writes the full 64-bit command as a single memory transaction, no
//     Delivery Status polling needed between the high and low halves
//     (the legacy xAPIC ICR has to be written as high-then-low with a
//     poll in between — that's not atomic against another IPI landing
//     on the same CPU).
//   - Slightly faster access overall: `wrmsr`/`rdmsr` typically cost
//     ~30 cycles on modern Intel/AMD, vs 50–100 cycles for uncached
//     MMIO to the LAPIC page.
//   - No `volatile` pointer hazards, no need to pin the MMIO mapping in
//     kernel page tables, no aliasing concerns.
//
// **Hard requirement**: the CPU must support x2APIC (CPUID.01h:ECX[21]).
// `lapic_init()` panics if it doesn't.  This kernel refuses to run in
// legacy xAPIC mode.  Every host CPU made after 2008 supports x2APIC;
// QEMU with `-cpu host` exposes it unconditionally.

// ── IDT vector assignments ──────────────────────────────────────────────
// CPU exceptions occupy 0x00–0x1F.  APIC vectors start at 0x20.
// IOAPIC-routed ISA IRQs (keyboard 0x21, mouse 0x2C) land at their
// legacy vector numbers.
#define VEC_LAPIC_SPURIOUS   0x30   // spurious vector (LAPIC requires one)
#define VEC_LAPIC_TIMER      0x31   // LAPIC periodic timer (replaces PIT)
#define VEC_AHCI_MSI         0x32   // AHCI MSI
#define VEC_HDA_MSI          0x33   // HDA MSI
#define VEC_VIRTIO_NET       0x34   // virtio-net MSI (RX+TX shared)

// IPI vectors — reserved for SMP work (Phase 9-5 and onwards).
#define VEC_IPI_RESCHEDULE   0x40   // wake an idle CPU so it picks up a task
#define VEC_IPI_TLB_FLUSH    0x41   // TLB shootdown IPI
#define VEC_IPI_CALL         0x42   // generic cross-CPU function call

// ── LAPIC lifecycle ─────────────────────────────────────────────────────

// Initialise the Local APIC on the current CPU in x2APIC mode.
// Panics if the CPU does not support x2APIC.  Must be called after
// tsc_init() (the timer calibration needs a working TSC).
//
// The `lapic_phys` argument is retained for API compatibility with the
// legacy xAPIC init signature, but is **unused** in x2APIC mode — the
// MSR interface has no physical base.  We still accept it so callers
// don't all have to change at once.
void lapic_init(uint64_t lapic_phys);

// Per-CPU LAPIC re-init for APs.  Same effect as lapic_init() but skips
// the one-time timer calibration (which requires the BSP).  Called from
// the AP startup path in Phase 9-4.
void lapic_init_ap(void);

// Program the LAPIC timer to fire periodically at `hz` interrupts per
// second.  The timer IRQ is delivered at VEC_LAPIC_TIMER.  Call after
// lapic_init().
void lapic_timer_start(uint32_t hz);

// Send End-of-Interrupt.  Must be called at the end of every LAPIC-
// delivered interrupt handler.  In x2APIC this is a single `wrmsr` to
// MSR 0x80B; no serialising read required, no MMIO flush.
void lapic_eoi(void);

// Return the current CPU's 32-bit APIC ID (from MSR 0x802).  Replaces
// the legacy 8-bit version; callers that were storing the ID in a
// uint8_t must widen.
uint32_t lapic_id(void);

// ── MSI helpers (unchanged from xAPIC — MSI still uses the 0xFEE00000
// compatibility format for destination CPUs 0..255, which is fine for
// our scale) ─────────────────────────────────────────────────────────

// Build the 64-bit MSI address value that targets the BSP.  The PCI
// device writes this to its MSI address register; the chipset turns
// it into an x2APIC-compatible upstream memory write that the LAPIC
// accepts.
uint64_t lapic_msi_addr(void);

// Build the 32-bit MSI data word for a given IDT vector.
// Delivery mode 000 (Fixed), level=0, trigger=0 (edge).
uint32_t lapic_msi_data(uint8_t vector);

// ── Inter-Processor Interrupts (IPIs) ───────────────────────────────────

// Send a fixed-delivery IPI to a single target CPU identified by its
// 32-bit APIC ID.  `vector` is the IDT vector that will fire on the
// target.  Delivery is atomic (one wrmsr to MSR 0x830).
//
// The target CPU does NOT need to be running; sending an IPI to an
// idle/halted CPU wakes it from `hlt`.  This is the mechanism Phase
// 9-4 uses to start APs and Phase 9-5 uses for reschedule IPIs.
void lapic_send_ipi(uint32_t target_apic_id, uint8_t vector);

// Send the special INIT IPI to a target CPU.  Part of the INIT-SIPI-SIPI
// sequence that brings an AP out of its reset state.  Delivery mode
// 0b101, level=1 (assert), trigger=0 (edge).
void lapic_send_init(uint32_t target_apic_id);

// Send a Startup IPI (SIPI) to a target CPU.  `vector` is the
// trampoline page number (physical address / 4 KiB), so the target
// starts executing at (vector << 12).  Delivery mode 0b110.
void lapic_send_sipi(uint32_t target_apic_id, uint8_t vector);
