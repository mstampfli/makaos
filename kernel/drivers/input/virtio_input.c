// ── virtio-input guest driver (absolute pointer / tablet) ─────────────────
//
// Binds the first virtio-input PCI device (vendor 0x1af4, device 0x1052,
// e.g. QEMU's `-device virtio-tablet-pci`) and bridges its event stream
// straight into the evdev layer.  virtio-input events ARE Linux evdev
// events ({le16 type; le16 code; le32 value}, including SYN framing), so
// the bridge is a passthrough — no decode step.
//
// WHY: a relative PS/2 mouse under QEMU SDL needs pointer grab, and in
// grab mode SDL hides the cursor sprite entirely — a persistent visible
// guest cursor is impossible (the old per-frame cursor redefine "flicker"
// was the only thing ever making it visible).  An ABSOLUTE tablet needs
// no grab: the host cursor maps 1:1 onto the guest cursor — always
// visible, no flicker, no teleporting.  This is how every Linux VM
// solves the same problem (libvirt defaults to a USB/virtio tablet).
//
// Device caps (key/rel/abs bitmaps, abs ranges, name, ids) are read from
// the virtio-input config space, so this driver works unmodified for
// virtio-keyboard/-mouse/-tablet devices.
//
// Single-device binding: only the FIRST virtio-input function is bound
// (see docs/SCALABILITY_DEBT.md) — today's topology has exactly one
// (the tablet); the loop + per-device state generalization is mechanical.
//
// Patterned on virtio_net.c (modern virtio-PCI caps + MSI-X) and
// virtio_gpu.c (virtq helpers).

#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "smp.h"
#include "idt.h"
#include "lapic.h"
#include "irq_wait.h"
#include "common.h"
#include "sched.h"
#include "process.h"
#include "evdev.h"

extern void virtio_input_irq_entry(void);
extern void kprintf(const char*, ...);

// ── PCI / virtio constants ────────────────────────────────────────────────
#define VIRTIO_VENDOR           0x1AF4u
#define VIRTIO_DEV_INPUT        0x1052u  // 0x1040 + VIRTIO_ID_INPUT (18)

#define VIRTIO_PCI_CAP_COMMON_CFG  1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2u
#define VIRTIO_PCI_CAP_DEVICE_CFG  4u

#define VIRTIO_STATUS_ACKNOWLEDGE   1u
#define VIRTIO_STATUS_DRIVER        2u
#define VIRTIO_STATUS_DRIVER_OK     4u
#define VIRTIO_STATUS_FEATURES_OK   8u
#define VIRTIO_STATUS_FAILED       128u

#define VIRTIO_F_VERSION_1          (1ULL << 32)

// virtio-input config selectors (spec §5.8.4)
#define VTI_CFG_ID_NAME    0x01u
#define VTI_CFG_ID_DEVIDS  0x03u
#define VTI_CFG_EV_BITS    0x11u
#define VTI_CFG_ABS_INFO   0x12u

// Logical irq_wait slot (16+ = non-ISA logical slots; see irq_wait.h).
#define VTI_IRQ_SLOT  16u

// ── virtio-PCI common config (spec §4.1.4.3) — same layout as net/gpu ────
typedef struct __attribute__((packed)) {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint32_t queue_desc_lo;
    uint32_t queue_desc_hi;
    uint32_t queue_driver_lo;
    uint32_t queue_driver_hi;
    uint32_t queue_device_lo;
    uint32_t queue_device_hi;
} virtio_pci_common_cfg_t;

// virtio-input device config window (spec §5.8.4).
typedef struct __attribute__((packed)) {
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    uint8_t data[128];   // string / bitmap / abs-info / dev-ids payload
} virtio_input_cfg_t;

// One event — identical to a Linux evdev event minus the timestamp.
typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} virtio_input_event_t;

// ── Virtqueue (same scheme as virtio_gpu.c) ───────────────────────────────
#define VTI_QSZ 64u

#define VIRTQ_DESC_F_NEXT   1u
#define VIRTQ_DESC_F_WRITE  2u

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VTI_QSZ];
    uint16_t used_event;
} virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    struct { uint32_t id; uint32_t len; } ring[VTI_QSZ];
    uint16_t avail_event;
} virtq_used_t;

