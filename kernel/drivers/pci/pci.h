#pragma once
#include "common.h"

// ── PCI configuration space (port I/O mechanism 1) ────────────────────────
#define PCI_ADDR_PORT  0x0CF8u
#define PCI_DATA_PORT  0x0CFCu

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
} pci_device_t;

// Read/write a 32-bit DWORD from PCI config space.
uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val);

// Walk the PCI capability list for the first capability whose id == `cap_id`
// (e.g. 0x11 = MSI-X, 0x05 = MSI); returns its config-space offset, or 0 if
// absent.  This is the standard cap-list walk that ahci/nvme/virtio each
// hand-rolled identically.  Bounded to 48 hops so a malformed (cyclic) cap
// list can never spin the kernel.
static inline uint8_t pci_find_cap(uint8_t bus, uint8_t dev, uint8_t fn,
                                   uint8_t cap_id) {
    uint8_t cap = (uint8_t)(pci_cfg_read32(bus, dev, fn, 0x34u) & 0xFCu);
    for (int hops = 0; cap && hops < 48; hops++) {
        uint32_t dw = pci_cfg_read32(bus, dev, fn, cap);
        if ((uint8_t)(dw & 0xFFu) == cap_id) return cap;
        cap = (uint8_t)((dw >> 8) & 0xFCu);
    }
    return 0;
}

// Scan all buses/devices for the first device matching class:subclass.
// Returns 1 and fills *out on success; returns 0 if not found.
uint8_t pci_find(uint8_t class_code, uint8_t subclass, pci_device_t* out);

// Return the base address from BARn (0–5), with type bits cleared.
// Handles both 32-bit and 64-bit BARs.
uint64_t pci_bar_base(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t bar);

// Enable MMIO decoding and bus mastering for the device.
void pci_enable(uint8_t bus, uint8_t dev, uint8_t fn);
