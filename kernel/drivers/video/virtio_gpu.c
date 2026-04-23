// ── virtio-gpu — 2D scanout driver (virtio 1.1 §5.7) ────────────────
//
// Exists so Hyprland / wlroots / any DRM client has a real modesetting
// device to talk to.  Tier 2.5a brings the device to life: PCI probe,
// feature negotiate, control virtqueue, GET_DISPLAY_INFO → enumerate
// scanouts.  Tier 2.5b layers the Linux DRM ioctl uAPI on top.
//
// We mirror kernel/net/virtio_net.c for everything up to virtqueue
// activation — the virtio-PCI transport is identical, only the device
// configuration block and the command payload differ.

#include "virtio_gpu.h"
#include "drm_backend.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "kprintf.h"
#include "log.h"
#include "trace.h"
#include "smp.h"
#include "common.h"

// ── PCI IDs ─────────────────────────────────────────────────────────
#define VIRTIO_VENDOR            0x1AF4u
#define VIRTIO_DEV_GPU_MODERN    0x1050u   // virtio 1.x GPU device
#define VIRTIO_DEV_GPU_LEGACY    0x1040u   // transitional (also accepted)

// ── virtio-PCI capability types ─────────────────────────────────────
#define VIRTIO_PCI_CAP_COMMON_CFG  1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2u
#define VIRTIO_PCI_CAP_ISR_CFG     3u
#define VIRTIO_PCI_CAP_DEVICE_CFG  4u

// ── virtio status bits ──────────────────────────────────────────────
#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER      2u
#define VIRTIO_STATUS_DRIVER_OK   4u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_FAILED      128u

// ── virtio-gpu feature bits (§5.7.3) ────────────────────────────────
#define VIRTIO_F_VERSION_1             (1ULL << 32)
#define VIRTIO_GPU_F_VIRGL             (1ULL << 0)   // 3D support (skip for now)
#define VIRTIO_GPU_F_EDID              (1ULL << 1)   // EDID blobs
#define VIRTIO_GPU_F_RESOURCE_UUID     (1ULL << 2)
#define VIRTIO_GPU_F_RESOURCE_BLOB     (1ULL << 3)
#define VIRTIO_GPU_F_CONTEXT_INIT      (1ULL << 4)

#define DRIVER_FEATURES                (VIRTIO_F_VERSION_1 | VIRTIO_GPU_F_EDID)

// ── Queue indices (§5.7.2) ──────────────────────────────────────────
#define VQ_CONTROLQ  0
#define VQ_CURSORQ   1

#define VIRTQ_SIZE   64u

// ── virtio-gpu control command opcodes (§5.7.6.7) ───────────────────
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO         0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET              0x0109
#define VIRTIO_GPU_CMD_GET_EDID                0x010a

#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO         0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET              0x1103
#define VIRTIO_GPU_RESP_OK_EDID                0x1104

#define VIRTIO_GPU_MAX_SCANOUTS                16

// ── virtio-PCI common configuration (§4.1.4.3) ──────────────────────
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

// ── virtio-gpu device configuration (§5.7.4) ────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t num_capsets;
} virtio_gpu_cfg_t;

// ── Virtqueue primitives (§2.6) ─────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

#define VIRTQ_DESC_F_NEXT   1u
#define VIRTQ_DESC_F_WRITE  2u

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_SIZE];
    uint16_t used_event;
} virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    struct { uint32_t id; uint32_t len; } ring[VIRTQ_SIZE];
    uint16_t avail_event;
} virtq_used_t;

typedef struct {
    virtq_desc_t*  desc;
    virtq_avail_t* avail;
    virtq_used_t*  used;
    phys_addr_t    desc_phys;
    phys_addr_t    avail_phys;
    phys_addr_t    used_phys;

    uint16_t free_head;
    uint16_t avail_idx;
    uint16_t last_used_idx;
    uint16_t notify_off;
} virtq_t;

// ── virtio-gpu control header (§5.7.6.2) ────────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t type;       // command / response opcode
    uint32_t flags;      // VIRTIO_GPU_FLAG_FENCE etc. (0 for synchronous)
    uint64_t fence_id;
    uint32_t ctx_id;     // 3D context (0 for 2D)
    uint32_t padding;
} virtio_gpu_ctrl_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t x, y, width, height;
} virtio_gpu_rect_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    struct {
        virtio_gpu_rect_t r;
        uint32_t          enabled;
        uint32_t          flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} virtio_gpu_resp_display_info_t;

