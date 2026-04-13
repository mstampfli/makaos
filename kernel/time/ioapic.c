#include "ioapic.h"
#include "lapic.h"
#include "vmm.h"
#include "common.h"

// ── IOAPIC MMIO interface ─────────────────────────────────────────────────
// The IOAPIC exposes two 32-bit registers:
//   IOREGSEL (offset 0x00): index register — write the register number here
//   IOWIN    (offset 0x10): data window — read/write the selected register
//
// Indirect registers:
//   0x00 IOAPICID  — APIC ID
//   0x01 IOAPICVER — version + max redirection entry (bits [23:16])
//   0x02 IOAPICARB — arbitration ID
//   0x10 + 2*n     — redirection entry n low  dword
//   0x11 + 2*n     — redirection entry n high dword

#define IOAPIC_REGSEL  0x00u
#define IOAPIC_WIN     0x10u

#define IOAPIC_REG_ID  0x00u
#define IOAPIC_REG_VER 0x01u
#define IOAPIC_REG_RDT_LO(n) (0x10u + 2u * (n))
#define IOAPIC_REG_RDT_HI(n) (0x11u + 2u * (n))

// Redirection entry bits (low dword)
#define RDT_MASKED        (1u << 16)
#define RDT_TRIGGER_LEVEL (1u << 15)   // 0 = edge, 1 = level
#define RDT_POLARITY_LOW  (1u << 13)   // 0 = active high, 1 = active low
#define RDT_DELIV_FIXED   (0u << 8)    // delivery mode 000 = fixed
// High dword bits [31:24]: destination LAPIC ID (physical mode)
#define RDT_DEST(lapic_id) ((uint32_t)(lapic_id) << 24)

// MPS INTI flags (from ACPI override .flags field)
#define INTI_POLARITY_MASK    0x3u
#define INTI_POLARITY_LOW     0x3u   // active low
#define INTI_TRIGGER_MASK     0xCu
#define INTI_TRIGGER_LEVEL    0xCu   // level-triggered

// ── Module state ──────────────────────────────────────────────────────────

static volatile uint32_t* s_base     = NULL;   // kernel virtual MMIO base
static uint32_t           s_gsi_base = 0;      // first GSI this IOAPIC handles
static uint8_t            s_max_rdt  = 0;      // number of redirection entries

// Saved ACPI overrides for ioapic_isa_to_gsi().
static acpi_override_t s_overrides[ACPI_MAX_OVERRIDES];
static uint8_t         s_override_count = 0;

// ── IOAPIC register accessors ─────────────────────────────────────────────

static uint32_t ioapic_read(uint8_t reg) {
    s_base[IOAPIC_REGSEL >> 2] = reg;
    return s_base[IOAPIC_WIN >> 2];
}

static void ioapic_write(uint8_t reg, uint32_t val) {
    s_base[IOAPIC_REGSEL >> 2] = reg;
    s_base[IOAPIC_WIN >> 2]    = val;
}

// ── Redirection table helpers ─────────────────────────────────────────────

// Write a complete 64-bit redirection entry for GSI `gsi`.
// `vector`  — IDT vector to deliver (0x20–0xFF)
// `flags`   — ACPI INTI flags (0 = default: edge, active-high)
// `masked`  — 1 to leave the entry masked initially
static void rdt_write(uint32_t gsi, uint8_t vector, uint16_t flags, int masked) {
    uint32_t n = gsi - s_gsi_base;
    if (n >= s_max_rdt) return;

    uint32_t lo = (uint32_t)vector | RDT_DELIV_FIXED;

    // Apply polarity from ACPI flags (default: active high = 0).
    if ((flags & INTI_POLARITY_MASK) == INTI_POLARITY_LOW)
        lo |= RDT_POLARITY_LOW;

    // Apply trigger mode from ACPI flags (default: edge = 0).
    if ((flags & INTI_TRIGGER_MASK) == INTI_TRIGGER_LEVEL)
        lo |= RDT_TRIGGER_LEVEL;

    if (masked) lo |= RDT_MASKED;

    // Destination: BSP LAPIC ID 0 (physical destination mode, bit 11 = 0).
    uint32_t hi = RDT_DEST(0);

    // Write high dword first so the entry is never half-programmed.
    ioapic_write((uint8_t)IOAPIC_REG_RDT_HI(n), hi);
    ioapic_write((uint8_t)IOAPIC_REG_RDT_LO(n), lo);
}

