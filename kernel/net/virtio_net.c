#include "virtio_net.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "smp.h"
#include "idt.h"
#include "lapic.h"
#include "irq_wait.h"
#include "common.h"
#include "checked.h"   // index_ok: single source of truth for bounded indices

extern void virtio_net_irq_entry(void);

// ── PCI IDs ───────────────────────────────────────────────────────────────
#define VIRTIO_VENDOR          0x1AF4u
#define VIRTIO_DEV_NET_MODERN  0x1041u  // virtio 1.x network device
#define VIRTIO_DEV_NET_LEGACY  0x1000u  // transitional (also works)
#define PCI_CLASS_NETWORK      0x02u
#define PCI_SUBCLASS_ETHERNET  0x00u

// ── virtio-PCI capability types ───────────────────────────────────────────
#define VIRTIO_PCI_CAP_COMMON_CFG  1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2u
#define VIRTIO_PCI_CAP_ISR_CFG     3u
#define VIRTIO_PCI_CAP_DEVICE_CFG  4u
#define VIRTIO_PCI_CAP_PCI_CFG     5u

// ── virtio device status bits ─────────────────────────────────────────────
#define VIRTIO_STATUS_ACKNOWLEDGE   1u
#define VIRTIO_STATUS_DRIVER        2u
#define VIRTIO_STATUS_DRIVER_OK     4u
#define VIRTIO_STATUS_FEATURES_OK   8u
#define VIRTIO_STATUS_FAILED       128u

// ── virtio feature bits ───────────────────────────────────────────────────
// We negotiate a minimal set — only what we need.
#define VIRTIO_F_VERSION_1          (1ULL << 32)  // modern device
#define VIRTIO_NET_F_MAC            (1ULL << 5)   // device has a MAC address
#define VIRTIO_NET_F_STATUS         (1ULL << 16)  // device exposes link status

#define DRIVER_FEATURES  (VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC)

// ── virtio-PCI common configuration structure (spec §4.1.4.3) ────────────
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

// ── virtio-net device configuration (spec §5.1.4) ────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
} virtio_net_cfg_t;

// ── Virtqueue descriptor (spec §2.6.5) ───────────────────────────────────
// Each descriptor points to one buffer.
typedef struct __attribute__((packed)) {
    uint64_t addr;    // physical address of buffer
    uint32_t len;     // buffer length
    uint16_t flags;   // VIRTQ_DESC_F_*
    uint16_t next;    // index of next descriptor (if NEXT flag set)
} virtq_desc_t;

#define VIRTQ_DESC_F_NEXT      1u   // descriptor continues via next field
#define VIRTQ_DESC_F_WRITE     2u   // buffer is device-writable (RX)
#define VIRTQ_DESC_F_INDIRECT  4u   // buffer contains a descriptor table

// ── Virtqueue available ring (spec §2.6.6) ────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t flags;           // 0 = notify device on update
    uint16_t idx;             // next slot to write
    uint16_t ring[VIRTQ_SIZE];
    uint16_t used_event;
} virtq_avail_t;

// ── Virtqueue used ring (spec §2.6.8) ────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;             // next slot device will write
    struct {
        uint32_t id;          // descriptor chain head index
        uint32_t len;         // bytes written into the buffer
    } ring[VIRTQ_SIZE];
    uint16_t avail_event;
} virtq_used_t;

// ── virtio-net header (prepended to every packet, spec §5.1.6) ───────────
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;    // VIRTIO_NET_HDR_GSO_NONE = 0
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers; // only present when VIRTIO_NET_F_MRG_RXBUF negotiated
} virtio_net_hdr_t;

// Header length.  Per virtio 1.0 §5.1.6.1: when VIRTIO_F_VERSION_1 is
// negotiated (which we do in DRIVER_FEATURES) the `num_buffers` field
// is ALWAYS present — set to 1 by the device if MRG_RXBUF isn't
// negotiated, but still there as part of the header — so the header
// is 12 bytes even without MRG_RXBUF.  Only pre-1.0 (legacy) devices
// omit `num_buffers` when MRG_RXBUF isn't negotiated.
//
// Using the wrong length silently shifts every Ethernet frame by 2
// bytes at the wire: QEMU strips 12 bytes of header, leaving the
// first 2 bytes of our Ethernet dst MAC in the tail of the header.
// Outgoing frames arrive at the NAT with a mangled dst MAC → silently
// dropped.  Symptom: DHCP DISCOVER retransmits forever, no OFFER
// comes back.
#define VIRTIO_NET_HDR_LEN  12u