// ── Driver state ────────────────────────────────────────────────────
static int s_ok = 0;
static pci_device_t                         s_dev;
static volatile virtio_pci_common_cfg_t*    s_common      = NULL;
static volatile uint8_t*                    s_notify      = NULL;
static volatile virtio_gpu_cfg_t*           s_devcfg      = NULL;
static uint32_t                             s_notify_mult = 0;

static virtq_t  s_ctrl_vq;   // VQ_CONTROLQ
static virtq_t  s_cursor_vq; // VQ_CURSORQ (allocated but unused for now)

static uint32_t s_num_scanouts = 0;
static struct { uint32_t w, h, enabled; } s_scanouts[VIRTIO_GPU_MAX_SCANOUTS];

// ── PCI capability walker (identical to virtio_net's) ───────────────
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
        uint32_t dw0 = pci_cfg_read32(bus, dev, fn, cap);
        uint8_t  id  = (uint8_t)(dw0 & 0xFFu);
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
                              ? pci_cfg_read32(bus, dev, fn, (uint8_t)(cap + 16u))
                              : 0;
                return 1;
            }
        }
        cap = next;
    }
    return 0;
}

// ── Virtqueue allocation + activation ───────────────────────────────
static int virtq_alloc(virtq_t* vq) {
    vq->desc_phys = pmm_buddy_alloc(0);
    if (!vq->desc_phys) return 0;
    vq->desc = (virtq_desc_t*)((uintptr_t)vq->desc_phys + HHDM_OFFSET);
    __builtin_memset(vq->desc, 0, VIRTQ_SIZE * sizeof(virtq_desc_t));

    vq->avail_phys = pmm_buddy_alloc(0);
    if (!vq->avail_phys) return 0;
    vq->avail = (virtq_avail_t*)((uintptr_t)vq->avail_phys + HHDM_OFFSET);
    __builtin_memset(vq->avail, 0, sizeof(virtq_avail_t));

    vq->used_phys = pmm_buddy_alloc(0);
    if (!vq->used_phys) return 0;
    vq->used = (virtq_used_t*)((uintptr_t)vq->used_phys + HHDM_OFFSET);
    __builtin_memset(vq->used, 0, sizeof(virtq_used_t));

    for (uint16_t i = 0; i < VIRTQ_SIZE - 1; i++)
        vq->desc[i].next = (uint16_t)(i + 1u);
    vq->desc[VIRTQ_SIZE - 1].next = 0xFFFFu;
    vq->free_head     = 0;
    vq->avail_idx     = 0;
    vq->last_used_idx = 0;
    return 1;
}

static void virtq_activate(volatile virtio_pci_common_cfg_t* cfg,
                            uint16_t qidx, virtq_t* vq) {
    cfg->queue_select = qidx;
    __asm__ volatile("mfence" ::: "memory");
    vq->notify_off      = cfg->queue_notify_off;
    cfg->queue_desc_lo   = (uint32_t)(vq->desc_phys  & 0xFFFFFFFFu);
    cfg->queue_desc_hi   = (uint32_t)(vq->desc_phys  >> 32);
    cfg->queue_driver_lo = (uint32_t)(vq->avail_phys & 0xFFFFFFFFu);
    cfg->queue_driver_hi = (uint32_t)(vq->avail_phys >> 32);
    cfg->queue_device_lo = (uint32_t)(vq->used_phys  & 0xFFFFFFFFu);
    cfg->queue_device_hi = (uint32_t)(vq->used_phys  >> 32);
    cfg->queue_enable    = 1;
    __asm__ volatile("mfence" ::: "memory");
}

// ── Send one command and poll for the response ─────────────────────
// We use a bounce buffer pair (request + response) allocated in
// physically-contiguous pages.  The GPU queue is idle most of the
// time so single-outstanding-request is fine; concurrency comes
// later with the DRM ioctl layer.  Returns 1 on success.
static phys_addr_t s_cmd_phys = 0;
static uint8_t*    s_cmd_virt = NULL;   // request at +0, response at +2048

#define CMD_REQ_OFF   0u
#define CMD_RESP_OFF  2048u