typedef struct {
    virtq_desc_t*  desc;
    virtq_avail_t* avail;
    virtq_used_t*  used;
    phys_addr_t    desc_phys, avail_phys, used_phys;
    uint16_t       avail_idx;
    uint16_t       last_used_idx;
    uint16_t       notify_off;
} vti_virtq_t;

// ── Driver state ──────────────────────────────────────────────────────────
static int s_ok = 0;
static volatile virtio_pci_common_cfg_t* s_common = NULL;
static volatile uint8_t*                 s_notify = NULL;
static volatile virtio_input_cfg_t*      s_cfg    = NULL;
static uint32_t                          s_notify_mult = 0;

static vti_virtq_t      s_eventq;            // queue 0: device → driver
static phys_addr_t      s_evbuf_phys = 0;    // VTI_QSZ × 8-byte event slots
static virtio_input_event_t* s_evbuf = NULL;

static input_device_t*  s_idev = NULL;       // evdev registration

// ── PCI capability walker (same as virtio_net.c) ──────────────────────────
typedef struct {
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
    uint32_t extra;
} vcap_t;

static int find_virtio_cap(uint8_t bus, uint8_t dev, uint8_t fn,
                            uint8_t cap_type, vcap_t* out) {
    uint8_t cap = (uint8_t)(pci_cfg_read32(bus, dev, fn, 0x34u) & 0xFCu);
    while (cap) {
        uint32_t dw0  = pci_cfg_read32(bus, dev, fn, cap);
        uint8_t  id   = (uint8_t)(dw0 & 0xFFu);
        uint8_t  next = (uint8_t)((dw0 >> 8) & 0xFCu);
        uint8_t  len  = (uint8_t)((dw0 >> 16) & 0xFFu);
        if (id == 0x09u && len >= 16u) {
            uint32_t dw1 = pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 4u));
            uint32_t dw2 = pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 8u));
            uint32_t dw3 = pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 12u));
            uint8_t cfg_type = (uint8_t)((dw0 >> 24) & 0xFFu);
            if (cfg_type == cap_type) {
                out->bar    = (uint8_t)(dw1 & 0xFFu);
                out->offset = dw2;
                out->length = dw3;
                out->extra  = (cap_type == VIRTIO_PCI_CAP_NOTIFY_CFG)
                    ? pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 16u)) : 0;
                return 1;
            }
        }
        cap = next;
    }
    return 0;
}

// ── Device-config queries ─────────────────────────────────────────────────
// Write select/subsel, then the device exposes `size` payload bytes.
static uint8_t vti_cfg_query(uint8_t sel, uint8_t subsel,
                             uint8_t* out, uint8_t cap) {
    s_cfg->select = sel;
    s_cfg->subsel = subsel;
    __asm__ volatile("mfence" ::: "memory");
    uint8_t size = s_cfg->size;
    if (size > cap) size = cap;
    for (uint8_t i = 0; i < size; i++) out[i] = s_cfg->data[i];
    return size;
}

// ── Event delivery: ISR → kthread → evdev ─────────────────────────────────

void virtio_input_irq_handler(void) {
    if (!s_ok) return;
    irq_notify(VTI_IRQ_SLOT);
}

static void vti_drain(void) {
    vti_virtq_t* vq = &s_eventq;
    uint16_t used_idx = vq->used->idx;
    __asm__ volatile("lfence" ::: "memory");
    if (used_idx == vq->last_used_idx) return;

    while (vq->last_used_idx != used_idx) {
        uint32_t id = vq->used->ring[vq->last_used_idx % VTI_QSZ].id;
        if (id < VTI_QSZ) {
            virtio_input_event_t ev = s_evbuf[id];
            // virtio-input events are evdev events — pass straight
            // through (EV_ABS/EV_REL/EV_KEY/EV_SYN, host-framed).
            input_device_emit(s_idev, ev.type, ev.code, (int32_t)ev.value);
            // Repost the slot for the device to fill again.
            vq->avail->ring[vq->avail_idx % VTI_QSZ] = (uint16_t)id;
            vq->avail_idx++;
        }
        vq->last_used_idx++;
    }
    __asm__ volatile("mfence" ::: "memory");
    vq->avail->idx = vq->avail_idx;
    __asm__ volatile("mfence" ::: "memory");
    *(volatile uint16_t*)(s_notify + vq->notify_off * s_notify_mult) = 0;
}

