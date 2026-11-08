#pragma once
#include "common.h"
#include "skbuff.h"

// ── virtio-net NIC driver ─────────────────────────────────────────────────
// Implements the virtio 1.1 specification for the network device (device ID
// 0x1041, transitional vendor 0x1AF4 / device 0x1000).
//
// We use the modern (non-legacy) virtio-PCI transport:
//   • PCI vendor 0x1AF4 (Red Hat / QEMU virtio)
//   • Device IDs: 0x1041 (modern net) or 0x1000 (transitional)
//   • Capabilities: virtio-pci-cap structures in PCI config space (type 9)
//
// Three virtqueues:
//   vq 0 — receiveq   (device → driver: incoming packets)
//   vq 1 — transmitq  (driver → device: outgoing packets)
//   vq 2 — controlq   (optional control commands; we don't use it)
//
// MSI-X: one vector per virtqueue + one for config changes.
// We request two vectors: vq0 (RX) and vq1 (TX) share VEC_VIRTIO_NET.

// MAC address length
#define ETH_ALEN  6u

// Maximum Ethernet frame size (without 802.1Q VLAN tag)
#define ETH_MAX_FRAME  1514u
// Maximum packet size we hand to the network stack (includes virtio header)
#define VIRTIO_NET_MAX_PKT  (ETH_MAX_FRAME + 12u)

// Number of descriptors per virtqueue ring (must be power of 2).
// Larger rings allow more in-flight packets; 256 is the standard value.
#define VIRTQ_SIZE  256u

// Number of RX packet buffers we actually allocate and post (one per RX
// descriptor we hand the device).  SINGLE SOURCE OF TRUTH: the alloc loop, the
// refill loop, AND the rx-completion id bound all use this, so the validation
// can never disagree with how many s_rx_bufs[] entries are populated.  A device
// reporting an RX completion id >= this (an id we never posted) would index an
// uninitialized buffer (NULL source / phys 0); we reject it.
#define VIRTQ_NUM_RX_BUFS  (VIRTQ_SIZE / 2u)

// Initialise the virtio-net device.
// Returns 1 on success, 0 if no device found or init failed.
int virtio_net_init(void);

// Transmit one Ethernet frame.
// `data` points to a fully-formed Ethernet frame (dst MAC → payload).
// `len`  is the frame length in bytes (≤ ETH_MAX_FRAME).
// Blocks until the transmit descriptor slot is available.
// Returns 0 on success, -1 on error.
int virtio_net_tx(const void* data, uint16_t len);

// Poll for a received packet.  Non-blocking.
// Fills *skb_out with a newly-allocated skbuff on success.
// Returns 1 if a packet was received, 0 if the RX ring is empty.
int virtio_net_rx_poll(skbuff_t** skb_out);

// Return the device MAC address (6 bytes, big-endian).
const uint8_t* virtio_net_mac(void);

// IRQ handler — called from irq_stubs.asm after LAPIC EOI.
void virtio_net_irq_handler(void);

// IRQ slot index used with irq_wait/irq_notify.
extern uint8_t g_virtio_net_irq;