// ── Control-queue serialisation ──────────────────────────────────────
// The bounce buffers + virtqueue state above are global, so two
// callers (e.g. a DRM ioctl on CPU 0 while fbcon flushes on CPU 1)
// would corrupt each other's descriptors.  A single IRQ-safe spinlock
// around the whole submit+poll window is sufficient — virtqueue
// commands complete in microseconds in QEMU so the contention window
// is small.  Multiple outstanding commands are not worth the
// complexity until we have an interrupt-driven completion path.
static spinlock_t s_ctrl_lock = SPINLOCK_INIT;

static int vgpu_send_ctrl(const void* req, uint32_t req_len,
                           void* resp, uint32_t resp_len) {
    if (!s_cmd_virt) return 0;
    if (req_len  > CMD_RESP_OFF)       return 0;
    if (resp_len > (4096 - CMD_RESP_OFF)) return 0;

    spin_lock(&s_ctrl_lock);
    __builtin_memcpy(s_cmd_virt + CMD_REQ_OFF, req, req_len);
    __builtin_memset(s_cmd_virt + CMD_RESP_OFF, 0, resp_len);

    // Build a 2-descriptor chain: request (device-read) + response
    // (device-write).  Allocated from free list.
    virtq_t* vq = &s_ctrl_vq;
    uint16_t d0 = vq->free_head;
    if (d0 == 0xFFFFu) { spin_unlock(&s_ctrl_lock); return 0; }
    uint16_t d1 = vq->desc[d0].next;
    if (d1 == 0xFFFFu) { spin_unlock(&s_ctrl_lock); return 0; }
    vq->free_head = vq->desc[d1].next;

    vq->desc[d0].addr  = (uint64_t)(s_cmd_phys + CMD_REQ_OFF);
    vq->desc[d0].len   = req_len;
    vq->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[d0].next  = d1;

    vq->desc[d1].addr  = (uint64_t)(s_cmd_phys + CMD_RESP_OFF);
    vq->desc[d1].len   = resp_len;
    vq->desc[d1].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[d1].next  = 0;

    // Publish into avail ring.
    uint16_t slot = vq->avail_idx % VIRTQ_SIZE;
    vq->avail->ring[slot] = d0;
    __asm__ volatile("mfence" ::: "memory");
    vq->avail_idx++;
    vq->avail->idx = vq->avail_idx;
    __asm__ volatile("mfence" ::: "memory");

    // Notify the device.
    *(volatile uint16_t*)(s_notify + vq->notify_off * s_notify_mult) = VQ_CONTROLQ;
    __asm__ volatile("mfence" ::: "memory");

    // Poll for completion.  Bounded by a large counter — no interrupt
    // yet; the DRM layer will wire one once we're event-driven.
    for (int i = 0; i < 10000000; i++) {
        uint16_t used_idx = vq->used->idx;
        __asm__ volatile("lfence" ::: "memory");
        if (used_idx != vq->last_used_idx) {
            vq->last_used_idx = used_idx;
            // Response bytes are now in s_cmd_virt + CMD_RESP_OFF.
            __builtin_memcpy(resp, s_cmd_virt + CMD_RESP_OFF, resp_len);
            // Return descriptors to free list.
            vq->desc[d1].next = vq->free_head;
            vq->desc[d0].next = d1;
            vq->free_head = d0;
            spin_unlock(&s_ctrl_lock);
            return 1;
        }
    }
    // Timeout → leak descriptors (safer than racing the device).
    spin_unlock(&s_ctrl_lock);
    return 0;
}