// ── Per-virtqueue state ───────────────────────────────────────────────────
typedef struct {
    // Descriptor table, avail ring, used ring — each must be physically
    // contiguous and naturally aligned.  We allocate each separately from
    // the buddy allocator.
    virtq_desc_t* desc;      // virtual (HHDM)
    virtq_avail_t* avail;
    virtq_used_t*  used;
    phys_addr_t    desc_phys;
    phys_addr_t    avail_phys;
    phys_addr_t    used_phys;

    uint16_t  free_head;     // first free descriptor index
    uint16_t  avail_idx;     // next slot to write in avail->ring[]
    uint16_t  last_used_idx; // last used->idx we processed

    uint16_t  notify_off;    // from queue_notify_off (multiply by notify_off_mult)
} virtq_t;

// ── Packet buffers for RX ─────────────────────────────────────────────────
// Each RX descriptor gets its own pre-allocated buffer.  We allocate
// VIRTQ_NUM_RX_BUFS buffers of ETH_MAX_FRAME + VIRTIO_NET_HDR_LEN bytes each
// (the array is sized VIRTQ_SIZE but only the first VIRTQ_NUM_RX_BUFS slots are
// populated).  Stored as physical addresses (for the device) + virtual (CPU).
typedef struct {
    phys_addr_t phys;
    uint8_t*    virt;
} rx_buf_t;

// ── Driver state ──────────────────────────────────────────────────────────

uint8_t g_virtio_net_irq = 0xFFu;

static int s_ok = 0;

static pci_device_t         s_dev;
static volatile virtio_pci_common_cfg_t* s_common = NULL;
static volatile uint8_t*                s_notify  = NULL;
static volatile virtio_net_cfg_t*       s_devcfg  = NULL;
static uint32_t                         s_notify_mult = 0;

static virtq_t   s_rxq;
static virtq_t   s_txq;
static rx_buf_t  s_rx_bufs[VIRTQ_SIZE];

// TX: one pre-allocated buffer for the virtio header + packet data.
// We serialize TX (one at a time) which is fine for a single-core kernel.
static uint8_t*    s_tx_buf_virt = NULL;
static phys_addr_t s_tx_buf_phys = 0;

static uint8_t s_mac[ETH_ALEN];

// ── PCI capability walker ─────────────────────────────────────────────────

typedef struct {
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
    uint32_t extra;   // notify_off_multiplier for type 2
} vcap_t;

static int find_virtio_cap(uint8_t bus, uint8_t dev, uint8_t fn,
                            uint8_t cap_type, vcap_t* out) {
    // Walk PCI capability list looking for vendor-specific (type 9) caps
    // that have virtio_pci_cap.cfg_type == cap_type.
    uint8_t cap = (uint8_t)(pci_cfg_read32(bus, dev, fn, 0x34u) & 0xFCu);
    while (cap) {
        uint32_t dw0 = pci_cfg_read32(bus, dev, fn, cap);
        uint8_t  id  = (uint8_t)(dw0 & 0xFFu);
        uint8_t  next = (uint8_t)((dw0 >> 8) & 0xFCu);
        uint8_t  len  = (uint8_t)((dw0 >> 16) & 0xFFu);

        if (id == 0x09u && len >= 16u) {  // vendor-specific PCI capability
            // virtio_pci_cap layout at `cap`:
            // +0: cap_vndr, cap_next, cap_len, cfg_type
            // +4: bar
            // +8: offset (32-bit)
            // +12: length (32-bit)
            uint32_t dw1 = pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 4u));
            uint32_t dw2 = pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 8u));
            uint32_t dw3 = pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 12u));

            uint8_t cfg_type = (uint8_t)((dw0 >> 24) & 0xFFu);
            uint8_t bar_idx  = (uint8_t)(dw1 & 0xFFu);

            if (cfg_type == cap_type) {
                out->bar    = bar_idx;
                out->offset = dw2;
                out->length = dw3;
                if (cap_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    // notify_off_multiplier is an extra dword after cap+12
                    out->extra = pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 16u));
                } else {
                    out->extra = 0;
                }
                return 1;
            }
        }
        cap = next;
    }
    return 0;
}

// ── Virtqueue helpers ─────────────────────────────────────────────────────