static void vti_thread_fn(void) {
    for (;;) {
        irq_wait(VTI_IRQ_SLOT);
        vti_drain();
    }
}

// ── Registration: mirror the device's advertised evdev capabilities ──────
static void vti_register_caps(void) {
    uint8_t buf[128];

    // Name + identity.
    char name[80];
    uint8_t n = vti_cfg_query(VTI_CFG_ID_NAME, 0, (uint8_t*)name,
                              sizeof(name) - 1);
    name[n] = '\0';
    if (!n) { name[0] = 'v'; name[1] = 'i'; name[2] = 'r'; name[3] = 't';
              name[4] = 'i'; name[5] = 'o'; name[6] = '\0'; }

    uint16_t ids[4] = {BUS_VIRTUAL, 0x1AF4u, VIRTIO_DEV_INPUT, 1};
    if (vti_cfg_query(VTI_CFG_ID_DEVIDS, 0, (uint8_t*)ids, sizeof(ids))
            < sizeof(ids)) {
        ids[0] = BUS_VIRTUAL; ids[1] = 0x1AF4u;
        ids[2] = VIRTIO_DEV_INPUT; ids[3] = 1;
    }

    s_idev = input_device_register(name, ids[0], ids[1], ids[2], ids[3]);
    if (!s_idev) return;

    // EV_KEY bitmap → key bits.
    n = vti_cfg_query(VTI_CFG_EV_BITS, EV_KEY, buf, sizeof(buf));
    for (uint16_t code = 0; code < (uint16_t)(n * 8u); code++)
        if (buf[code >> 3] & (1u << (code & 7u)))
            input_device_set_key_bit(s_idev, code);

    // EV_REL bitmap → rel bits (tablet advertises REL_WHEEL).
    n = vti_cfg_query(VTI_CFG_EV_BITS, EV_REL, buf, sizeof(buf));
    for (uint16_t code = 0; code < (uint16_t)(n * 8u); code++)
        if (buf[code >> 3] & (1u << (code & 7u)))
            input_device_set_rel_bit(s_idev, code);

    // EV_ABS bitmap → abs bits, each with its real range from ABS_INFO
    // (libinput rejects absolute axes without sane min/max).
    n = vti_cfg_query(VTI_CFG_EV_BITS, EV_ABS, buf, sizeof(buf));
    for (uint16_t code = 0; code < (uint16_t)(n * 8u); code++) {
        if (!(buf[code >> 3] & (1u << (code & 7u)))) continue;
        uint32_t ai[5] = {0};   // le32 min, max, fuzz, flat, res
        vti_cfg_query(VTI_CFG_ABS_INFO, (uint8_t)code,
                      (uint8_t*)ai, sizeof(ai));
        input_absinfo_t info = {
            .value      = 0,
            .minimum    = (int32_t)ai[0],
            .maximum    = (int32_t)ai[1],
            .fuzz       = (int32_t)ai[2],
            .flat       = (int32_t)ai[3],
            .resolution = (int32_t)ai[4],
        };
        input_device_set_abs_bit(s_idev, code, &info);
    }
}