// ── Init ────────────────────────────────────────────────────────────
int virtio_gpu_init(void) {
    pci_device_t dev;
    int found = 0;
    for (uint16_t bus = 0; bus < 256 && !found; bus++) {
        for (uint8_t d = 0; d < 32 && !found; d++) {
            uint32_t id = pci_cfg_read32((uint8_t)bus, d, 0, 0);
            if ((id & 0xFFFFu) != VIRTIO_VENDOR) continue;
            uint16_t did = (uint16_t)(id >> 16);
            if (did != VIRTIO_DEV_GPU_MODERN && did != VIRTIO_DEV_GPU_LEGACY) continue;
            dev.bus = (uint8_t)bus;
            dev.dev = d;
            dev.fn  = 0;
            dev.vendor_id = VIRTIO_VENDOR;
            dev.device_id = did;
            found = 1;
        }
    }
    if (!found) { kprintf("[virtio-gpu] no device found\n"); return 0; }
    s_dev = dev;
    kprintf("[virtio-gpu] found at %02x:%02x.%x did=%04x\n",
            dev.bus, dev.dev, dev.fn, dev.device_id);

    pci_enable(dev.bus, dev.dev, dev.fn);

    vcap_t common_cap, notify_cap, devcfg_cap;
    if (!find_virtio_cap(dev.bus, dev.dev, dev.fn,
                         VIRTIO_PCI_CAP_COMMON_CFG, &common_cap)) return 0;
    if (!find_virtio_cap(dev.bus, dev.dev, dev.fn,
                         VIRTIO_PCI_CAP_NOTIFY_CFG, &notify_cap)) return 0;
    if (!find_virtio_cap(dev.bus, dev.dev, dev.fn,
                         VIRTIO_PCI_CAP_DEVICE_CFG, &devcfg_cap)) return 0;

    uint64_t common_bar = pci_bar_base(dev.bus, dev.dev, dev.fn, common_cap.bar);
    uint64_t notify_bar = pci_bar_base(dev.bus, dev.dev, dev.fn, notify_cap.bar);
    uint64_t devcfg_bar = pci_bar_base(dev.bus, dev.dev, dev.fn, devcfg_cap.bar);

    s_common = (volatile virtio_pci_common_cfg_t*)
               vmm_map_mmio((phys_addr_t)(common_bar + common_cap.offset), 0x1000u);
    s_notify = (volatile uint8_t*)
               vmm_map_mmio((phys_addr_t)(notify_bar + notify_cap.offset), 0x4000u);
    s_devcfg = (volatile virtio_gpu_cfg_t*)
               vmm_map_mmio((phys_addr_t)(devcfg_bar + devcfg_cap.offset), 0x1000u);
    s_notify_mult = notify_cap.extra;
    if (!s_common || !s_notify || !s_devcfg) return 0;

    // Device init sequence (§3.1.1).
    s_common->device_status = 0;
    __asm__ volatile("mfence" ::: "memory");
    for (int i = 0; i < 1000000 && s_common->device_status != 0; i++);

    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    __asm__ volatile("mfence" ::: "memory");

    // Feature negotiation.
    s_common->device_feature_select = 0;
    uint32_t dflo = s_common->device_feature;
    s_common->device_feature_select = 1;
    uint32_t dfhi = s_common->device_feature;
    uint64_t dfeat = ((uint64_t)dfhi << 32) | dflo;
    uint64_t want  = DRIVER_FEATURES & dfeat;

    s_common->driver_feature_select = 0;
    s_common->driver_feature        = (uint32_t)(want & 0xFFFFFFFFu);
    s_common->driver_feature_select = 1;
    s_common->driver_feature        = (uint32_t)(want >> 32);
    __asm__ volatile("mfence" ::: "memory");

    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE |
                              VIRTIO_STATUS_DRIVER |
                              VIRTIO_STATUS_FEATURES_OK;
    __asm__ volatile("mfence" ::: "memory");
    if (!(s_common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[virtio-gpu] FEATURES_OK rejected\n");
        s_common->device_status = VIRTIO_STATUS_FAILED;
        return 0;
    }

    // Allocate both virtqueues.
    if (!virtq_alloc(&s_ctrl_vq))   { kprintf("[virtio-gpu] ctrl alloc fail\n");   return 0; }
    if (!virtq_alloc(&s_cursor_vq)) { kprintf("[virtio-gpu] cursor alloc fail\n"); return 0; }
    virtq_activate(s_common, VQ_CONTROLQ, &s_ctrl_vq);
    virtq_activate(s_common, VQ_CURSORQ,  &s_cursor_vq);

    // Command bounce buffer.
    s_cmd_phys = pmm_buddy_alloc(0);
    if (!s_cmd_phys) return 0;
    s_cmd_virt = (uint8_t*)((uintptr_t)s_cmd_phys + HHDM_OFFSET);

    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE |
                              VIRTIO_STATUS_DRIVER |
                              VIRTIO_STATUS_FEATURES_OK |
                              VIRTIO_STATUS_DRIVER_OK;
    __asm__ volatile("mfence" ::: "memory");

    // Probe displays.
    virtio_gpu_ctrl_hdr_t req = { .type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO };
    virtio_gpu_resp_display_info_t resp;
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) {
        kprintf("[virtio-gpu] GET_DISPLAY_INFO timeout\n");
        return 0;
    }
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        kprintf("[virtio-gpu] GET_DISPLAY_INFO bad resp=%x\n", resp.hdr.type);
        return 0;
    }

    uint32_t dev_scanouts = s_devcfg->num_scanouts;
    if (dev_scanouts > VIRTIO_GPU_MAX_SCANOUTS) dev_scanouts = VIRTIO_GPU_MAX_SCANOUTS;
    s_num_scanouts = dev_scanouts;
    for (uint32_t i = 0; i < dev_scanouts; i++) {
        s_scanouts[i].w       = resp.pmodes[i].r.width;
        s_scanouts[i].h       = resp.pmodes[i].r.height;
        s_scanouts[i].enabled = resp.pmodes[i].enabled;
        if (resp.pmodes[i].enabled) {
            kprintf("[virtio-gpu] scanout %u: %ux%u\n",
                    i, resp.pmodes[i].r.width, resp.pmodes[i].r.height);
        }
    }

    s_ok = 1;
    kprintf("[virtio-gpu] initialised (%u scanouts)\n", s_num_scanouts);
    return 1;
}

