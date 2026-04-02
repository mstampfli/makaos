#include "pci.h"
#include "common.h"

// ── Config space access ───────────────────────────────────────────────────
// Address register: bit31=enable, bits23:16=bus, bits15:11=dev,
//                   bits10:8=fn, bits7:2=register, bits1:0=0

static uint32_t make_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return (1u << 31)
         | ((uint32_t)bus << 16)
         | ((uint32_t)(dev & 0x1F) << 11)
         | ((uint32_t)(fn  & 0x07) <<  8)
         | (off & 0xFC);
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(PCI_ADDR_PORT, make_addr(bus, dev, fn, off));
    return inl(PCI_DATA_PORT);
}

void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    outl(PCI_ADDR_PORT, make_addr(bus, dev, fn, off));
    outl(PCI_DATA_PORT, val);
}

// ── Scan ──────────────────────────────────────────────────────────────────

uint8_t pci_find(uint8_t class_code, uint8_t subclass, pci_device_t* out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_cfg_read32((uint8_t)bus, dev, fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;  // no device

                uint32_t cls = pci_cfg_read32((uint8_t)bus, dev, fn, 0x08);
                uint8_t cc  = (cls >> 24) & 0xFF;
                uint8_t sc  = (cls >> 16) & 0xFF;
                uint8_t pi  = (cls >>  8) & 0xFF;

                if (cc == class_code && sc == subclass) {
                    out->bus       = (uint8_t)bus;
                    out->dev       = dev;
                    out->fn        = fn;
                    out->vendor_id = (uint16_t)(id & 0xFFFF);
                    out->device_id = (uint16_t)((id >> 16) & 0xFFFF);
                    out->class_code = cc;
                    out->subclass   = sc;
                    out->prog_if    = pi;
                    return 1;
                }

                // Only function 0 unless the header indicates multi-function.
                if (fn == 0) {
                    uint32_t hdr = pci_cfg_read32((uint8_t)bus, dev, fn, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;
                }
            }
        }
    }
    return 0;
}

// ── BAR ───────────────────────────────────────────────────────────────────

uint64_t pci_bar_base(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t bar) {
    uint8_t  off = 0x10 + bar * 4;
    uint32_t lo  = pci_cfg_read32(bus, dev, fn, off);

    if (lo & 1) {
        // I/O BAR — return I/O base address
        return lo & ~0x3u;
    }

    // Memory BAR
    uint8_t type = (lo >> 1) & 0x3;
    if (type == 2) {
        // 64-bit BAR: next register holds upper 32 bits
        uint32_t hi = pci_cfg_read32(bus, dev, fn, (uint8_t)(off + 4));
        return ((uint64_t)hi << 32) | (lo & ~0xFu);
    }
    return lo & ~0xFu;
}

// ── Enable ────────────────────────────────────────────────────────────────

void pci_enable(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t cmd = pci_cfg_read32(bus, dev, fn, 0x04);
    cmd |= (1u << 1) | (1u << 2);  // bit1 = MMIO enable, bit2 = bus master
    pci_cfg_write32(bus, dev, fn, 0x04, cmd);
}
