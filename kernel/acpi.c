#include "acpi.h"
#include "common.h"

// ── ACPI structure definitions ────────────────────────────────────────────
// All multi-byte fields are little-endian (x86 native).

typedef struct __attribute__((packed)) {
    char     signature[8];   // "RSD PTR "
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;       // 0 = ACPI 1.0 (RSDT), ≥2 = ACPI 2.0+ (XSDT)
    uint32_t rsdt_phys;
    // ACPI 2.0+ extension (only valid when revision ≥ 2):
    uint32_t length;
    uint64_t xsdt_phys;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} rsdp_t;

typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} sdt_header_t;

// MADT (Multiple APIC Description Table) header.
typedef struct __attribute__((packed)) {
    sdt_header_t hdr;
    uint32_t     lapic_phys;   // default LAPIC base (may be overridden by type 5)
    uint32_t     flags;        // bit 0: 8259 PICs installed
} madt_t;

// MADT entry header (common to all entry types).
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} madt_entry_hdr_t;

// MADT type 1: I/O APIC
typedef struct __attribute__((packed)) {
    madt_entry_hdr_t hdr;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_phys;
    uint32_t gsi_base;
} madt_ioapic_t;

// MADT type 2: Interrupt Source Override
typedef struct __attribute__((packed)) {
    madt_entry_hdr_t hdr;
    uint8_t  bus;       // always 0 = ISA
    uint8_t  source;    // ISA IRQ
    uint32_t gsi;       // mapped GSI
    uint16_t flags;
} madt_override_t;

// MADT type 5: 64-bit LAPIC address override
typedef struct __attribute__((packed)) {
    madt_entry_hdr_t hdr;
    uint16_t reserved;
    uint64_t lapic_phys;
} madt_lapic64_t;

// ── Checksum helper ───────────────────────────────────────────────────────

static int acpi_checksum_ok(const void* ptr, uint32_t len) {
    const uint8_t* p = (const uint8_t*)ptr;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += p[i];
    return sum == 0;
}

// ── MADT parser ───────────────────────────────────────────────────────────

static void parse_madt(const madt_t* madt, acpi_info_t* out) {
    // Default LAPIC base from MADT header (may be overridden by type 5).
    out->lapic_phys = madt->lapic_phys;

    uint32_t offset = sizeof(madt_t);
    uint32_t total  = madt->hdr.length;

    while (offset + sizeof(madt_entry_hdr_t) <= total) {
        const madt_entry_hdr_t* e =
            (const madt_entry_hdr_t*)((const uint8_t*)madt + offset);

        if (e->length < sizeof(madt_entry_hdr_t) ||
            offset + e->length > total) break;

        switch (e->type) {
        case 1: { // I/O APIC
            const madt_ioapic_t* io = (const madt_ioapic_t*)e;
            // Use the first IOAPIC found (systems with one IOAPIC cover GSI 0–23).
            if (!out->ioapic_phys) {
                out->ioapic_phys    = io->ioapic_phys;
                out->ioapic_gsi_base = io->gsi_base;
            }
            break;
        }
        case 2: { // Interrupt Source Override
            const madt_override_t* ov = (const madt_override_t*)e;
            if (out->override_count < ACPI_MAX_OVERRIDES) {
                acpi_override_t* o = &out->overrides[out->override_count++];
                o->bus     = ov->bus;
                o->isa_irq = ov->source;
                o->gsi     = ov->gsi;
                o->flags   = ov->flags;
            }
            break;
        }
        case 5: { // 64-bit LAPIC address override
            const madt_lapic64_t* la = (const madt_lapic64_t*)e;
            out->lapic_phys = la->lapic_phys;
            break;
        }
        default:
            break;
        }

        offset += e->length;
    }
}

// ── SDT table scanner ─────────────────────────────────────────────────────
// Walk either an RSDT (32-bit pointers) or XSDT (64-bit pointers) and
// find the MADT ("APIC" signature).