uint32_t virtio_gpu_num_scanouts(void) { return s_ok ? s_num_scanouts : 0; }

void virtio_gpu_get_mode(uint32_t idx, uint32_t* out_w, uint32_t* out_h) {
    if (!s_ok || idx >= s_num_scanouts || !s_scanouts[idx].enabled) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }
    if (out_w) *out_w = s_scanouts[idx].w;
    if (out_h) *out_h = s_scanouts[idx].h;
}

// ── Resource lifecycle + scanout driving ─────────────────────────────

// Pixel format — B8G8R8X8 (little-endian 0xAABBGGRR on-wire = blue low).
// Matches QEMU's SDL window default and the GOP framebuffer's layout,
// so our one in-memory buffer can feed both paths if we want.
#define VIRTIO_GPU_FORMAT_B8G8R8X8  2

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} vgpu_resource_unref_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width, height;
} vgpu_resource_create_2d_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    // followed by nr_entries × { uint64_t addr; uint32_t length; uint32_t pad; }
} vgpu_attach_backing_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} vgpu_mem_entry_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} vgpu_set_scanout_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} vgpu_xfer_to_host_2d_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} vgpu_resource_flush_t;

int virtio_gpu_resource_create_2d(uint32_t res_id, uint32_t fmt,
                                    uint32_t w, uint32_t h) {
    vgpu_resource_create_2d_t req = {0};
    req.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req.resource_id = res_id;
    req.format      = fmt;
    req.width       = w;
    req.height      = h;
    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) return 0;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

int virtio_gpu_resource_unref(uint32_t res_id) {
    vgpu_resource_unref_t req = {0};
    req.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    req.resource_id = res_id;
    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) return 0;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

int virtio_gpu_resource_attach_backing_single(uint32_t res_id,
                                               phys_addr_t phys, uint32_t len) {
    // Single mem_entry — attach one physically contiguous range.  The
    // common case for our driver-allocated buffers.  For more complex
    // SG lists (user-mapped dumb buffers later), walk the PTE tree and
    // emit one entry per run.
    struct {
        vgpu_attach_backing_t hdr;
        vgpu_mem_entry_t      entries[1];
    } __attribute__((packed)) req = {0};
    req.hdr.hdr.type     = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    req.hdr.resource_id  = res_id;
    req.hdr.nr_entries   = 1;
    req.entries[0].addr   = (uint64_t)phys;
    req.entries[0].length = len;
    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) return 0;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

int virtio_gpu_set_scanout(uint32_t scanout_id, uint32_t res_id,
                             uint32_t w, uint32_t h) {
    vgpu_set_scanout_t req = {0};
    req.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    req.r.width     = w;
    req.r.height    = h;
    req.scanout_id  = scanout_id;
    req.resource_id = res_id;
    virtio_gpu_ctrl_hdr_t resp = {0};
    TRACE(TRACE_GPU_SET_SCANOUT, scanout_id, res_id, w, h);
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) {
        pr_warn("virtio-gpu", "SET_SCANOUT sc=%u res=%u %ux%u: ctrl send failed",
                scanout_id, res_id, w, h);
        return 0;
    }
    int ok = (resp.type == VIRTIO_GPU_RESP_OK_NODATA);
    if (!ok) pr_warn("virtio-gpu",
                     "SET_SCANOUT sc=%u res=%u: host resp type=0x%x",
                     scanout_id, res_id, resp.type);
    return ok;
}

