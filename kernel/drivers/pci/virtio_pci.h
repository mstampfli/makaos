#pragma once
#include "common.h"   // uint*_t
#include "pci.h"      // pci_cfg_read32

// ── Shared virtio-PCI (1.x modern) transport definitions ────────────────────
//
// The generic, spec-defined pieces of the virtio-over-PCI transport that every
// virtio driver (net / gpu / input / ...) needs identically: the vendor id, the
// capability cfg_type values, the device-status bits, the VERSION_1 feature bit,
// the common-configuration MMIO layout, and the PCI capability walker.  These
// were copy-pasted per driver (F155 fixed the SAME find_virtio_cap NULL-deref in
// three places); this is the single source of truth so they cannot drift.
//
// Device-SPECIFIC constants (device ids, per-device feature bits, command
// opcodes, config-space layouts, queue indices) stay in each driver.

#define VIRTIO_VENDOR              0x1AF4u

// virtio_pci_cap.cfg_type values (spec 4.1.4).
#define VIRTIO_PCI_CAP_COMMON_CFG  1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2u
#define VIRTIO_PCI_CAP_ISR_CFG     3u
#define VIRTIO_PCI_CAP_DEVICE_CFG  4u
#define VIRTIO_PCI_CAP_PCI_CFG     5u

// device status bits (spec 2.1).
#define VIRTIO_STATUS_ACKNOWLEDGE   1u
#define VIRTIO_STATUS_DRIVER        2u
#define VIRTIO_STATUS_DRIVER_OK     4u
#define VIRTIO_STATUS_FEATURES_OK   8u
#define VIRTIO_STATUS_FAILED      128u

// transport feature bit: modern (non-legacy) device.
#define VIRTIO_F_VERSION_1          (1ULL << 32)

// ── virtio-PCI common configuration structure (spec 4.1.4.3) ────────────────
typedef struct __attribute__((packed)) {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    // Per-queue fields (select queue first via queue_select):
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint32_t queue_desc_lo;
    uint32_t queue_desc_hi;
    uint32_t queue_driver_lo;   // avail ring
    uint32_t queue_driver_hi;
    uint32_t queue_device_lo;   // used ring
    uint32_t queue_device_hi;
} virtio_pci_common_cfg_t;

// ── PCI capability descriptor + walker ──────────────────────────────────────

typedef struct {
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
    uint32_t extra;   // notify_off_multiplier for VIRTIO_PCI_CAP_NOTIFY_CFG
} vcap_t;

// Walk the PCI capability list for the vendor-specific (id 0x09) virtio cap
// whose cfg_type == cap_type; fill *out and return 1, else return 0.
static inline int find_virtio_cap(uint8_t bus, uint8_t dev, uint8_t fn,
                                  uint8_t cap_type, vcap_t* out) {
    uint8_t cap = (uint8_t)(pci_cfg_read32(bus, dev, fn, 0x34u) & 0xFCu);
    while (cap) {
        uint32_t dw0  = pci_cfg_read32(bus, dev, fn, cap);
        uint8_t  id   = (uint8_t)(dw0 & 0xFFu);
        uint8_t  next = (uint8_t)((dw0 >> 8) & 0xFCu);
        uint8_t  len  = (uint8_t)((dw0 >> 16) & 0xFFu);

        if (id == 0x09u && len >= 16u) {  // vendor-specific PCI capability
            // virtio_pci_cap layout at `cap`:
            //   +0: cap_vndr, cap_next, cap_len, cfg_type
            //   +4: bar   +8: offset (32-bit)   +12: length (32-bit)
            uint32_t dw1 = pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 4u));
            uint32_t dw2 = pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 8u));
            uint32_t dw3 = pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 12u));

            uint8_t cfg_type = (uint8_t)((dw0 >> 24) & 0xFFu);
            if (cfg_type == cap_type) {
                out->bar    = (uint8_t)(dw1 & 0xFFu);
                out->offset = dw2;
                out->length = dw3;
                // notify_off_multiplier is an extra dword after cap+12.
                out->extra  = (cap_type == VIRTIO_PCI_CAP_NOTIFY_CFG)
                              ? pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 16u))
                              : 0;
                return 1;
            }
        }
        cap = next;
    }
    return 0;
}