// ── Public API ────────────────────────────────────────────────────────────

uint32_t ioapic_isa_to_gsi(uint8_t isa_irq) {
    for (uint8_t i = 0; i < s_override_count; i++) {
        if (s_overrides[i].isa_irq == isa_irq)
            return s_overrides[i].gsi;
    }
    return (uint32_t)isa_irq;  // 1:1 mapping (the common case)
}

// Get the ACPI flags for a given ISA IRQ (for polarity/trigger correction).
static uint16_t isa_irq_flags(uint8_t isa_irq) {
    for (uint8_t i = 0; i < s_override_count; i++) {
        if (s_overrides[i].isa_irq == isa_irq)
            return s_overrides[i].flags;
    }
    return 0;  // default: edge, active-high
}

void ioapic_mask(uint32_t gsi) {
    uint32_t n = gsi - s_gsi_base;
    if (n >= s_max_rdt) return;
    uint32_t lo = ioapic_read((uint8_t)IOAPIC_REG_RDT_LO(n));
    ioapic_write((uint8_t)IOAPIC_REG_RDT_LO(n), lo | RDT_MASKED);
}

void ioapic_unmask(uint32_t gsi) {
    uint32_t n = gsi - s_gsi_base;
    if (n >= s_max_rdt) return;
    uint32_t lo = ioapic_read((uint8_t)IOAPIC_REG_RDT_LO(n));
    ioapic_write((uint8_t)IOAPIC_REG_RDT_LO(n), lo & ~RDT_MASKED);
}

void ioapic_init(const acpi_info_t* info) {
    // Map IOAPIC MMIO (only 32 bytes needed but map a full page).
    s_base     = (volatile uint32_t*)vmm_map_mmio(
                     (phys_addr_t)info->ioapic_phys, 0x1000u);
    s_gsi_base = info->ioapic_gsi_base;

    // Save overrides for ioapic_isa_to_gsi().
    s_override_count = info->override_count;
    for (uint8_t i = 0; i < info->override_count; i++)
        s_overrides[i] = info->overrides[i];

    // Read max redirection entry count from IOAPICVER bits [23:16].
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    s_max_rdt    = (uint8_t)(((ver >> 16) & 0xFFu) + 1u);

    // Start with all entries masked.
    for (uint8_t n = 0; n < s_max_rdt; n++) {
        ioapic_write((uint8_t)IOAPIC_REG_RDT_LO(n), RDT_MASKED);
        ioapic_write((uint8_t)IOAPIC_REG_RDT_HI(n), 0);
    }

    // ── Route ISA IRQs we actually use ───────────────────────────────────
    // IRQ 0 (PIT): route to VEC_LAPIC_TIMER, leave masked until timer_init()
    //   calls ioapic_unmask() after programming the PIT divisor.
    {
        uint32_t gsi = ioapic_isa_to_gsi(0);
        rdt_write(gsi, VEC_LAPIC_TIMER, isa_irq_flags(0), /*masked=*/1);
    }

    // IRQ 1 (PS/2 keyboard): route to vector 0x21, leave MASKED.
    // keyboard_init() installs the real handler, drains the KBC, then
    // calls ioapic_unmask() when the driver is ready to receive events.
    {
        uint32_t gsi   = ioapic_isa_to_gsi(1);
        uint16_t flags = isa_irq_flags(1);
        rdt_write(gsi, 0x21u, flags, /*masked=*/1);
    }

    // IRQ 12 (PS/2 mouse): same pattern — routed but masked until mouse_init().
    {
        uint32_t gsi   = ioapic_isa_to_gsi(12);
        uint16_t flags = isa_irq_flags(12);
        rdt_write(gsi, 0x2Cu, flags, /*masked=*/1);
    }

    // IRQ 8 (RTC): leave masked — we don't use it.
    // All other ISA IRQs: already masked by the loop above.
}