// Allocate and zero the three rings for one virtqueue.
// desc: VIRTQ_SIZE × 16 bytes = 4096 bytes → order 0
// avail: 6 + 2×VIRTQ_SIZE bytes = 518 bytes → order 0
// used:  6 + 8×VIRTQ_SIZE bytes = 2054 bytes → order 1
static int virtq_alloc(virtq_t* vq) {
    // Descriptor table: 16 bytes × 256 = 4096 bytes, needs 16-byte alignment.
    vq->desc_phys = pmm_buddy_alloc(0);
    if (!vq->desc_phys) return 0;
    vq->desc = (virtq_desc_t*)((uintptr_t)vq->desc_phys + HHDM_OFFSET);
    __builtin_memset(vq->desc, 0, VIRTQ_SIZE * sizeof(virtq_desc_t));

    // Available ring: 4 + 2 + 2×256 = 518 bytes, 2-byte alignment → 1 page.
    vq->avail_phys = pmm_buddy_alloc(0);
    if (!vq->avail_phys) return 0;
    vq->avail = (virtq_avail_t*)((uintptr_t)vq->avail_phys + HHDM_OFFSET);
    __builtin_memset(vq->avail, 0, sizeof(virtq_avail_t));

    // Used ring: 4 + 2 + 8×256 = 2054 bytes, 4-byte alignment → 1 page.
    vq->used_phys = pmm_buddy_alloc(0);
    if (!vq->used_phys) return 0;
    vq->used = (virtq_used_t*)((uintptr_t)vq->used_phys + HHDM_OFFSET);
    __builtin_memset(vq->used, 0, sizeof(virtq_used_t));

    // Build free-descriptor chain.
    for (uint16_t i = 0; i < VIRTQ_SIZE - 1; i++)
        vq->desc[i].next = (uint16_t)(i + 1u);
    vq->desc[VIRTQ_SIZE - 1].next = 0xFFFFu;

    vq->free_head     = 0;
    vq->avail_idx     = 0;
    vq->last_used_idx = 0;
    return 1;
}

// Tell the device about a virtqueue (select it then write ring addresses).
static void virtq_activate(volatile virtio_pci_common_cfg_t* cfg,
                            uint16_t qidx, virtq_t* vq) {
    cfg->queue_select   = qidx;
    __asm__ volatile("mfence" ::: "memory");

    cfg->queue_desc_lo   = (uint32_t)(vq->desc_phys  & 0xFFFFFFFFu);
    cfg->queue_desc_hi   = (uint32_t)(vq->desc_phys  >> 32);
    cfg->queue_driver_lo = (uint32_t)(vq->avail_phys & 0xFFFFFFFFu);
    cfg->queue_driver_hi = (uint32_t)(vq->avail_phys >> 32);
    cfg->queue_device_lo = (uint32_t)(vq->used_phys  & 0xFFFFFFFFu);
    cfg->queue_device_hi = (uint32_t)(vq->used_phys  >> 32);
    cfg->queue_enable    = 1;
    __asm__ volatile("mfence" ::: "memory");
}

// Allocate a descriptor from the free list.
// Returns descriptor index, or 0xFFFF if the ring is full.
static uint16_t virtq_alloc_desc(virtq_t* vq) {
    uint16_t idx = vq->free_head;
    if (idx == 0xFFFFu) return 0xFFFFu;
    vq->free_head = vq->desc[idx].next;
    return idx;
}

// PRIMITIVE (device-supplied index, category D -> delegates to index_ok).
// Is a descriptor id a valid index into the VIRTQ_SIZE-entry desc ring / free
// list?  Device-supplied ids (read from the used ring) MUST pass this before
// being used as an index -- an out-of-range id otherwise drives an OOB write of
// s_rxq.desc[] / the free list.  Used by the TX completion path and virtq_submit,
// which operate over the FULL desc ring.  Pure -> unit-tested.
static int virtio_desc_id_valid(uint32_t id) { return index_ok(id, VIRTQ_SIZE); }

// PRIMITIVE (device-supplied RX completion id, category D).  The RX path is
// TIGHTER than virtio_desc_id_valid: a completion id also indexes s_rx_bufs[],
// which is populated only for VIRTQ_NUM_RX_BUFS entries (those are the only RX
// descriptors we ever post), so an id in [VIRTQ_NUM_RX_BUFS, VIRTQ_SIZE) is one
// the device invented -- its s_rx_bufs[] slot is {NULL, 0}, which would memcpy
// from a NULL source and re-post a descriptor pointing at physical frame 0.
// Pure -> unit-tested (virtio_rx_id_valid_selftest).
static int virtio_rx_id_valid(uint32_t id) { return index_ok(id, VIRTQ_NUM_RX_BUFS); }

// Return a descriptor to the free list.
static void virtq_free_desc(virtq_t* vq, uint16_t idx) {
    if (!virtio_desc_id_valid(idx)) return;   // defense-in-depth: never index OOB
    vq->desc[idx].next = vq->free_head;
    vq->free_head      = idx;
}

// Submit a descriptor chain to the avail ring and kick the device.
static void virtq_submit(virtq_t* vq, uint16_t head_idx, uint16_t qidx) {
    vq->avail->ring[vq->avail_idx & (VIRTQ_SIZE - 1)] = head_idx;
    __asm__ volatile("mfence" ::: "memory");
    vq->avail->idx = (uint16_t)(vq->avail_idx + 1u);
    vq->avail_idx++;
    __asm__ volatile("mfence" ::: "memory");

    // Notify the device by writing the queue index to the notify register.
    // notify address = notify_base + queue_notify_off × notify_off_mult
    volatile uint16_t* notify_reg =
        (volatile uint16_t*)(s_notify + vq->notify_off * s_notify_mult);
    *notify_reg = qidx;
}