int virtio_gpu_transfer_to_host_2d(uint32_t res_id, uint32_t w, uint32_t h) {
    vgpu_xfer_to_host_2d_t req = {0};
    req.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    req.r.width     = w;
    req.r.height    = h;
    req.offset      = 0;
    req.resource_id = res_id;
    virtio_gpu_ctrl_hdr_t resp = {0};
    TRACE(TRACE_GPU_RES_TRANSFER, res_id, w, h, 0);
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) {
        pr_warn("virtio-gpu", "XFER_TO_HOST_2D res=%u %ux%u: ctrl send failed",
                res_id, w, h);
        return 0;
    }
    int ok = (resp.type == VIRTIO_GPU_RESP_OK_NODATA);
    if (!ok) pr_warn("virtio-gpu",
                     "XFER_TO_HOST_2D res=%u: host resp type=0x%x",
                     res_id, resp.type);
    return ok;
}

int virtio_gpu_resource_flush(uint32_t res_id, uint32_t w, uint32_t h) {
    vgpu_resource_flush_t req = {0};
    req.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    req.r.width     = w;
    req.r.height    = h;
    req.resource_id = res_id;
    virtio_gpu_ctrl_hdr_t resp = {0};
    TRACE(TRACE_GPU_RES_FLUSH, res_id, w, h, 0);
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) {
        pr_warn("virtio-gpu", "RESOURCE_FLUSH res=%u %ux%u: ctrl send failed",
                res_id, w, h);
        return 0;
    }
    int ok = (resp.type == VIRTIO_GPU_RESP_OK_NODATA);
    if (!ok) pr_warn("virtio-gpu",
                     "RESOURCE_FLUSH res=%u: host resp type=0x%x",
                     res_id, resp.type);
    return ok;
}

// ── Framebuffer state (kept for DRM layer + present_test) ────────────
static uint32_t     s_fb_res_id   = 0;       // 0 = not created
static uint32_t     s_fb_w        = 0;
static uint32_t     s_fb_h        = 0;
static phys_addr_t  s_fb_phys     = 0;
static uint8_t*     s_fb_virt     = NULL;
static uint32_t     s_fb_bytes    = 0;

// Allocate a buffer large enough for w*h*4 bytes, attach it, hand to
// scanout 0.  Idempotent — calling again re-creates (destroys old).
// Page-aligned + order-aligned so virtio-gpu sees one contiguous range.
static int vgpu_setup_scanout_buffer(uint32_t w, uint32_t h) {
    uint32_t bytes = w * h * 4;
    // Round up to a power-of-two page count.
    uint32_t pages = (bytes + 4095) / 4096;
    uint8_t  order = 0;
    while (((uint32_t)1 << order) < pages) order++;
    phys_addr_t phys = pmm_buddy_alloc(order);
    if (!phys) { kprintf("[virtio-gpu] fb alloc fail (%u pages order=%u)\n",
                         pages, order); return 0; }
    uint8_t* virt = (uint8_t*)((uintptr_t)phys + HHDM_OFFSET);
    __builtin_memset(virt, 0, (uint64_t)((uint64_t)1 << order) * 4096u);

    // Use resource_id = 1 (zero is reserved).
    const uint32_t res_id = 1;

    if (!virtio_gpu_resource_create_2d(res_id, VIRTIO_GPU_FORMAT_B8G8R8X8, w, h)) {
        kprintf("[virtio-gpu] RESOURCE_CREATE_2D failed\n");
        pmm_buddy_free(phys, order); return 0;
    }
    if (!virtio_gpu_resource_attach_backing_single(res_id, phys,
                                     (uint32_t)((uint64_t)1 << order) * 4096u)) {
        kprintf("[virtio-gpu] ATTACH_BACKING failed\n");
        pmm_buddy_free(phys, order); return 0;
    }
    if (!virtio_gpu_set_scanout(0, res_id, w, h)) {
        kprintf("[virtio-gpu] SET_SCANOUT failed\n");
        pmm_buddy_free(phys, order); return 0;
    }

    // File-scope; used by virtio_gpu_restore_default_scanout below.
    s_fb_res_id = res_id;
    s_fb_w      = w;
    s_fb_h      = h;
    s_fb_phys   = phys;
    s_fb_virt   = virt;
    s_fb_bytes  = (uint32_t)((uint64_t)1 << order) * 4096u;
    return 1;
}