// ── Init ──────────────────────────────────────────────────────────────────
int virtio_input_init(void) {
    // 1. Find the first virtio-input function.
    uint8_t bus = 0, dv = 0;
    int found = 0;
    for (uint16_t b = 0; b < 256 && !found; b++) {
        for (uint8_t d = 0; d < 32 && !found; d++) {
            uint32_t id = pci_cfg_read32((uint8_t)b, d, 0, 0);
            if ((id & 0xFFFFu) != VIRTIO_VENDOR) continue;
            if ((uint16_t)(id >> 16) != VIRTIO_DEV_INPUT) continue;
            bus = (uint8_t)b; dv = d; found = 1;
        }
    }
    if (!found) return 0;   // no tablet attached — not an error

    pci_enable(bus, dv, 0);

    // 2. Map common / notify / device-config windows.
    vcap_t ccap, ncap, dcap;
    if (!find_virtio_cap(bus, dv, 0, VIRTIO_PCI_CAP_COMMON_CFG, &ccap) ||
        !find_virtio_cap(bus, dv, 0, VIRTIO_PCI_CAP_NOTIFY_CFG, &ncap) ||
        !find_virtio_cap(bus, dv, 0, VIRTIO_PCI_CAP_DEVICE_CFG, &dcap)) {
        kprintf("[virtio-input] missing virtio caps\n");
        return 0;
    }
    s_common = (volatile virtio_pci_common_cfg_t*)vmm_map_mmio(
        pci_bar_base(bus, dv, 0, ccap.bar) + ccap.offset, 0x1000u);
    s_notify = (volatile uint8_t*)vmm_map_mmio(
        pci_bar_base(bus, dv, 0, ncap.bar) + ncap.offset, 0x1000u);
    s_cfg = (volatile virtio_input_cfg_t*)vmm_map_mmio(
        pci_bar_base(bus, dv, 0, dcap.bar) + dcap.offset, 0x1000u);
    s_notify_mult = ncap.extra;
    if (!s_common || !s_notify || !s_cfg) return 0;

    // 3. Standard virtio init dance (reset → ACK → DRIVER → features).
    s_common->device_status = 0;
    __asm__ volatile("mfence" ::: "memory");
    for (int i = 0; i < 1000000 && s_common->device_status != 0; i++);
    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    __asm__ volatile("mfence" ::: "memory");
    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    __asm__ volatile("mfence" ::: "memory");

    s_common->driver_feature_select = 0;
    s_common->driver_feature        = 0;
    s_common->driver_feature_select = 1;
    s_common->driver_feature        = (uint32_t)(VIRTIO_F_VERSION_1 >> 32);
    __asm__ volatile("mfence" ::: "memory");
    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE |
                              VIRTIO_STATUS_DRIVER |
                              VIRTIO_STATUS_FEATURES_OK;
    __asm__ volatile("mfence" ::: "memory");
    if (!(s_common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        s_common->device_status = VIRTIO_STATUS_FAILED;
        kprintf("[virtio-input] FEATURES_OK rejected\n");
        return 0;
    }

    // 4. Event queue: VTI_QSZ 8-byte device-writable slots in one page.
    s_eventq.desc_phys  = pmm_buddy_alloc(0);
    s_eventq.avail_phys = pmm_buddy_alloc(0);
    s_eventq.used_phys  = pmm_buddy_alloc(0);
    s_evbuf_phys        = pmm_buddy_alloc(0);
    if (!s_eventq.desc_phys || !s_eventq.avail_phys ||
        !s_eventq.used_phys || !s_evbuf_phys)
        return 0;
    s_eventq.desc  = (virtq_desc_t*) (s_eventq.desc_phys  + HHDM_OFFSET);
    s_eventq.avail = (virtq_avail_t*)(s_eventq.avail_phys + HHDM_OFFSET);
    s_eventq.used  = (virtq_used_t*) (s_eventq.used_phys  + HHDM_OFFSET);
    s_evbuf        = (virtio_input_event_t*)(s_evbuf_phys + HHDM_OFFSET);
    __builtin_memset(s_eventq.desc,  0, VTI_QSZ * sizeof(virtq_desc_t));
    __builtin_memset(s_eventq.avail, 0, sizeof(virtq_avail_t));
    __builtin_memset(s_eventq.used,  0, sizeof(virtq_used_t));

    for (uint16_t i = 0; i < VTI_QSZ; i++) {
        s_eventq.desc[i].addr  = s_evbuf_phys + (uint64_t)i * 8u;
        s_eventq.desc[i].len   = 8u;
        s_eventq.desc[i].flags = VIRTQ_DESC_F_WRITE;
        s_eventq.avail->ring[i] = i;
    }
    s_eventq.avail_idx     = VTI_QSZ;
    s_eventq.last_used_idx = 0;
    // PUBLISH the buffers to the device: avail->idx is what the DEVICE
    // reads — without this it sees zero available buffers and silently
    // drops every input event (observed: used_idx stuck at 0 while the
    // host pointer moved).  The shadow avail_idx above is driver-side
    // bookkeeping only.
    __asm__ volatile("mfence" ::: "memory");
    s_eventq.avail->idx = VTI_QSZ;

    s_common->queue_select = 0;
    __asm__ volatile("mfence" ::: "memory");
    s_common->queue_size   = VTI_QSZ;
    s_eventq.notify_off    = s_common->queue_notify_off;
    s_common->queue_desc_lo   = (uint32_t)(s_eventq.desc_phys  & 0xFFFFFFFFu);
    s_common->queue_desc_hi   = (uint32_t)(s_eventq.desc_phys  >> 32);
    s_common->queue_driver_lo = (uint32_t)(s_eventq.avail_phys & 0xFFFFFFFFu);
    s_common->queue_driver_hi = (uint32_t)(s_eventq.avail_phys >> 32);
    s_common->queue_device_lo = (uint32_t)(s_eventq.used_phys  & 0xFFFFFFFFu);
    s_common->queue_device_hi = (uint32_t)(s_eventq.used_phys  >> 32);

    // 5. MSI-X: entry 0 → VEC_VIRTIO_INPUT, bound to the event queue.
    {
        uint8_t cap = (uint8_t)(pci_cfg_read32(bus, dv, 0, 0x34u) & 0xFCu);
        uint8_t msix_cap = 0;
        while (cap) {
            uint32_t dw = pci_cfg_read32(bus, dv, 0, cap);
            if ((dw & 0xFFu) == 0x11u) { msix_cap = cap; break; }
            cap = (uint8_t)((dw >> 8) & 0xFCu);
        }
        if (!msix_cap) { kprintf("[virtio-input] no MSI-X\n"); return 0; }

        uint32_t tbl_dw  = pci_cfg_read32(bus, dv, 0, msix_cap + 4u);
        uint64_t bar     = pci_bar_base(bus, dv, 0, (uint8_t)(tbl_dw & 0x7u));
        volatile uint32_t* tbl = (volatile uint32_t*)
            vmm_map_mmio(bar + (tbl_dw & ~0x7u), 0x1000u);
        if (!tbl) return 0;
        tbl[0] = (uint32_t)lapic_msi_addr();
        tbl[1] = 0;
        tbl[2] = lapic_msi_data(VEC_VIRTIO_INPUT);
        tbl[3] = 0;   // unmask entry

        uint32_t mc = pci_cfg_read32(bus, dv, 0, msix_cap);
        pci_cfg_write32(bus, dv, 0, msix_cap,
                        (mc | (1u << 31)) & ~(1u << 30));

        s_common->queue_select      = 0;
        s_common->queue_msix_vector = 0;
        __asm__ volatile("mfence" ::: "memory");
    }

    s_common->queue_select = 0;
    __asm__ volatile("mfence" ::: "memory");
    s_common->queue_enable = 1;
    __asm__ volatile("mfence" ::: "memory");

    // 6. Register evdev caps from device config, go live, kick the queue.
    vti_register_caps();
    if (!s_idev) return 0;

    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE |
                              VIRTIO_STATUS_DRIVER |
                              VIRTIO_STATUS_FEATURES_OK |
                              VIRTIO_STATUS_DRIVER_OK;
    __asm__ volatile("mfence" ::: "memory");

    idt_irq_register(VEC_VIRTIO_INPUT, (uint64_t)virtio_input_irq_entry);
    s_ok = 1;

    *(volatile uint16_t*)(s_notify + s_eventq.notify_off * s_notify_mult) = 0;

    task_t* t = task_create_kthread(vti_thread_fn, pid_alloc());
    if (t) sched_add(t);

    kprintf("[virtio-input] bound %02x:%02x event%u\n",
            bus, dv, (unsigned)0);
    return 1;
}