// ── RX ring setup ─────────────────────────────────────────────────────────
// Pre-populate the RX ring with VIRTQ_NUM_RX_BUFS device-writable buffers so
// the device can deposit incoming packets immediately.

static void rxq_refill(void) {
    // Each RX descriptor chain: [virtio_net_hdr (10 B)] [frame (1514 B)]
    // We use two descriptors per packet: one for the header, one for data.
    // Actually simpler: one large buffer = VIRTIO_NET_HDR_LEN + ETH_MAX_FRAME.
    // Use single-descriptor approach (simpler, one page per slot).  Posts
    // exactly VIRTQ_NUM_RX_BUFS descriptors (the populated s_rx_bufs[] slots);
    // the sequential free list makes the posted desc ids 0..VIRTQ_NUM_RX_BUFS-1,
    // so a completion id from the device maps back to s_rx_bufs[id].
    for (uint16_t i = 0; i < VIRTQ_NUM_RX_BUFS; i++) {
        uint16_t idx = virtq_alloc_desc(&s_rxq);
        if (idx == 0xFFFFu) break;

        s_rxq.desc[idx].addr  = s_rx_bufs[i].phys;
        s_rxq.desc[idx].len   = VIRTIO_NET_HDR_LEN + ETH_MAX_FRAME;
        s_rxq.desc[idx].flags = VIRTQ_DESC_F_WRITE;  // device writes here
        s_rxq.desc[idx].next  = 0;

        s_rxq.avail->ring[s_rxq.avail_idx & (VIRTQ_SIZE - 1)] = idx;
        s_rxq.avail_idx++;
    }
    __asm__ volatile("mfence" ::: "memory");
    s_rxq.avail->idx = s_rxq.avail_idx;
    __asm__ volatile("mfence" ::: "memory");

    // Kick RX queue (queue index 0).
    volatile uint16_t* notify_reg =
        (volatile uint16_t*)(s_notify + s_rxq.notify_off * s_notify_mult);
    *notify_reg = 0;
}

// ── IRQ handler ───────────────────────────────────────────────────────────

void virtio_net_irq_handler(void) {
    if (!s_ok) return;
    // Notify the driver thread — actual processing happens there.
    irq_notify(g_virtio_net_irq);
}

// ── Public TX API ─────────────────────────────────────────────────────────

// Serializes the entire TX submit+poll window: the descriptor free list,
// the shared TX bounce buffer, the avail ring, and last_used_idx are all
// global and TX can be entered concurrently by any task (eth.c) and the
// net RX thread.  The device's TX side never touches these from an ISR,
// so a plain spin_lock (no irqsave) suffices — contention is cross-CPU.
static spinlock_t s_tx_lock = SPINLOCK_INIT;

int virtio_net_tx(const void* data, uint16_t len) {
    if (!s_ok || len > ETH_MAX_FRAME) return -1;

    spin_lock(&s_tx_lock);
    // Build virtio-net header + frame in the TX buffer.
    __builtin_memset(s_tx_buf_virt, 0, VIRTIO_NET_HDR_LEN);
    __builtin_memcpy(s_tx_buf_virt + VIRTIO_NET_HDR_LEN, data, len);

    __asm__ volatile("mfence" ::: "memory");

    // Allocate one descriptor for the combined header + frame.
    uint16_t idx = virtq_alloc_desc(&s_txq);
    if (idx == 0xFFFFu) { spin_unlock(&s_tx_lock); return -1; }  // TX ring full

    s_txq.desc[idx].addr  = s_tx_buf_phys;
    s_txq.desc[idx].len   = VIRTIO_NET_HDR_LEN + len;
    s_txq.desc[idx].flags = 0;  // device reads this (not WRITE)
    s_txq.desc[idx].next  = 0;

    virtq_submit(&s_txq, idx, 1);  // queue index 1 = transmitq

    // Wait for the device to consume the descriptor (used ring advances).
    // This is a synchronous/polling wait — acceptable since we're single-core.
    // Future: use a TX completion interrupt and a pending queue.
    uint32_t spins = 0;
    while (s_txq.used->idx == s_txq.last_used_idx) {
        __asm__ volatile("pause" ::: "memory");
        if (++spins > 10000000u) { spin_unlock(&s_tx_lock); return -1; }  // timeout
    }
    uint16_t used_idx = (uint16_t)(s_txq.last_used_idx & (VIRTQ_SIZE - 1));
    // Free the descriptor the device reports it completed, but never trust an
    // out-of-range id (it would OOB-write the desc table / corrupt the free
    // list): fall back to `idx`, the single descriptor we actually submitted.
    uint32_t done_id = s_txq.used->ring[used_idx].id;
    virtq_free_desc(&s_txq, virtio_desc_id_valid(done_id) ? (uint16_t)done_id : idx);
    s_txq.last_used_idx++;

    spin_unlock(&s_tx_lock);
    return 0;
}