int virtio_gpu_present_test(void) {
    if (!s_ok)           { kprintf("[virtio-gpu] present: not initialised\n"); return 0; }
    if (!s_num_scanouts) { kprintf("[virtio-gpu] present: no scanouts\n");    return 0; }

    uint32_t w = s_scanouts[0].w;
    uint32_t h = s_scanouts[0].h;
    if (!w || !h) { kprintf("[virtio-gpu] present: scanout 0 has 0x0 mode\n"); return 0; }

    if (!vgpu_setup_scanout_buffer(w, h)) return 0;

    // Paint a recognisable pattern: 4 quadrants (red, green, blue, grey)
    // + a 1-pixel-wide white border so we can visually verify width/height
    // are correct and byte order is what we think.
    uint32_t* px = (uint32_t*)s_fb_virt;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c;
            if (x == 0 || y == 0 || x == w-1 || y == h-1) c = 0x00FFFFFFu; // white
            else if (x < w/2 && y < h/2) c = 0x00FF0000u; // red
            else if (x >= w/2 && y < h/2) c = 0x0000FF00u; // green
            else if (x < w/2 && y >= h/2) c = 0x000000FFu; // blue
            else                           c = 0x00808080u; // grey
            px[y * w + x] = c;
        }
    }

    if (!virtio_gpu_transfer_to_host_2d(s_fb_res_id, w, h)) {
        kprintf("[virtio-gpu] TRANSFER_TO_HOST_2D failed\n"); return 0;
    }
    if (!virtio_gpu_resource_flush(s_fb_res_id, w, h)) {
        kprintf("[virtio-gpu] RESOURCE_FLUSH failed\n"); return 0;
    }
    kprintf("[virtio-gpu] present OK (%ux%u, resource %u, %u bytes)\n",
            w, h, s_fb_res_id, s_fb_bytes);
    return 1;
}

// ── drm_backend_ops adapters ─────────────────────────────────────────
// Thin wrappers that match the backend vtable signatures.  The DRM
// Restore the default scanout.  Two paths:
//   (1) If a kernel-owned banner resource was created via
//       vgpu_setup_scanout_buffer (present_test / early-boot fbcon),
//       point scanout at it + transfer+flush so the current backing
//       contents show up.
//   (2) Otherwise, disable scanout 0 with res_id=0 — per virtio-gpu
//       spec this "removes the scanout configuration" and on
//       virtio-vga reverts the host display to the VGA-compat
//       framebuffer, which mirrors the UEFI GOP memory our text
//       console (fb.c) writes into.  Without this, after a
//       compositor exits the hardware keeps scanning out the
//       compositor's (freed) framebuffer and the bash prompt is
//       invisible even though the TTY keeps receiving keypresses.
int virtio_gpu_restore_default_scanout(void) {
    // QEMU's virtio-vga does NOT fall back to the VGA-compat BAR when
    // SET_SCANOUT is called with resource_id=0 — it leaves the display
    // blanked ("display output not active").  So the fbcon resource
    // MUST already exist at this point: virtio_gpu_fbcon_init runs at
    // subsys boot and repoints g_fb.base_virt at its backing.
    if (!s_fb_res_id || !s_fb_w || !s_fb_h) return 0;
    if (!virtio_gpu_set_scanout(0, s_fb_res_id, s_fb_w, s_fb_h)) return 0;
    virtio_gpu_transfer_to_host_2d(s_fb_res_id, s_fb_w, s_fb_h);
    virtio_gpu_resource_flush(s_fb_res_id, s_fb_w, s_fb_h);
    return 1;
}