static const sdt_header_t* find_table(const sdt_header_t* root,
                                       int use_xsdt,
                                       const char sig[4]) {
    uint32_t entry_size  = use_xsdt ? 8u : 4u;
    uint32_t n_entries   = (root->length - sizeof(sdt_header_t)) / entry_size;
    const uint8_t* ptr   = (const uint8_t*)root + sizeof(sdt_header_t);

    for (uint32_t i = 0; i < n_entries; i++) {
        uint64_t phys;
        if (use_xsdt) {
            // Unaligned 64-bit read — copy byte by byte to avoid UB.
            uint8_t tmp[8];
            for (int b = 0; b < 8; b++) tmp[b] = ptr[i * 8 + b];
            phys = ((uint64_t)tmp[7] << 56) | ((uint64_t)tmp[6] << 48) |
                   ((uint64_t)tmp[5] << 40) | ((uint64_t)tmp[4] << 32) |
                   ((uint64_t)tmp[3] << 24) | ((uint64_t)tmp[2] << 16) |
                   ((uint64_t)tmp[1] <<  8) |  (uint64_t)tmp[0];
        } else {
            uint8_t tmp[4];
            for (int b = 0; b < 4; b++) tmp[b] = ptr[i * 4 + b];
            phys = ((uint32_t)tmp[3] << 24) | ((uint32_t)tmp[2] << 16) |
                   ((uint32_t)tmp[1] <<  8) |  (uint32_t)tmp[0];
        }

        const sdt_header_t* hdr =
            (const sdt_header_t*)(uintptr_t)(phys + HHDM_OFFSET);

        if (hdr->signature[0] == sig[0] && hdr->signature[1] == sig[1] &&
            hdr->signature[2] == sig[2] && hdr->signature[3] == sig[3]) {
            return hdr;
        }
    }
    return NULL;
}

// ── RSDP search in legacy BIOS area ──────────────────────────────────────

static const rsdp_t* find_rsdp_bios(void) {
    // The RSDP is 16-byte aligned within 0xE0000–0xFFFFF.
    for (uint64_t phys = 0xE0000; phys < 0x100000; phys += 16) {
        const rsdp_t* r = (const rsdp_t*)(uintptr_t)(phys + HHDM_OFFSET);
        if (r->signature[0] == 'R' && r->signature[1] == 'S' &&
            r->signature[2] == 'D' && r->signature[3] == ' ' &&
            r->signature[4] == 'P' && r->signature[5] == 'T' &&
            r->signature[6] == 'R' && r->signature[7] == ' ') {
            if (acpi_checksum_ok(r, 20)) return r;
        }
    }
    return NULL;
}

// ── Public API ────────────────────────────────────────────────────────────

acpi_info_t acpi_parse(uint64_t rsdp_phys) {
    acpi_info_t out = {0};

    const rsdp_t* rsdp = NULL;

    if (rsdp_phys) {
        rsdp = (const rsdp_t*)(uintptr_t)(rsdp_phys + HHDM_OFFSET);
        // Basic checksum on the first 20 bytes (ACPI 1.0 portion).
        if (!acpi_checksum_ok(rsdp, 20)) rsdp = NULL;
    }

    if (!rsdp) rsdp = find_rsdp_bios();
    if (!rsdp) return out;  // out.ok == 0

    // Prefer XSDT (64-bit) over RSDT (32-bit) when available.
    int use_xsdt = (rsdp->revision >= 2) && (rsdp->xsdt_phys != 0);

    uint64_t root_phys = use_xsdt ? rsdp->xsdt_phys : (uint64_t)rsdp->rsdt_phys;
    const sdt_header_t* root =
        (const sdt_header_t*)(uintptr_t)(root_phys + HHDM_OFFSET);

    // Find MADT ("APIC").
    const sdt_header_t* madt_hdr = find_table(root, use_xsdt, "APIC");
    if (!madt_hdr) return out;

    parse_madt((const madt_t*)madt_hdr, &out);

    // Sanity: we need at least an IOAPIC address.
    if (!out.ioapic_phys) return out;

    // Fall back to default LAPIC address if MADT header had 0.
    if (!out.lapic_phys) out.lapic_phys = 0xFEE00000ULL;

    out.ok = 1;
    return out;
}