// ── Public RX API ─────────────────────────────────────────────────────────

int virtio_net_rx_poll(skbuff_t** skb_out) {
    if (!s_ok) return 0;

    __asm__ volatile("lfence" ::: "memory");
    if (s_rxq.used->idx == s_rxq.last_used_idx) return 0;  // nothing received
    serial_puts_dbg("[virtio] rx packet!\n");

    uint16_t used_slot = (uint16_t)(s_rxq.last_used_idx & (VIRTQ_SIZE - 1));
    uint32_t desc_id   = s_rxq.used->ring[used_slot].id;
    uint32_t pkt_len   = s_rxq.used->ring[used_slot].len;
    s_rxq.last_used_idx++;   // advance first so a bad entry can't wedge the ring

    // desc_id is written by the device and indexes s_rx_bufs[] (the memcpy
    // source) and s_rxq.desc[].  Only VIRTQ_NUM_RX_BUFS s_rx_bufs[] entries are
    // populated -- those are the only RX descriptors we posted -- so use the
    // TIGHTER rx bound, not the full-ring virtio_desc_id_valid: an id the device
    // invented in [VIRTQ_NUM_RX_BUFS, VIRTQ_SIZE) has a {NULL, 0} buffer slot,
    // which would memcpy from NULL and re-post a descriptor at physical frame 0.
    // Drop the bogus completion: do not index the tables and do not re-post it.
    if (!virtio_rx_id_valid(desc_id)) return 0;

    // The packet starts after the virtio-net header.
    uint8_t* raw    = s_rx_bufs[desc_id].virt;
    uint32_t eth_len = (pkt_len > VIRTIO_NET_HDR_LEN)
                       ? (pkt_len - VIRTIO_NET_HDR_LEN) : 0;
    // CLAMP against the actual RX buffer payload — pkt_len comes straight
    // from the device-written used ring and is otherwise untrusted.  The
    // source buffer holds at most ETH_MAX_FRAME payload bytes; a device
    // reporting more would make the memcpy over-read past the source page
    // into adjacent kernel memory (info leak / corruption).
    if (eth_len > ETH_MAX_FRAME) eth_len = ETH_MAX_FRAME;

    skbuff_t* skb = skb_alloc(eth_len);
    if (skb && eth_len > 0) {
        uint8_t* dst = (uint8_t*)skb_put(skb, eth_len);
        __builtin_memcpy(dst, raw + VIRTIO_NET_HDR_LEN, eth_len);
    }

    // Return the descriptor to the avail ring so the device can reuse it.
    s_rxq.desc[desc_id].addr  = s_rx_bufs[desc_id].phys;
    s_rxq.desc[desc_id].len   = VIRTIO_NET_HDR_LEN + ETH_MAX_FRAME;
    s_rxq.desc[desc_id].flags = VIRTQ_DESC_F_WRITE;
    s_rxq.desc[desc_id].next  = 0;
    s_rxq.avail->ring[s_rxq.avail_idx & (VIRTQ_SIZE - 1)] = (uint16_t)desc_id;
    s_rxq.avail_idx++;
    __asm__ volatile("mfence" ::: "memory");
    s_rxq.avail->idx = s_rxq.avail_idx;
    __asm__ volatile("mfence" ::: "memory");
    // Notify device that avail ring has new entries (queue 0 = RX).
    volatile uint16_t* notify_reg =
        (volatile uint16_t*)(s_notify + s_rxq.notify_off * s_notify_mult);
    *notify_reg = 0;

    *skb_out = skb;
    return 1;
}

const uint8_t* virtio_net_mac(void) { return s_mac; }

// ── Initialisation ────────────────────────────────────────────────────────