// ── fbcon-as-DRM-client wiring ───────────────────────────────────────
// Boot path: virtio_gpu_fbcon_init creates a 2D resource sized to
// scanout 0's preferred mode, attaches a physically-contiguous
// backing, sets scanout, and returns the backing phys+virt so main.c
// can re-call fb_init against it.  Text-console writes land in our
// backing; virtio_gpu_fbcon_flush pushes the backing to the host
// resource and flushes the scanout.
//
// Correctness notes:
//  * Reuses vgpu_setup_scanout_buffer's existing logic (which sets
//    s_fb_res_id / s_fb_phys / s_fb_virt / s_fb_w / s_fb_h).  The
//    DRM destroy path already calls virtio_gpu_restore_default_scanout
//    which points scanout back at s_fb_res_id — so once fbcon_init
//    runs, post-dwl-exit display restoration is automatic.
//  * When a DRM client (dwl) sets its own scanout, the fbcon backing
//    keeps being written by the TTY but isn't being scanned out, so
//    flushes are wasted.  We still issue them (cheap — µs range) to
//    keep the code trivially correct; a dirty-rect batched flush is
//    an optimisation for later once the text console is on a timer.
int virtio_gpu_fbcon_init(phys_addr_t* out_phys,
                           uint8_t**   out_virt,
                           uint32_t*   out_w,
                           uint32_t*   out_h,
                           uint32_t*   out_pitch) {
    if (!s_ok || !s_num_scanouts) return 0;
    uint32_t w = s_scanouts[0].w;
    uint32_t h = s_scanouts[0].h;
    if (!w || !h) return 0;
    if (!vgpu_setup_scanout_buffer(w, h)) return 0;

    if (out_phys)  *out_phys  = s_fb_phys;
    if (out_virt)  *out_virt  = s_fb_virt;
    if (out_w)     *out_w     = w;
    if (out_h)     *out_h     = h;
    if (out_pitch) *out_pitch = w * 4u;
    return 1;
}

void virtio_gpu_fbcon_flush(void) {
    if (!s_fb_res_id || !s_fb_w || !s_fb_h) return;
    virtio_gpu_transfer_to_host_2d(s_fb_res_id, s_fb_w, s_fb_h);
    virtio_gpu_resource_flush(s_fb_res_id, s_fb_w, s_fb_h);
}

// core calls these; legacy virtio_gpu_* functions are preserved as
// the adapter targets and for the present_test.

static uint32_t vgpu_be_scanout_count(void) { return virtio_gpu_num_scanouts(); }
static void     vgpu_be_scanout_mode(uint32_t idx, uint32_t* w, uint32_t* h) {
    virtio_gpu_get_mode(idx, w, h);
}
static int vgpu_be_resource_create(uint32_t id, uint32_t fmt, uint32_t w, uint32_t h) {
    return virtio_gpu_resource_create_2d(id, fmt, w, h) ? 0 : -1;
}
static int vgpu_be_resource_destroy(uint32_t id) {
    return virtio_gpu_resource_unref(id) ? 0 : -1;
}
static int vgpu_be_resource_attach(uint32_t id, phys_addr_t phys, uint32_t bytes) {
    return virtio_gpu_resource_attach_backing_single(id, phys, bytes) ? 0 : -1;
}
static int vgpu_be_scanout_set(uint32_t sc, uint32_t res, uint32_t w, uint32_t h) {
    return virtio_gpu_set_scanout(sc, res, w, h) ? 0 : -1;
}
static int vgpu_be_transfer(uint32_t id, uint32_t w, uint32_t h) {
    return virtio_gpu_transfer_to_host_2d(id, w, h) ? 0 : -1;
}
static int vgpu_be_flush(uint32_t id, uint32_t w, uint32_t h) {
    return virtio_gpu_resource_flush(id, w, h) ? 0 : -1;
}

static const drm_backend_ops_t vgpu_backend_ops = {
    .scanout_count           = vgpu_be_scanout_count,
    .scanout_mode            = vgpu_be_scanout_mode,
    .resource_create         = vgpu_be_resource_create,
    .resource_destroy        = vgpu_be_resource_destroy,
    .resource_attach_backing = vgpu_be_resource_attach,
    .scanout_set             = vgpu_be_scanout_set,
    .resource_transfer       = vgpu_be_transfer,
    .resource_flush          = vgpu_be_flush,
};

// drm_backend global pointer + registration — defined here so the
// backend owns its own storage.  drm.c reads it through __atomic_load
// (effectively one-time init; a future multi-backend world would use
// proper RCU).
const drm_backend_ops_t* drm_backend = NULL;

void drm_backend_register(const drm_backend_ops_t* ops) {
    __atomic_store_n(&drm_backend, ops, __ATOMIC_RELEASE);
}

// Hook: at init, after s_ok = 1 means the device is live, register
// ourselves as the backend.  Must be called AFTER virtio_gpu_init.
void virtio_gpu_register_backend(void) {
    if (s_ok) drm_backend_register(&vgpu_backend_ops);
}