int virtio_net_init(void) {
    // 1. Find virtio-net PCI device.
    //    Try modern device ID first (0x1041), then transitional (0x1000).
    pci_device_t dev;
    int found = 0;

    // Scan all PCI devices for virtio network devices.
    for (uint16_t bus = 0; bus < 256 && !found; bus++) {
        for (uint8_t d = 0; d < 32 && !found; d++) {
            uint32_t id = pci_cfg_read32((uint8_t)bus, d, 0, 0);
            if ((id & 0xFFFF) != VIRTIO_VENDOR) continue;
            uint16_t did = (uint16_t)(id >> 16);
            if (did != VIRTIO_DEV_NET_MODERN && did != VIRTIO_DEV_NET_LEGACY) continue;
            // Check it's actually a network device.
            uint32_t cc = pci_cfg_read32((uint8_t)bus, d, 0, 0x08u) >> 16;
            uint8_t  cls = (uint8_t)(cc >> 8);
            uint8_t  sub = (uint8_t)(cc & 0xFF);
            if (cls != PCI_CLASS_NETWORK || sub != PCI_SUBCLASS_ETHERNET) {
                // Transitional devices may report 0xFF/0xFF — accept those too.
                if (cls != 0xFF) continue;
            }
            dev.bus       = (uint8_t)bus;
            dev.dev       = d;
            dev.fn        = 0;
            dev.vendor_id = VIRTIO_VENDOR;
            dev.device_id = did;
            found = 1;
        }
    }
    if (!found) return 0;

    s_dev = dev;

    // 2. Enable MMIO + bus mastering.
    pci_enable(dev.bus, dev.dev, dev.fn);

    // 3. Find virtio capability structures in PCI config space.
    vcap_t common_cap, notify_cap, devcfg_cap;
    if (!find_virtio_cap(dev.bus, dev.dev, dev.fn,
                          VIRTIO_PCI_CAP_COMMON_CFG, &common_cap)) return 0;
    if (!find_virtio_cap(dev.bus, dev.dev, dev.fn,
                          VIRTIO_PCI_CAP_NOTIFY_CFG, &notify_cap)) return 0;
    if (!find_virtio_cap(dev.bus, dev.dev, dev.fn,
                          VIRTIO_PCI_CAP_DEVICE_CFG, &devcfg_cap)) return 0;

    // 4. Map the three MMIO regions.
    uint64_t common_bar = pci_bar_base(dev.bus, dev.dev, dev.fn, common_cap.bar);
    uint64_t notify_bar = pci_bar_base(dev.bus, dev.dev, dev.fn, notify_cap.bar);
    uint64_t devcfg_bar = pci_bar_base(dev.bus, dev.dev, dev.fn, devcfg_cap.bar);

    s_common = (volatile virtio_pci_common_cfg_t*)
               vmm_map_mmio((phys_addr_t)(common_bar + common_cap.offset), 0x1000u);
    s_notify = (volatile uint8_t*)
               vmm_map_mmio((phys_addr_t)(notify_bar + notify_cap.offset), 0x1000u);
    s_devcfg = (volatile virtio_net_cfg_t*)
               vmm_map_mmio((phys_addr_t)(devcfg_bar + devcfg_cap.offset), 0x1000u);
    s_notify_mult = notify_cap.extra;  // notify_off_multiplier

    if (!s_common || !s_notify || !s_devcfg) return 0;

    // 5. Device initialisation sequence (virtio spec §3.1.1).

    // Step 1: Reset device.
    s_common->device_status = 0;
    __asm__ volatile("mfence" ::: "memory");
    // Wait for reset to complete.
    for (int i = 0; i < 1000000 && s_common->device_status != 0; i++);

    // Step 2: Acknowledge.
    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    __asm__ volatile("mfence" ::: "memory");

    // Step 3: Driver.
    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    __asm__ volatile("mfence" ::: "memory");

    // Step 4: Read and negotiate features.
    s_common->device_feature_select = 0;
    __asm__ volatile("mfence" ::: "memory");
    uint32_t dev_feat_lo = s_common->device_feature;
    s_common->device_feature_select = 1;
    __asm__ volatile("mfence" ::: "memory");
    uint32_t dev_feat_hi = s_common->device_feature;

    uint64_t dev_features = ((uint64_t)dev_feat_hi << 32) | dev_feat_lo;
    uint64_t drv_features = DRIVER_FEATURES & dev_features;  // intersect

    s_common->driver_feature_select = 0;
    s_common->driver_feature        = (uint32_t)(drv_features & 0xFFFFFFFFu);
    s_common->driver_feature_select = 1;
    s_common->driver_feature        = (uint32_t)(drv_features >> 32);
    __asm__ volatile("mfence" ::: "memory");

    // Step 5: Features OK.
    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE |
                               VIRTIO_STATUS_DRIVER |
                               VIRTIO_STATUS_FEATURES_OK;
    __asm__ volatile("mfence" ::: "memory");
    // Verify the device accepted our features.
    if (!(s_common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        s_common->device_status = VIRTIO_STATUS_FAILED;
        return 0;
    }

    // Step 6: Configure virtqueues.

    // Allocate RX packet buffers (one page each = 4 KiB, holds max frame).
    // Exactly VIRTQ_NUM_RX_BUFS slots are populated; s_rx_bufs[] entries beyond
    // that stay {NULL, 0} and are rejected by virtio_rx_id_valid at completion.
    for (uint16_t i = 0; i < VIRTQ_NUM_RX_BUFS; i++) {
        phys_addr_t p = pmm_buddy_alloc(0);
        if (!p) return 0;
        s_rx_bufs[i].phys = p;
        s_rx_bufs[i].virt = (uint8_t*)((uintptr_t)p + HHDM_OFFSET);
    }

    // Allocate TX buffer (one page).
    s_tx_buf_phys = pmm_buddy_alloc(0);
    if (!s_tx_buf_phys) return 0;
    s_tx_buf_virt = (uint8_t*)((uintptr_t)s_tx_buf_phys + HHDM_OFFSET);

    // Allocate and init virtqueues.
    if (!virtq_alloc(&s_rxq)) return 0;
    if (!virtq_alloc(&s_txq)) return 0;

    // Read notify offsets for each queue.
    s_common->queue_select = 0;
    __asm__ volatile("mfence" ::: "memory");
    s_rxq.notify_off = s_common->queue_notify_off;
    s_common->queue_select = 1;
    __asm__ volatile("mfence" ::: "memory");
    s_txq.notify_off = s_common->queue_notify_off;

    // Activate queues (write ring addresses).
    s_common->queue_select = 0;
    __asm__ volatile("mfence" ::: "memory");
    s_common->queue_size = VIRTQ_SIZE;
    virtq_activate(s_common, 0, &s_rxq);

    s_common->queue_select = 1;
    __asm__ volatile("mfence" ::: "memory");
    s_common->queue_size = VIRTQ_SIZE;
    virtq_activate(s_common, 1, &s_txq);

    // Step 7: Set DRIVER_OK — device is live.
    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE |
                               VIRTIO_STATUS_DRIVER |
                               VIRTIO_STATUS_FEATURES_OK |
                               VIRTIO_STATUS_DRIVER_OK;
    __asm__ volatile("mfence" ::: "memory");

    // 6. Read MAC address from device config.
    for (uint32_t i = 0; i < ETH_ALEN; i++)
        s_mac[i] = s_devcfg->mac[i];

    // 7. Pre-populate RX ring.
    rxq_refill();

    // 8. Enable MSI-X and register ISR.
    //
    // QEMU's modern virtio-net-pci ONLY exposes MSI-X (cap 0x11), no
    // legacy MSI (cap 0x05).  Previously we searched only for 0x05, so
    // no IRQ was ever wired up and RX silently stalled — the device
    // had plenty of incoming packets but no notification reached us.
    //
    // Steps:
    //   - Locate MSI-X capability
    //   - Map the MSI-X table BAR
    //   - Program entry 0: (LAPIC addr, VEC_VIRTIO_NET)
    //   - Unmask entry 0
    //   - Enable MSI-X in the capability control word
    //   - Tell virtio which MSI-X vector serves queue 0 (and queue 1)
    //     via queue_select / queue_msix_vector
    {
        uint8_t cap = (uint8_t)(pci_cfg_read32(dev.bus, dev.dev, dev.fn,
                                               0x34u) & 0xFCu);
        uint8_t msix_cap = 0;
        {
            uint8_t c = cap;
            while (c) {
                uint32_t dw = pci_cfg_read32(dev.bus, dev.dev, dev.fn, c);
                if ((dw & 0xFFu) == 0x11u) { msix_cap = c; break; }
                c = (uint8_t)((dw >> 8) & 0xFCu);
            }
        }
        if (!msix_cap) {
            // Fall back to legacy MSI if MSI-X is absent (very old QEMU).
            cap = (uint8_t)(pci_cfg_read32(dev.bus, dev.dev, dev.fn,
                                           0x34u) & 0xFCu);
            while (cap) {
                uint32_t dw = pci_cfg_read32(dev.bus, dev.dev, dev.fn, cap);
                if ((dw & 0xFFu) == 0x05u) {
                    uint32_t mc = dw;
                    uint64_t msi_addr = lapic_msi_addr();
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap + 4u,
                                    (uint32_t)(msi_addr & 0xFFFFFFFFu));
                    int is_64 = (int)((mc >> 23) & 1u);
                    if (is_64) {
                        pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap + 8u,
                                        (uint32_t)(msi_addr >> 32));
                        pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap + 12u,
                                        lapic_msi_data(VEC_VIRTIO_NET));
                    } else {
                        pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap + 8u,
                                        lapic_msi_data(VEC_VIRTIO_NET));
                    }
                    pci_cfg_write32(dev.bus, dev.dev, dev.fn, cap,
                                    (mc & ~(0x7u << 20)) | (1u << 16));
                    break;
                }
                cap = (uint8_t)((dw >> 8) & 0xFCu);
            }
        } else {
            // MSI-X path.
            uint32_t tbl_dw = pci_cfg_read32(dev.bus, dev.dev, dev.fn,
                                               msix_cap + 4u);
            uint32_t bir     = tbl_dw & 0x7u;
            uint32_t tbl_off = tbl_dw & ~0x7u;
            uint64_t bar_phys = pci_bar_base(dev.bus, dev.dev, dev.fn,
                                              (uint8_t)bir);
            volatile uint32_t* msix_table = (volatile uint32_t*)
                vmm_map_mmio(bar_phys + tbl_off, 0x1000u);

            uint32_t msi_addr = (uint32_t)lapic_msi_addr();
            uint32_t msi_data = lapic_msi_data(VEC_VIRTIO_NET);
            msix_table[0] = msi_addr;       // addr_lo
            msix_table[1] = 0;               // addr_hi
            msix_table[2] = msi_data;        // data
            msix_table[3] = 0;               // vector_ctrl: unmask

            // Enable MSI-X (bit 31 of message control word); clear
            // function mask (bit 30).
            uint32_t mc = pci_cfg_read32(dev.bus, dev.dev, dev.fn, msix_cap);
            mc = (mc | (1u << 31)) & ~(1u << 30);
            pci_cfg_write32(dev.bus, dev.dev, dev.fn, msix_cap, mc);

            // Bind MSI-X vector 0 to RX queue (index 0) and TX queue
            // (index 1).  Virtio spec §4.1.4.3: select queue, write
            // queue_msix_vector.  0xFFFF = "no vector" (default).
            s_common->queue_select       = 0;   // rxq
            s_common->queue_msix_vector  = 0;
            s_common->queue_select       = 1;   // txq
            s_common->queue_msix_vector  = 0;   // share same vector
        }

        g_virtio_net_irq = 4u;  // logical irq_wait slot (free slot)
        idt_irq_register(VEC_VIRTIO_NET, (uint64_t)virtio_net_irq_entry);
    }

    s_ok = 1;
    return 1;
}

// ── virtio_desc_id_valid selftest ─────────────────────────────────────────
// Deterministic check of the device-supplied descriptor-id bounds guard that
// stops an OOB read of s_rx_bufs[] / OOB write of s_rxq.desc[] from a malicious
// or buggy device reporting an id >= VIRTQ_SIZE in the used ring.
void virtio_desc_id_valid_selftest(void) {
    extern void kprintf(const char*, ...);
    struct { uint32_t id; int want; } c[] = {
        { 0,            1 },  // first slot
        { VIRTQ_SIZE-1, 1 },  // last valid slot (255)
        { VIRTQ_SIZE,   0 },  // == 256, first OOB
        { 0xFFFFFFFFu,  0 },  // device garbage
        { 1000u,        0 },  // arbitrary OOB
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        if (virtio_desc_id_valid(c[i].id) != c[i].want) {
            kprintf("[virtio_descid] FAIL id=%lu got=%d want=%d\n",
                    (unsigned long)c[i].id, virtio_desc_id_valid(c[i].id), c[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[virtio_descid] SELF-TEST FAILED\n"
                  : "[virtio_descid] SELF-TEST PASSED (device desc-id bounds)\n");
}

// ── virtio_rx_id_valid selftest ───────────────────────────────────────────
// Deterministic check of the TIGHTER RX-completion bound: an id is valid only
// if it indexes a POPULATED s_rx_bufs[] slot ([0, VIRTQ_NUM_RX_BUFS)).  The
// gap [VIRTQ_NUM_RX_BUFS, VIRTQ_SIZE) passes the generic desc-id guard but must
// be REJECTED here -- those slots are {NULL, 0} and a device inventing such an
// id would memcpy from NULL and re-post a descriptor at physical frame 0.
void virtio_rx_id_valid_selftest(void) {
    extern void kprintf(const char*, ...);
    struct { uint32_t id; int want; } c[] = {
        { 0,                    1 },  // first populated slot
        { VIRTQ_NUM_RX_BUFS-1,  1 },  // last populated slot (127)
        { VIRTQ_NUM_RX_BUFS,    0 },  // first unpopulated (128): generic-valid but rx-INVALID
        { VIRTQ_SIZE-1,         0 },  // 255: in the desc ring but no rx buffer
        { VIRTQ_SIZE,           0 },  // == 256, OOB everywhere
        { 0xFFFFFFFFu,          0 },  // device garbage
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        if (virtio_rx_id_valid(c[i].id) != c[i].want) {
            kprintf("[virtio_rxid] FAIL id=%lu got=%d want=%d\n",
                    (unsigned long)c[i].id, virtio_rx_id_valid(c[i].id), c[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[virtio_rxid] SELF-TEST FAILED\n"
                  : "[virtio_rxid] SELF-TEST PASSED (rx-completion id bounded to populated buffers)\n");
}
