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
#include "errno.h"
#include "pci.h"
#include "virtio_pci.h"   // shared virtio-PCI transport: caps, status bits, common_cfg, find_virtio_cap
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "kprintf.h"
#include "log.h"
#include "trace.h"
#include "smp.h"
#include "common.h"
#include "checked.h"   // mul_within_u32: overflow-safe bounded multiply
#include "idt.h"       // idt_irq_register -- Phase 2b control-queue MSI-X
#include "lapic.h"     // VEC_VIRTIO_GPU, lapic_msi_addr / lapic_msi_data
#include "irq_wait.h"  // irq_wait / irq_notify -- the GPU fence wait queue

// ── PCI IDs ─────────────────────────────────────────────────────────
// Generic virtio-PCI transport constants (VIRTIO_VENDOR, VIRTIO_PCI_CAP_*,
// VIRTIO_STATUS_*, VIRTIO_F_VERSION_1) live in virtio_pci.h.
#define VIRTIO_DEV_GPU_MODERN    0x1050u   // virtio 1.x GPU device
#define VIRTIO_DEV_GPU_LEGACY    0x1040u   // transitional (also accepted)

// ── virtio-gpu feature bits (§5.7.3) ────────────────────────────────
#define VIRTIO_GPU_F_VIRGL             (1ULL << 0)   // 3D support (skip for now)
#define VIRTIO_GPU_F_EDID              (1ULL << 1)   // EDID blobs
#define VIRTIO_GPU_F_RESOURCE_UUID     (1ULL << 2)
#define VIRTIO_GPU_F_RESOURCE_BLOB     (1ULL << 3)
#define VIRTIO_GPU_F_CONTEXT_INIT      (1ULL << 4)

// VIRGL is requested but only NEGOTIATED when the host offers it (want =
// DRIVER_FEATURES & device_features).  A plain virtio-gpu-pci device never
// offers it, so the 2D path is unchanged; virtio-gpu-gl offers it and we then
// light up the 3D command layer.  s_virgl records the outcome.
#define DRIVER_FEATURES                (VIRTIO_F_VERSION_1 | VIRTIO_GPU_F_EDID | \
                                        VIRTIO_GPU_F_VIRGL)

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

// ── 3D (virgl) control commands (§5.7.6.8) -- Phase 1 (docs/VIRGL_BRINGUP.md) ─
#define VIRTIO_GPU_CMD_CTX_CREATE              0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY             0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE     0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE     0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D      0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D     0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D   0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D               0x0207

// ctrl_hdr.flags: request a fence; response arrives after the command retires.
#define VIRTIO_GPU_FLAG_FENCE                  (1u << 0)

#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO         0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET              0x1103
#define VIRTIO_GPU_RESP_OK_EDID                0x1104

#define VIRTIO_GPU_MAX_SCANOUTS                16

// virtio_pci_common_cfg_t (spec 4.1.4.3) lives in virtio_pci.h.

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

// Capset query (§5.7.6.7).  Read-only: reports which 3D command sets (virgl,
// virgl2, ...) the host offers.  capset_id: 1=VIRGL, 2=VIRGL2.  Used by the
// Phase 0 probe (docs/VIRGL_BRINGUP.md); no 3D command is issued.
typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t capset_index;
    uint32_t padding;
} virtio_gpu_get_capset_info_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} virtio_gpu_resp_capset_info_t;

// ── Driver state ────────────────────────────────────────────────────
static int s_ok = 0;
static int s_virgl = 0;   // 1 if VIRTIO_GPU_F_VIRGL negotiated (host has 3D)
static void virtio_gpu_virgl_selftest(void);        // Phase 1a: 3D data path
static void virtio_gpu_sg_backing_selftest(void);   // Phase 1e: SG multi-entry backing
static void virtio_gpu_virgl_clear_selftest(void);  // Phase 1b: GPU render (clear)

// ── GPU fence -- Phase 2b (docs/VIRGL_BRINGUP.md) ───────────────────────────
// A monotonic completion counter, NOT Linux's per-fence objects + pending list
// + signalling workqueue.  s_gpu_fence_submit counts commands published to the
// control queue; s_gpu_fence_done counts completions.  On the synchronous path
// they stay equal (one command outstanding under s_ctrl_lock), so a fence WAIT
// is satisfied immediately -- the counter + control-queue MSI-X + wait queue are
// the O(1) mechanism a future async-submit path (async host GL) would ride.
extern void virtio_gpu_irq_entry(void);
uint8_t g_virtio_gpu_irq = 0xFFu;               // logical irq_wait slot; set at init
static volatile uint64_t s_gpu_fence_submit = 0;
static volatile uint64_t s_gpu_fence_done   = 0;
static volatile uint64_t s_gpu_irq_count    = 0;   // diagnostic: MSI-X deliveries

// Control-queue MSI-X handler.  It deliberately does NOT drain the used ring
// (vgpu_send_ctrl owns that under s_ctrl_lock; draining here would race it) --
// it only wakes fence waiters.  So a misconfigured MSI-X can never stall a
// command: vgpu_send_ctrl's poll stays authoritative and this IRQ is a pure
// wakeup.  Called from irq_stubs.asm after LAPIC EOI.
void virtio_gpu_irq_handler(void) {
    s_gpu_irq_count++;
    if (g_virtio_gpu_irq != 0xFFu) irq_notify(g_virtio_gpu_irq);
}
static pci_device_t                         s_dev;
static volatile virtio_pci_common_cfg_t*    s_common      = NULL;
static volatile uint8_t*                    s_notify      = NULL;
static volatile virtio_gpu_cfg_t*           s_devcfg      = NULL;
static uint32_t                             s_notify_mult = 0;

static virtq_t  s_ctrl_vq;   // VQ_CONTROLQ
static virtq_t  s_cursor_vq; // VQ_CURSORQ (allocated but unused for now)

static uint32_t s_num_scanouts = 0;
static struct { uint32_t w, h, enabled; } s_scanouts[VIRTIO_GPU_MAX_SCANOUTS];

// vcap_t + find_virtio_cap (the PCI capability walker) live in virtio_pci.h.

// ── Virtqueue allocation + activation ───────────────────────────────
static int virtq_alloc(virtq_t* vq) {
    vq->desc_phys = pmm_buddy_alloc(0);
    if (!PMM_ALLOC_OK(vq->desc_phys)) return 0;
    vq->desc = (virtq_desc_t*)((uintptr_t)vq->desc_phys + HHDM_OFFSET);
    __builtin_memset(vq->desc, 0, VIRTQ_SIZE * sizeof(virtq_desc_t));

    vq->avail_phys = pmm_buddy_alloc(0);
    if (!PMM_ALLOC_OK(vq->avail_phys)) goto free_desc;   // else desc_phys leaks
    vq->avail = (virtq_avail_t*)((uintptr_t)vq->avail_phys + HHDM_OFFSET);
    __builtin_memset(vq->avail, 0, sizeof(virtq_avail_t));

    vq->used_phys = pmm_buddy_alloc(0);
    if (!PMM_ALLOC_OK(vq->used_phys)) goto free_avail;   // else desc_phys + avail_phys leak
    vq->used = (virtq_used_t*)((uintptr_t)vq->used_phys + HHDM_OFFSET);
    __builtin_memset(vq->used, 0, sizeof(virtq_used_t));

    for (uint16_t i = 0; i < VIRTQ_SIZE - 1; i++)
        vq->desc[i].next = (uint16_t)(i + 1u);
    vq->desc[VIRTQ_SIZE - 1].next = 0xFFFFu;
    vq->free_head     = 0;
    vq->avail_idx     = 0;
    vq->last_used_idx = 0;
    return 1;

free_avail:
    pmm_buddy_free(vq->avail_phys, 0);
free_desc:
    pmm_buddy_free(vq->desc_phys, 0);
    return 0;
}

static void virtq_activate(volatile virtio_pci_common_cfg_t* cfg,
                            uint16_t qidx, virtq_t* vq) {
    cfg->queue_select = qidx;
    __asm__ volatile("mfence" ::: "memory");
    vq->notify_off      = cfg->queue_notify_off;
    // Negotiate the queue size (virtio 1.0 §4.1.5.1.3).  The device reports
    // its MAX in queue_size; the driver may write a smaller power-of-two.  We
    // allocate fixed VIRTQ_SIZE-entry rings, so the device MUST use exactly
    // VIRTQ_SIZE -- otherwise it indexes avail->ring[head % dev_size] while we
    // fill avail->ring[head % VIRTQ_SIZE]; those agree only while head <
    // min(sizes), so a device default of 256 works for the first 64 commands
    // and then reads slots we never wrote -> garbage descriptor head ->
    // DEVICE_NEEDS_RESET.  Writing queue_size here is mandatory, not optional.
    uint16_t dev_max = cfg->queue_size;
    if (dev_max < VIRTQ_SIZE) {
        // Our rings are larger than the device supports; can't run this queue
        // safely.  (No real virtio-gpu reports < 64; guarded so a future
        // device change fails loud instead of corrupting the ring.)
        pr_warn("virtio-gpu", "queue %u max size %u < VIRTQ_SIZE %u",
                qidx, dev_max, (unsigned)VIRTQ_SIZE);
    }
    cfg->queue_size = VIRTQ_SIZE;
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

// ── Send one command and poll for the response ─────────────────────
// We use a bounce buffer pair (request + response) allocated in
// physically-contiguous pages.  The GPU queue is idle most of the
// time so single-outstanding-request is fine; concurrency comes
// later with the DRM ioctl layer.  Returns 1 on success.
static phys_addr_t s_cmd_phys = 0;
static uint8_t*    s_cmd_virt = NULL;   // request at +0, response at +CMD_RESP_OFF

// The control bounce buffer is 128 KiB: a 64 KiB request window followed by a
// 64 KiB response window.  Mesa's virgl submits command streams and reads capset
// blobs much larger than the old single-page (2 KiB req) buffer allowed.
#define CMD_BUF_ORDER 5u                     // 2^5 = 32 pages = 128 KiB
#define CMD_BUF_SIZE  (32u * 4096u)          // 131072
#define CMD_REQ_OFF   0u
#define CMD_RESP_OFF  (CMD_BUF_SIZE / 2u)    // 65536: req [0,64K), resp [64K,128K)

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
    if (req_len  > CMD_RESP_OFF)                return 0;
    if (resp_len > (CMD_BUF_SIZE - CMD_RESP_OFF)) return 0;

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
    __atomic_add_fetch(&s_gpu_fence_submit, 1, __ATOMIC_ACQ_REL);   // fence: submitted
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
            // Fence: this command retired.  Wake any WAITer directly (the MSI-X
            // handler also wakes, but the poll is authoritative so completion is
            // never lost if the IRQ is slow/misconfigured).
            __atomic_add_fetch(&s_gpu_fence_done, 1, __ATOMIC_ACQ_REL);
            if (g_virtio_gpu_irq != 0xFFu) irq_notify(g_virtio_gpu_irq);
            spin_unlock(&s_ctrl_lock);
            return 1;
        }
    }
    // Timeout → leak descriptors (safer than racing the device).  Log loudly
    // with the device status: a SILENT return here is exactly what let a
    // queue-size mismatch (device DEVICE_NEEDS_RESET, status bit 0x40) wedge
    // the control queue undiagnosed.  This path should never fire in normal
    // operation now that queue geometry is negotiated in virtq_activate().
    pr_warn("virtio-gpu", "ctrl timeout reqtype=0x%x len=%u status=0x%x",
            ((virtio_gpu_ctrl_hdr_t*)s_cmd_virt)->type, req_len,
            (unsigned)s_common->device_status);
    spin_unlock(&s_ctrl_lock);
    return 0;
}

// ── Cursor queue (§5.7.6.10) ─────────────────────────────────────────
// UPDATE_CURSOR / MOVE_CURSOR go on the dedicated cursor virtqueue so
// pointer motion never queues behind bulk control-queue transfers.
// Own bounce page + own lock: the ctrl path's response window may span
// its whole page, and mouse-move frequency must not contend with
// scanout flushes.

#define VIRTIO_GPU_CMD_UPDATE_CURSOR  0x0300u
#define VIRTIO_GPU_CMD_MOVE_CURSOR    0x0301u

typedef struct __attribute__((packed)) {
    uint32_t scanout_id;
    uint32_t x, y;
    uint32_t padding;
} virtio_gpu_cursor_pos_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t   hdr;       // UPDATE_CURSOR or MOVE_CURSOR
    virtio_gpu_cursor_pos_t pos;
    uint32_t resource_id;              // UPDATE only; 0 hides the cursor
    uint32_t hot_x, hot_y;             // UPDATE only
    uint32_t padding;
} virtio_gpu_update_cursor_t;

static phys_addr_t s_cursor_phys = 0;
static uint8_t*    s_cursor_virt = NULL;   // request at +0, response at +2048
static spinlock_t  s_cursor_lock = SPINLOCK_INIT;

static int vgpu_send_cursor(const virtio_gpu_update_cursor_t* req) {
    if (!s_cursor_virt) return 0;

    spin_lock(&s_cursor_lock);
    __builtin_memcpy(s_cursor_virt, req, sizeof(*req));
    __builtin_memset(s_cursor_virt + 2048, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    virtq_t* vq = &s_cursor_vq;
    uint16_t d0 = vq->free_head;
    if (d0 == 0xFFFFu) { spin_unlock(&s_cursor_lock); return 0; }
    uint16_t d1 = vq->desc[d0].next;
    if (d1 == 0xFFFFu) { spin_unlock(&s_cursor_lock); return 0; }
    vq->free_head = vq->desc[d1].next;

    vq->desc[d0].addr  = (uint64_t)s_cursor_phys;
    vq->desc[d0].len   = sizeof(*req);
    vq->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[d0].next  = d1;

    vq->desc[d1].addr  = (uint64_t)(s_cursor_phys + 2048);
    vq->desc[d1].len   = sizeof(virtio_gpu_ctrl_hdr_t);
    vq->desc[d1].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[d1].next  = 0;

    uint16_t slot = vq->avail_idx % VIRTQ_SIZE;
    vq->avail->ring[slot] = d0;
    __asm__ volatile("mfence" ::: "memory");
    vq->avail_idx++;
    vq->avail->idx = vq->avail_idx;
    __asm__ volatile("mfence" ::: "memory");

    *(volatile uint16_t*)(s_notify + vq->notify_off * s_notify_mult) = VQ_CURSORQ;
    __asm__ volatile("mfence" ::: "memory");

    // Poll for completion (QEMU answers in microseconds; same bounded
    // poll as the control queue until we go interrupt-driven).
    for (int i = 0; i < 10000000; i++) {
        uint16_t used_idx = vq->used->idx;
        __asm__ volatile("lfence" ::: "memory");
        if (used_idx != vq->last_used_idx) {
            vq->last_used_idx = used_idx;
            vq->desc[d1].next = vq->free_head;
            vq->desc[d0].next = d1;
            vq->free_head = d0;
            spin_unlock(&s_cursor_lock);
            return 1;
        }
    }
    spin_unlock(&s_cursor_lock);
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
    s_virgl = (want & VIRTIO_GPU_F_VIRGL) != 0;   // 3D lit up only if host offers it

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

    // Command bounce buffer (128 KiB: 64K request + 64K response window).
    s_cmd_phys = pmm_buddy_alloc(CMD_BUF_ORDER);
    if (!PMM_ALLOC_OK(s_cmd_phys)) return 0;
    s_cmd_virt = (uint8_t*)((uintptr_t)s_cmd_phys + HHDM_OFFSET);

    // Cursor-queue bounce buffer (separate page — see vgpu_send_cursor).
    s_cursor_phys = pmm_buddy_alloc(0);
    if (!PMM_ALLOC_OK(s_cursor_phys)) return 0;
    s_cursor_virt = (uint8_t*)((uintptr_t)s_cursor_phys + HHDM_OFFSET);

    s_common->device_status = VIRTIO_STATUS_ACKNOWLEDGE |
                              VIRTIO_STATUS_DRIVER |
                              VIRTIO_STATUS_FEATURES_OK |
                              VIRTIO_STATUS_DRIVER_OK;
    __asm__ volatile("mfence" ::: "memory");

    // ── Control-queue MSI-X (Phase 2b) ───────────────────────────────────
    // Wire an interrupt for control-queue completions so a GPU fence WAIT can
    // sleep instead of spin.  Additive + SAFE: vgpu_send_ctrl's poll stays
    // authoritative, so a failed setup here degrades to poll-only, never a
    // stall.  Mirrors the virtio-net MSI-X path.
    {
        uint8_t msix_cap = pci_find_cap(dev.bus, dev.dev, dev.fn, 0x11u);
        if (msix_cap) {
            uint32_t tbl_dw  = pci_cfg_read32(dev.bus, dev.dev, dev.fn, msix_cap + 4u);
            uint32_t bir     = tbl_dw & 0x7u;
            uint32_t tbl_off = tbl_dw & ~0x7u;
            uint64_t bar_phys = pci_bar_base(dev.bus, dev.dev, dev.fn, (uint8_t)bir);
            volatile uint32_t* msix_table =
                (volatile uint32_t*)vmm_map_mmio(bar_phys + tbl_off, 0x1000u);
            if (msix_table) {
                msix_table[0] = (uint32_t)lapic_msi_addr();      // addr_lo
                msix_table[1] = 0;                                // addr_hi
                msix_table[2] = lapic_msi_data(VEC_VIRTIO_GPU);   // data
                msix_table[3] = 0;                                // vector_ctrl: unmask
                uint32_t mc = pci_cfg_read32(dev.bus, dev.dev, dev.fn, msix_cap);
                mc = (mc | (1u << 31)) & ~(1u << 30);             // enable MSI-X, clear fn-mask
                pci_cfg_write32(dev.bus, dev.dev, dev.fn, msix_cap, mc);
                s_common->queue_select      = VQ_CONTROLQ;
                s_common->queue_msix_vector = 0;                  // control queue -> vector 0
                __asm__ volatile("mfence" ::: "memory");
                g_virtio_gpu_irq = 5u;   // logical irq_wait slot (net=4, gpu=5)
                idt_irq_register(VEC_VIRTIO_GPU, (uint64_t)virtio_gpu_irq_entry);
                kprintf("[virtio-gpu] control-queue MSI-X on vector 0x%x\n", VEC_VIRTIO_GPU);
            }
        }
        // No MSI-X / map fail -> g_virtio_gpu_irq stays 0xFF; the fence path
        // falls back to the poll (synchronous, always correct).
    }

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

    // ── Phase 0 virgl probe (read-only; does NOT enable 3D) ──────────────
    // Report whether the host offers 3D (virgl) and enumerate its capsets so
    // the virgl bring-up (docs/VIRGL_BRINGUP.md) knows what the host supports.
    // We do NOT negotiate VIRTIO_GPU_F_VIRGL and issue NO 3D command -- the 2D
    // path is byte-identical.  GET_CAPSET_INFO is a plain query available
    // whenever the device advertises capsets (num_capsets > 0, set by the host
    // when launched with virtio-gpu-gl + virglrenderer).
    {
        int virgl_offered = (dfeat & VIRTIO_GPU_F_VIRGL) != 0;
        uint32_t ncaps = s_devcfg->num_capsets;
        kprintf("[virtio-gpu] virgl(3D) offered by host: %s; num_capsets=%u\n",
                virgl_offered ? "yes" : "no", ncaps);
        for (uint32_t ci = 0; ci < ncaps; ci++) {
            virtio_gpu_get_capset_info_t creq = {
                .hdr.type     = VIRTIO_GPU_CMD_GET_CAPSET_INFO,
                .capset_index = ci,
            };
            virtio_gpu_resp_capset_info_t cresp;
            if (!vgpu_send_ctrl(&creq, sizeof(creq), &cresp, sizeof(cresp))) {
                kprintf("[virtio-gpu] GET_CAPSET_INFO[%u] timeout\n", ci);
                continue;
            }
            if (cresp.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO) {
                kprintf("[virtio-gpu] GET_CAPSET_INFO[%u] bad resp=%x\n",
                        ci, cresp.hdr.type);
                continue;
            }
            kprintf("[virtio-gpu] capset[%u]: id=%u max_version=%u max_size=%u\n",
                    ci, cresp.capset_id, cresp.capset_max_version,
                    cresp.capset_max_size);
        }
    }

    s_ok = 1;
    kprintf("[virtio-gpu] initialised (%u scanouts)\n", s_num_scanouts);
    if (s_virgl) {
        virtio_gpu_virgl_selftest();        // Phase 1a: 3D resource data path
        virtio_gpu_sg_backing_selftest();   // Phase 1e: scatter-gather backing
        virtio_gpu_virgl_clear_selftest();  // Phase 1b: GPU renders (clear)
    }
    // Phase 2b diagnostic: after the boot-time control commands, report how many
    // control-queue MSI-X interrupts were delivered (0 = IRQ not firing, running
    // poll-only; >0 = the fence wake path is live).
    kprintf("[virtio-gpu] control-queue IRQs delivered: %lu\n",
            (unsigned long)s_gpu_irq_count);
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

// Like vgpu_send_ctrl, but splices a caller-provided device-READABLE data
// segment (data_phys, data_len) between the request header and the response.
// The virtqueue concatenates the header and data descriptors into one logical
// device-read buffer, so header||data reproduces a single wire command whose
// tail (e.g. an ATTACH_BACKING mem_entry array) can be arbitrarily large -- it
// is NOT bounded by the 64 KiB request window an inlined request would face.
static int vgpu_send_ctrl_data(const void* req, uint32_t req_len,
                               phys_addr_t data_phys, uint32_t data_len,
                               void* resp, uint32_t resp_len) {
    if (!s_cmd_virt) return 0;
    if (req_len  > CMD_RESP_OFF)                  return 0;
    if (resp_len > (CMD_BUF_SIZE - CMD_RESP_OFF)) return 0;

    spin_lock(&s_ctrl_lock);
    __builtin_memcpy(s_cmd_virt + CMD_REQ_OFF, req, req_len);
    __builtin_memset(s_cmd_virt + CMD_RESP_OFF, 0, resp_len);

    // Build a 3-descriptor chain: header (device-read) + data (device-read) +
    // response (device-write).  Allocated from the free list, same as the
    // 2-descriptor vgpu_send_ctrl path.
    virtq_t* vq = &s_ctrl_vq;
    uint16_t d0 = vq->free_head;
    if (d0 == 0xFFFFu) { spin_unlock(&s_ctrl_lock); return 0; }
    uint16_t d1 = vq->desc[d0].next;
    if (d1 == 0xFFFFu) { spin_unlock(&s_ctrl_lock); return 0; }
    uint16_t d2 = vq->desc[d1].next;
    if (d2 == 0xFFFFu) { spin_unlock(&s_ctrl_lock); return 0; }
    vq->free_head = vq->desc[d2].next;

    vq->desc[d0].addr  = (uint64_t)(s_cmd_phys + CMD_REQ_OFF);
    vq->desc[d0].len   = req_len;
    vq->desc[d0].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[d0].next  = d1;

    vq->desc[d1].addr  = (uint64_t)data_phys;
    vq->desc[d1].len   = data_len;
    vq->desc[d1].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[d1].next  = d2;

    vq->desc[d2].addr  = (uint64_t)(s_cmd_phys + CMD_RESP_OFF);
    vq->desc[d2].len   = resp_len;
    vq->desc[d2].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[d2].next  = 0;

    uint16_t slot = vq->avail_idx % VIRTQ_SIZE;
    vq->avail->ring[slot] = d0;
    __asm__ volatile("mfence" ::: "memory");
    vq->avail_idx++;
    vq->avail->idx = vq->avail_idx;
    __atomic_add_fetch(&s_gpu_fence_submit, 1, __ATOMIC_ACQ_REL);
    __asm__ volatile("mfence" ::: "memory");

    *(volatile uint16_t*)(s_notify + vq->notify_off * s_notify_mult) = VQ_CONTROLQ;
    __asm__ volatile("mfence" ::: "memory");

    for (int i = 0; i < 10000000; i++) {
        uint16_t used_idx = vq->used->idx;
        __asm__ volatile("lfence" ::: "memory");
        if (used_idx != vq->last_used_idx) {
            vq->last_used_idx = used_idx;
            __builtin_memcpy(resp, s_cmd_virt + CMD_RESP_OFF, resp_len);
            // Return all three descriptors to the free list.
            vq->desc[d2].next = vq->free_head;
            vq->desc[d1].next = d2;
            vq->desc[d0].next = d1;
            vq->free_head = d0;
            __atomic_add_fetch(&s_gpu_fence_done, 1, __ATOMIC_ACQ_REL);
            if (g_virtio_gpu_irq != 0xFFu) irq_notify(g_virtio_gpu_irq);
            spin_unlock(&s_ctrl_lock);
            return 1;
        }
    }
    pr_warn("virtio-gpu", "ctrl(data) timeout reqtype=0x%x datalen=%u status=0x%x",
            ((virtio_gpu_ctrl_hdr_t*)s_cmd_virt)->type, data_len,
            (unsigned)s_common->device_status);
    spin_unlock(&s_ctrl_lock);
    return 0;
}

int virtio_gpu_resource_attach_backing_sg(uint32_t res_id,
                                          const vgpu_sg_run_t* runs,
                                          uint32_t nruns) {
    if (!runs || nruns == 0) return 0;
    // Build the mem_entry array in a transient device-readable buffer -- one
    // entry per contiguous run.  The caller coalesces adjacent pages, so nruns
    // is small for unfragmented memory and, worst case, equals the page count.
    // kmalloc'd memory is HHDM-linear, so its physical address is virt-HHDM and
    // the buffer is physically contiguous (single descriptor).
    uint32_t nbytes = nruns * (uint32_t)sizeof(vgpu_mem_entry_t);
    vgpu_mem_entry_t* ents = (vgpu_mem_entry_t*)kmalloc(nbytes);
    if (!ents) return 0;
    for (uint32_t i = 0; i < nruns; i++) {
        ents[i].addr    = (uint64_t)runs[i].addr;
        ents[i].length  = runs[i].npages * 4096u;
        ents[i].padding = 0;
    }
    __asm__ volatile("mfence" ::: "memory");   // publish entries before the device reads them

    vgpu_attach_backing_t hdr = {0};
    hdr.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    hdr.resource_id = res_id;
    hdr.nr_entries  = nruns;
    phys_addr_t ents_phys = (phys_addr_t)((uintptr_t)ents - HHDM_OFFSET);
    virtio_gpu_ctrl_hdr_t resp = {0};
    int ok = vgpu_send_ctrl_data(&hdr, sizeof(hdr), ents_phys, nbytes,
                                 &resp, sizeof(resp));
    kfree(ents);
    return ok && resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

int virtio_gpu_resource_attach_backing_single(uint32_t res_id,
                                               phys_addr_t phys, uint32_t len) {
    // One contiguous range is just a one-run scatter-gather list.
    vgpu_sg_run_t run = { .addr = phys, .npages = (len + 4095u) / 4096u };
    return virtio_gpu_resource_attach_backing_sg(res_id, &run, 1);
}

// ── 3D (virgl) command layer -- Phase 1 (docs/VIRGL_BRINGUP.md) ──────────────
// Thin wrappers over the same synchronous vgpu_send_ctrl the 2D path uses: the
// control queue completes commands in FIFO order, so a later command's response
// proves every earlier one retired -- no fence object, no fence list, no
// completion workqueue (Linux carries all three).  The async fence path
// (VIRTIO_GPU_FLAG_FENCE + an O(1) monotonic completion counter woken from the
// control-queue IRQ) lands in Phase 2, where userland EXECBUFFER/WAIT needs it.

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;      // hdr.ctx_id = the context being created
    uint32_t nlen;
    uint32_t context_init;          // 0 = default virgl context
    char     debug_name[64];
} vgpu_ctx_create_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;      // hdr.ctx_id
} vgpu_ctx_only_t;                  // CTX_DESTROY

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;      // hdr.ctx_id
    uint32_t resource_id;
    uint32_t padding;
} vgpu_ctx_resource_t;              // CTX_ATTACH_RESOURCE / CTX_DETACH_RESOURCE

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t target;               // pipe_texture_target (PIPE_TEXTURE_2D = 2)
    uint32_t format;               // VIRGL_FORMAT_* (B8G8R8A8_UNORM = 1)
    uint32_t bind;                 // VIRGL_BIND_* mask
    uint32_t width, height, depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} vgpu_resource_create_3d_t;

typedef struct __attribute__((packed)) {
    uint32_t x, y, z, w, h, d;
} vgpu_box_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;      // hdr.ctx_id
    vgpu_box_t box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} vgpu_transfer_host_3d_t;

// virgl constants (virglrenderer virgl_hw.h + gallium p_defines.h).
#define VIRGL_FORMAT_B8G8R8A8_UNORM   1u
#define PIPE_TEXTURE_2D               2u
#define VIRGL_BIND_RENDER_TARGET      (1u << 1)   // 2
#define VIRGL_BIND_SAMPLER_VIEW       (1u << 3)   // 8

static int vgpu_ctx_create(uint32_t ctx_id, uint32_t context_init) {
    vgpu_ctx_create_t req = {0};
    req.hdr.type      = VIRTIO_GPU_CMD_CTX_CREATE;
    req.hdr.ctx_id    = ctx_id;
    req.context_init  = context_init;   // 0 = default virgl context; else capset id
    static const char nm[] = "makaos-virgl";
    uint32_t n = 0;
    while (nm[n] && n < sizeof(req.debug_name) - 1) { req.debug_name[n] = nm[n]; n++; }
    req.nlen = n;
    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) return 0;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

static int vgpu_ctx_destroy(uint32_t ctx_id) {
    vgpu_ctx_only_t req = {0};
    req.hdr.type   = VIRTIO_GPU_CMD_CTX_DESTROY;
    req.hdr.ctx_id = ctx_id;
    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) return 0;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

static int vgpu_ctx_attach_resource(uint32_t ctx_id, uint32_t res_id) {
    vgpu_ctx_resource_t req = {0};
    req.hdr.type    = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    req.hdr.ctx_id  = ctx_id;
    req.resource_id = res_id;
    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) return 0;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

static int vgpu_resource_create_3d(uint32_t res_id, uint32_t target, uint32_t format,
                                   uint32_t bind, uint32_t w, uint32_t h, uint32_t depth,
                                   uint32_t array_size, uint32_t last_level, uint32_t nr_samples) {
    vgpu_resource_create_3d_t req = {0};
    req.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    req.resource_id = res_id;
    req.target      = target;
    req.format      = format;
    req.bind        = bind;
    req.width       = w;
    req.height      = h;
    req.depth       = depth ? depth : 1u;
    // array_size is the layer count -- 6 for a cube map, N*6 for a cube array,
    // else the array length.  Hardcoding 1 (as before) made the host reject
    // every cube map ("unexpected array size 1"), so thread the real value.
    req.array_size  = array_size ? array_size : 1u;
    req.last_level  = last_level;
    req.nr_samples  = nr_samples;
    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) return 0;
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        pr_warn("virtio-gpu",
                "RESOURCE_CREATE_3D res=%u tgt=%u fmt=%u bind=0x%x %ux%ux%u arr=%u ll=%u ns=%u -> host resp 0x%x",
                res_id, target, format, bind, w, h, depth, array_size, last_level, nr_samples, resp.type);
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

// TRANSFER_TO_HOST_3D (cmd) or TRANSFER_FROM_HOST_3D: copy the box
// [x,y .. x+w,y+h] between the guest backing (at byte `offset`) and the host
// resource at mip `level`.  stride/layer_stride 0 => tightly packed (host
// computes them).
static int vgpu_transfer_3d(uint32_t cmd, uint32_t ctx_id, uint32_t res_id,
                            uint32_t x, uint32_t y, uint32_t z,
                            uint32_t w, uint32_t h, uint32_t d,
                            uint64_t offset, uint32_t level,
                            uint32_t stride, uint32_t layer_stride) {
    vgpu_transfer_host_3d_t req = {0};
    req.hdr.type     = cmd;
    req.hdr.ctx_id   = ctx_id;
    req.box.x        = x;
    req.box.y        = y;
    req.box.z        = z;              // 3D depth slice / array layer / cube face
    req.box.w        = w;
    req.box.h        = h;
    req.box.d        = d ? d : 1;      // guard 0 -> at least one slice
    req.offset       = offset;
    req.level        = level;
    req.stride       = stride;         // guest row pitch; 0 => host derives box.w*bpp
    req.layer_stride = layer_stride;   // guest layer pitch; 0 => host derives
    req.resource_id  = res_id;
    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!vgpu_send_ctrl(&req, sizeof(req), &resp, sizeof(resp))) return 0;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;      // hdr.ctx_id
    uint32_t size;                  // bytes of virgl command stream that follow
    uint32_t padding;
} vgpu_cmd_submit_t;

// virgl command-stream encoding (virglrenderer virgl_protocol.h).  Each command
// starts with a dword: cmd | (obj_type << 8) | (payload_dwords << 16).
#define VIRGL_CMD0(cmd, obj, len)         ((uint32_t)((cmd) | ((obj) << 8) | ((len) << 16)))
#define VIRGL_CCMD_CREATE_OBJECT          1u
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE  5u
#define VIRGL_CCMD_CLEAR                  7u
#define VIRGL_OBJECT_SURFACE              8u
#define PIPE_CLEAR_COLOR0                 (1u << 2)   // gallium p_defines.h == 4

// SUBMIT_3D: hand a virgl command stream (ndwords dwords) to the host context.
// The request is the submit header immediately followed by the command bytes,
// bounded by the request window (minus the submit header).  Built in a heap
// buffer, not on the stack: virgl command streams reach tens of KiB.
#define VGPU_SUBMIT_MAX (CMD_RESP_OFF - sizeof(vgpu_cmd_submit_t))
static int vgpu_submit_3d(uint32_t ctx_id, const uint32_t* cmd, uint32_t ndwords) {
    const uint32_t clen = ndwords * 4u;
    if (clen > VGPU_SUBMIT_MAX) return 0;
    uint8_t* buf = (uint8_t*)kmalloc(sizeof(vgpu_cmd_submit_t) + clen);
    if (!buf) return 0;
    vgpu_cmd_submit_t* s = (vgpu_cmd_submit_t*)buf;
    __builtin_memset(s, 0, sizeof(*s));
    s->hdr.type   = VIRTIO_GPU_CMD_SUBMIT_3D;
    s->hdr.ctx_id = ctx_id;
    s->size       = clen;
    __builtin_memcpy(buf + sizeof(vgpu_cmd_submit_t), cmd, clen);
    virtio_gpu_ctrl_hdr_t resp = {0};
    int ok = vgpu_send_ctrl(buf, sizeof(vgpu_cmd_submit_t) + clen, &resp, sizeof(resp));
    kfree(buf);
    return ok && resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

// ── virgl self-test (Phase 1a: 3D command layer + guest<->host data path) ────
// Proves the 3D control commands and BOTH transfer directions work end to end,
// with no GPU rendering yet: create a context + a 3D BGRA texture, upload a
// distinctive pattern (TRANSFER_TO_HOST_3D), wipe the guest backing so a stale
// copy cannot masquerade as success, download it back (TRANSFER_FROM_HOST_3D),
// and verify every pixel round-trips.  Runs once at init when VIRGL negotiated.
// Fixed handles (a real id allocator is a Phase 2 concern once userland creates
// resources); cleans up the context + resource + page afterwards.
// ── Scatter-gather backing self-test (Phase 1e) ──────────────────────────────
// Proves the multi-entry ATTACH_BACKING path: back a 3D texture with pages that
// are DELIBERATELY scattered (two separate buddy blocks, nruns=2 -> a chained
// request descriptor, two device mem_entries), upload guest pixels, wipe the
// guest side, read them back, and confirm every pixel survived the round-trip
// across the scattered backing.  A FAIL here means the device did not honour the
// SG list; a PASS means scattered backing is safe to use for real resources.
#define VIRGL_SG_CTX   0xF010u
#define VIRGL_SG_RES   0xF011u
#define VIRGL_SG_W     64u
#define VIRGL_SG_H     64u

static void virtio_gpu_sg_backing_selftest(void) {
    const uint32_t npix   = VIRGL_SG_W * VIRGL_SG_H;   // 4096 px
    const uint32_t bytes  = npix * 4u;                 // 16 KiB = 4 pages
    // Two runs of 2 pages each (8 KiB), allocated separately so they are almost
    // certainly NOT physically adjacent -- exactly the fragmented case.
    const uint32_t run_pages = 2u;
    const uint32_t px_per_run = run_pages * 4096u / 4u;  // 2048 px per 8 KiB run
    phys_addr_t p0 = pmm_buddy_alloc(1);
    phys_addr_t p1 = pmm_buddy_alloc(1);
    if (!PMM_ALLOC_OK(p0) || !PMM_ALLOC_OK(p1)) {
        kprintf("[virtio-gpu] SG backing selftest: no mem\n");
        if (PMM_ALLOC_OK(p0)) pmm_buddy_free(p0, 1);
        if (PMM_ALLOC_OK(p1)) pmm_buddy_free(p1, 1);
        return;
    }
    volatile uint32_t* b0 = (volatile uint32_t*)((uintptr_t)p0 + HHDM_OFFSET);
    volatile uint32_t* b1 = (volatile uint32_t*)((uintptr_t)p1 + HHDM_OFFSET);
    // Pixel i lives in run 0 (i < px_per_run) or run 1.  Unique value per pixel.
    for (uint32_t i = 0; i < npix; i++) {
        uint32_t v = 0xFF000000u | i;
        if (i < px_per_run) b0[i] = v; else b1[i - px_per_run] = v;
    }
    __asm__ volatile("mfence" ::: "memory");

    vgpu_sg_run_t runs[2] = { { p0, run_pages }, { p1, run_pages } };
    (void)bytes;
    int ok = vgpu_ctx_create(VIRGL_SG_CTX, 0)
          && vgpu_resource_create_3d(VIRGL_SG_RES, PIPE_TEXTURE_2D,
                                     VIRGL_FORMAT_B8G8R8A8_UNORM,
                                     VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                                     VIRGL_SG_W, VIRGL_SG_H, 1, 1, 0, 0)
          && virtio_gpu_resource_attach_backing_sg(VIRGL_SG_RES, runs, 2)
          && vgpu_ctx_attach_resource(VIRGL_SG_CTX, VIRGL_SG_RES);
    if (!ok) { kprintf("[virtio-gpu] SG backing selftest: setup FAILED\n"); goto out; }

    if (!vgpu_transfer_3d(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, VIRGL_SG_CTX,
                          VIRGL_SG_RES, 0, 0, 0, VIRGL_SG_W, VIRGL_SG_H, 1, 0, 0, 0, 0)) {
        kprintf("[virtio-gpu] SG backing selftest: TO_HOST FAILED\n"); goto out;
    }
    for (uint32_t i = 0; i < px_per_run; i++) { b0[i] = 0xDEADBEEFu; b1[i] = 0xDEADBEEFu; }
    __asm__ volatile("mfence" ::: "memory");
    if (!vgpu_transfer_3d(VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, VIRGL_SG_CTX,
                          VIRGL_SG_RES, 0, 0, 0, VIRGL_SG_W, VIRGL_SG_H, 1, 0, 0, 0, 0)) {
        kprintf("[virtio-gpu] SG backing selftest: FROM_HOST FAILED\n"); goto out;
    }
    __asm__ volatile("mfence" ::: "memory");

    uint32_t bad = 0;
    for (uint32_t i = 0; i < npix; i++) {
        uint32_t want = 0xFF000000u | i;
        uint32_t got  = (i < px_per_run) ? b0[i] : b1[i - px_per_run];
        if (got != want) bad++;
    }
    kprintf("[virtio-gpu] SG backing selftest: %s (2 runs, %ux%u, %u/%u px wrong)\n",
            bad ? "FAIL" : "PASS", VIRGL_SG_W, VIRGL_SG_H, bad, npix);
out:
    virtio_gpu_resource_unref(VIRGL_SG_RES);
    vgpu_ctx_destroy(VIRGL_SG_CTX);
    pmm_buddy_free(p0, 1);
    pmm_buddy_free(p1, 1);
}

#define VIRGL_TEST_CTX   0xF000u
#define VIRGL_TEST_RES   0xF001u
#define VIRGL_TEST_W     128u
#define VIRGL_TEST_H     128u

static void virtio_gpu_virgl_selftest(void) {
    const uint32_t npix  = VIRGL_TEST_W * VIRGL_TEST_H;
    const uint32_t bytes = npix * 4u;                     // BGRA8 (128x128 = 64 KiB)
    phys_addr_t bp = pmm_buddy_alloc(4);                  // 16 pages = 64 KiB
    if (!PMM_ALLOC_OK(bp)) { kprintf("[virtio-gpu] virgl selftest: no mem\n"); return; }
    volatile uint32_t* back = (volatile uint32_t*)((uintptr_t)bp + HHDM_OFFSET);

    for (uint32_t i = 0; i < npix; i++) back[i] = 0xFF000000u | i;   // every pixel unique

    int ok = vgpu_ctx_create(VIRGL_TEST_CTX, 0)
          && vgpu_resource_create_3d(VIRGL_TEST_RES, PIPE_TEXTURE_2D,
                                     VIRGL_FORMAT_B8G8R8A8_UNORM,
                                     VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
                                     VIRGL_TEST_W, VIRGL_TEST_H, 1, 1, 0, 0)
          && virtio_gpu_resource_attach_backing_single(VIRGL_TEST_RES, bp, bytes)
          && vgpu_ctx_attach_resource(VIRGL_TEST_CTX, VIRGL_TEST_RES);
    if (!ok) { kprintf("[virtio-gpu] virgl selftest: setup FAILED\n"); goto out; }

    if (!vgpu_transfer_3d(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, VIRGL_TEST_CTX,
                          VIRGL_TEST_RES, 0, 0, 0, VIRGL_TEST_W, VIRGL_TEST_H, 1, 0, 0, 0, 0)) {
        kprintf("[virtio-gpu] virgl selftest: TO_HOST_3D FAILED\n"); goto out;
    }
    for (uint32_t i = 0; i < npix; i++) back[i] = 0xDEADBEEFu;       // wipe
    __asm__ volatile("mfence" ::: "memory");
    if (!vgpu_transfer_3d(VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, VIRGL_TEST_CTX,
                          VIRGL_TEST_RES, 0, 0, 0, VIRGL_TEST_W, VIRGL_TEST_H, 1, 0, 0, 0, 0)) {
        kprintf("[virtio-gpu] virgl selftest: FROM_HOST_3D FAILED\n"); goto out;
    }
    __asm__ volatile("mfence" ::: "memory");

    uint32_t bad = 0;
    for (uint32_t i = 0; i < npix; i++) if (back[i] != (0xFF000000u | i)) bad++;
    if (bad == 0)
        kprintf("[virtio-gpu] virgl selftest: PASS (3D resource round-trip %ux%u)\n",
                VIRGL_TEST_W, VIRGL_TEST_H);
    else
        kprintf("[virtio-gpu] virgl selftest: FAIL (%u/%u px wrong, first=%x)\n",
                bad, npix, back[0]);

    // ── Phase 1c: IN-STREAM texture upload (op=43 TRANSFER3D) ──────────────
    // The control-command TRANSFER above proves the host texture storage works.
    // But DarkPlaces uploads textures via the IN-STREAM virgl command
    // TRANSFER3D (op 43) inside SUBMIT_3D, NOT the control command.  Test that
    // exact path: write a fresh pattern, upload it via an in-stream TRANSFER3D,
    // wipe the backing, then read it back with the (proven) control TRANSFER_FROM
    // and verify.  A FAIL here means in-stream texture upload is the black-
    // texture bug; PASS means the upload works and the fault is elsewhere.
    for (uint32_t i = 0; i < npix; i++) back[i] = 0x00C0FFEEu;
    __asm__ volatile("mfence" ::: "memory");
    uint32_t tx[14], tn = 0;
    tx[tn++] = VIRGL_CMD0(43u, 0u, 13u);   // VIRGL_CCMD_TRANSFER3D, 13 payload dwords
    tx[tn++] = VIRGL_TEST_RES;             // res_handle
    tx[tn++] = 0;                          // level
    tx[tn++] = 0;                          // usage
    tx[tn++] = 0;                          // stride (0 => host derives from box.w)
    tx[tn++] = 0;                          // layer_stride
    tx[tn++] = 0; tx[tn++] = 0; tx[tn++] = 0;                 // box x,y,z
    tx[tn++] = VIRGL_TEST_W; tx[tn++] = VIRGL_TEST_H; tx[tn++] = 1;  // box w,h,d
    tx[tn++] = 0;                          // data_offset into the resource backing
    tx[tn++] = 1;                          // direction: 1 = TO_HOST (upload)
    int istx_ok = vgpu_submit_3d(VIRGL_TEST_CTX, tx, tn);
    for (uint32_t i = 0; i < npix; i++) back[i] = 0xDEADBEEFu;   // wipe guest side
    __asm__ volatile("mfence" ::: "memory");
    vgpu_transfer_3d(VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, VIRGL_TEST_CTX,
                     VIRGL_TEST_RES, 0, 0, 0, VIRGL_TEST_W, VIRGL_TEST_H, 1, 0, 0, 0, 0);
    __asm__ volatile("mfence" ::: "memory");
    uint32_t bad2 = 0;
    for (uint32_t i = 0; i < npix; i++) if (back[i] != 0x00C0FFEEu) bad2++;
    kprintf("[virtio-gpu] virgl IN-STREAM tex upload: %s (submit=%d, %u/%u wrong, first=%x)\n",
            bad2 ? "FAIL" : "PASS", istx_ok, bad2, npix, back[0]);

    // ── Phase 1d: COPY_TRANSFER3D from a STAGING buffer (op=45) ────────────
    // This is the path Mesa/DarkPlaces actually uses for most textures: write
    // pixels into a VIRGL_BIND_STAGING buffer, then COPY_TRANSFER3D staging ->
    // texture on the host.  If THIS is broken every world texture is black on
    // the host even though the guest data + all commands look correct.
    {
        const uint32_t STAGE_RES = 0xF004u;
        phys_addr_t sp = pmm_buddy_alloc(4);   // 64 KiB, holds 128x128 BGRA
        if (PMM_ALLOC_OK(sp)) {
            volatile uint32_t* stage = (volatile uint32_t*)((uintptr_t)sp + HHDM_OFFSET);
            for (uint32_t i = 0; i < npix; i++) stage[i] = 0x00BADA55u ^ i;
            __asm__ volatile("mfence" ::: "memory");
            int ct_ok = vgpu_resource_create_3d(STAGE_RES, 0 /*PIPE_BUFFER*/, 64 /*R8*/,
                                                0x80000u /*VIRGL_BIND_STAGING*/,
                                                npix * 4u, 1, 1, 1, 0, 0)
                     && virtio_gpu_resource_attach_backing_single(STAGE_RES, sp, bytes)
                     && vgpu_ctx_attach_resource(VIRGL_TEST_CTX, STAGE_RES);
            uint32_t cc[16], cn = 0;
            cc[cn++] = VIRGL_CMD0(45u, 0u, 14u);   // VIRGL_CCMD_COPY_TRANSFER3D
            cc[cn++] = VIRGL_TEST_RES;             // dst handle
            cc[cn++] = 0;                          // level
            cc[cn++] = 0;                          // usage
            cc[cn++] = 0;                          // stride
            cc[cn++] = 0;                          // layer_stride
            cc[cn++] = 0; cc[cn++] = 0; cc[cn++] = 0;                 // box x,y,z
            cc[cn++] = VIRGL_TEST_W; cc[cn++] = VIRGL_TEST_H; cc[cn++] = 1;  // w,h,d
            cc[cn++] = STAGE_RES;                  // src handle
            cc[cn++] = 0;                          // src offset
            cc[cn++] = 1;                          // flags: SYNCHRONIZED
            int cs_ok = vgpu_submit_3d(VIRGL_TEST_CTX, cc, cn);
            for (uint32_t i = 0; i < npix; i++) back[i] = 0xDEADBEEFu;   // wipe
            __asm__ volatile("mfence" ::: "memory");
            vgpu_transfer_3d(VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, VIRGL_TEST_CTX,
                             VIRGL_TEST_RES, 0, 0, 0, VIRGL_TEST_W, VIRGL_TEST_H, 1, 0, 0, 0, 0);
            __asm__ volatile("mfence" ::: "memory");
            uint32_t bad3 = 0;
            for (uint32_t i = 0; i < npix; i++) if (back[i] != (0x00BADA55u ^ i)) bad3++;
            kprintf("[virtio-gpu] virgl COPY_TRANSFER3D(op45): %s (setup=%d submit=%d, %u/%u wrong, first=%x)\n",
                    bad3 ? "FAIL" : "PASS", ct_ok, cs_ok, bad3, npix, back[0]);
            virtio_gpu_resource_unref(STAGE_RES);
            pmm_buddy_free(sp, 4);
        }
    }
out:
    virtio_gpu_resource_unref(VIRGL_TEST_RES);   // harmless if never created
    vgpu_ctx_destroy(VIRGL_TEST_CTX);
    pmm_buddy_free(bp, 4);
}

// ── virgl self-test (Phase 1b: the GPU actually renders) ─────────────────────
// The real 3D proof: build a virgl command stream that creates a surface view
// of a render-target texture, binds it as colour attachment 0, and CLEARs it to
// magenta (R=1,G=0,B=1,A=1); SUBMIT_3D it; then TRANSFER_FROM_HOST_3D and check
// every pixel is the cleared colour.  A correct read-back means virglrenderer
// ran GL on the host GPU on our behalf -- not just a memory copy (Phase 1a).
#define VIRGL_CLEAR_CTX   0xF002u
#define VIRGL_CLEAR_RES   0xF003u
#define VIRGL_CLEAR_SURF  1u          // virgl object handle for the surface view
#define VIRGL_CLEAR_W     16u
#define VIRGL_CLEAR_H     16u

static void virtio_gpu_virgl_clear_selftest(void) {
    const uint32_t npix  = VIRGL_CLEAR_W * VIRGL_CLEAR_H;
    const uint32_t bytes = npix * 4u;
    phys_addr_t bp = pmm_buddy_alloc(0);
    if (!PMM_ALLOC_OK(bp)) { kprintf("[virtio-gpu] virgl clear: no mem\n"); return; }
    volatile uint32_t* back = (volatile uint32_t*)((uintptr_t)bp + HHDM_OFFSET);
    for (uint32_t i = 0; i < npix; i++) back[i] = 0xDEADBEEFu;   // pre-poison

    int ok = vgpu_ctx_create(VIRGL_CLEAR_CTX, 0)
          && vgpu_resource_create_3d(VIRGL_CLEAR_RES, PIPE_TEXTURE_2D,
                                     VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET,
                                     VIRGL_CLEAR_W, VIRGL_CLEAR_H, 1, 1, 0, 0)
          && virtio_gpu_resource_attach_backing_single(VIRGL_CLEAR_RES, bp, bytes)
          && vgpu_ctx_attach_resource(VIRGL_CLEAR_CTX, VIRGL_CLEAR_RES);
    if (!ok) { kprintf("[virtio-gpu] virgl clear: setup FAILED\n"); goto out; }

    // create surface(5) + set_framebuffer_state(3) + clear(8) = 19 dwords.
    uint32_t cmd[19], n = 0;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    cmd[n++] = VIRGL_CLEAR_SURF;                // surface handle
    cmd[n++] = VIRGL_CLEAR_RES;                 // backing resource handle
    cmd[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;     // surface format
    cmd[n++] = 0;                               // texture level
    cmd[n++] = 0;                               // texture layers (first|last<<16)
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    cmd[n++] = 1;                               // nr_cbufs
    cmd[n++] = 0;                               // zsurf handle (none)
    cmd[n++] = VIRGL_CLEAR_SURF;                // cbuf[0] = our surface
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
    cmd[n++] = PIPE_CLEAR_COLOR0;               // clear colour buffer 0 only
    cmd[n++] = 0x3F800000u;                     // R = 1.0f
    cmd[n++] = 0x00000000u;                     // G = 0.0f
    cmd[n++] = 0x3F800000u;                     // B = 1.0f
    cmd[n++] = 0x3F800000u;                     // A = 1.0f
    cmd[n++] = 0; cmd[n++] = 0;                 // clear depth (double, unused)
    cmd[n++] = 0;                               // clear stencil (unused)

    if (!vgpu_submit_3d(VIRGL_CLEAR_CTX, cmd, n)) {
        kprintf("[virtio-gpu] virgl clear: SUBMIT_3D FAILED\n"); goto out;
    }
    if (!vgpu_transfer_3d(VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, VIRGL_CLEAR_CTX,
                          VIRGL_CLEAR_RES, 0, 0, 0, VIRGL_CLEAR_W, VIRGL_CLEAR_H, 1, 0, 0, 0, 0)) {
        kprintf("[virtio-gpu] virgl clear: FROM_HOST_3D FAILED\n"); goto out;
    }
    __asm__ volatile("mfence" ::: "memory");

    // B8G8R8A8 memory bytes for magenta = B,G,R,A = FF,00,FF,FF => LE 0xFFFF00FF.
    const uint32_t want = 0xFFFF00FFu;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < npix; i++) if (back[i] != want) bad++;
    if (bad == 0)
        kprintf("[virtio-gpu] virgl clear: PASS (GPU cleared %ux%u -> %x)\n",
                VIRGL_CLEAR_W, VIRGL_CLEAR_H, back[0]);
    else
        kprintf("[virtio-gpu] virgl clear: FAIL (%u/%u px wrong, first=%x want=%x)\n",
                bad, npix, back[0], want);
out:
    virtio_gpu_resource_unref(VIRGL_CLEAR_RES);
    vgpu_ctx_destroy(VIRGL_CLEAR_CTX);
    pmm_buddy_free(bp, 0);
}

// ── 3D public API (Phase 2) -- 0/-errno wrappers for the render-node uAPI ────
// kernel/drivers/video/drm.c's DRM_IOCTL_VIRTGPU_* handlers call these.  They
// convert the internal 1/0 (RESP_OK) convention to the 0/-errno the DRM layer
// expects, and gate on s_virgl so a non-3D host cannot be driven.
int virtio_gpu_3d_available(void) { return s_virgl; }

int virtio_gpu_3d_capset(uint32_t idx, uint32_t* id, uint32_t* max_ver,
                          uint32_t* max_size) {
    if (!s_virgl) return -ENODEV;
    if (idx >= s_devcfg->num_capsets) return -EINVAL;
    virtio_gpu_get_capset_info_t creq = {0};
    creq.hdr.type     = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    creq.capset_index = idx;
    virtio_gpu_resp_capset_info_t cresp = {0};
    if (!vgpu_send_ctrl(&creq, sizeof(creq), &cresp, sizeof(cresp))) return -EIO;
    if (cresp.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET_INFO) return -EIO;
    if (id)       *id       = cresp.capset_id;
    if (max_ver)  *max_ver  = cresp.capset_max_version;
    if (max_size) *max_size = cresp.capset_max_size;
    return 0;
}

// Capset blobs must fit in the response window (after its ctrl header).
#define VGPU_CAPSET_MAX ((CMD_BUF_SIZE - CMD_RESP_OFF) - sizeof(virtio_gpu_ctrl_hdr_t))
int virtio_gpu_3d_get_capset(uint32_t capset_id, uint32_t version,
                              void* out, uint32_t size) {
    if (!s_virgl) return -ENODEV;
    if (size == 0 || size > VGPU_CAPSET_MAX) return -EINVAL;   // fits the resp window
    struct __attribute__((packed)) {
        virtio_gpu_ctrl_hdr_t hdr;
        uint32_t capset_id;
        uint32_t capset_version;
    } req = {0};
    req.hdr.type        = VIRTIO_GPU_CMD_GET_CAPSET;
    req.capset_id       = capset_id;
    req.capset_version  = version;
    // Response is ctrl_hdr followed by `size` bytes of capset_data.  Heap, not
    // stack: capset blobs are multi-KiB.
    uint32_t rlen = (uint32_t)sizeof(virtio_gpu_ctrl_hdr_t) + size;
    uint8_t* resp = (uint8_t*)kmalloc(rlen);
    if (!resp) return -ENOMEM;
    if (!vgpu_send_ctrl(&req, sizeof(req), resp, rlen)) { kfree(resp); return -EIO; }
    if (((virtio_gpu_ctrl_hdr_t*)resp)->type != VIRTIO_GPU_RESP_OK_CAPSET) { kfree(resp); return -EIO; }
    __builtin_memcpy(out, resp + sizeof(virtio_gpu_ctrl_hdr_t), size);
    kfree(resp);
    return 0;
}

int virtio_gpu_3d_context_create(uint32_t ctx_id, uint32_t capset_id) {
    if (!s_virgl) return -ENODEV;
    return vgpu_ctx_create(ctx_id, capset_id) ? 0 : -EIO;
}
int virtio_gpu_3d_context_destroy(uint32_t ctx_id) {
    return vgpu_ctx_destroy(ctx_id) ? 0 : -EIO;
}
int virtio_gpu_3d_resource_create(uint32_t res_id, uint32_t target, uint32_t format,
                                   uint32_t bind, uint32_t w, uint32_t h, uint32_t depth,
                                   uint32_t array_size, uint32_t last_level, uint32_t nr_samples) {
    if (!s_virgl) return -ENODEV;
    return vgpu_resource_create_3d(res_id, target, format, bind, w, h, depth,
                                   array_size, last_level, nr_samples) ? 0 : -EIO;
}
int virtio_gpu_3d_ctx_attach_resource(uint32_t ctx_id, uint32_t res_id) {
    return vgpu_ctx_attach_resource(ctx_id, res_id) ? 0 : -EIO;
}
int virtio_gpu_3d_transfer(int to_host, uint32_t ctx_id, uint32_t res_id,
                            uint32_t x, uint32_t y, uint32_t z,
                            uint32_t w, uint32_t h, uint32_t d,
                            uint64_t offset, uint32_t level,
                            uint32_t stride, uint32_t layer_stride) {
    uint32_t cmd = to_host ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D
                           : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    return vgpu_transfer_3d(cmd, ctx_id, res_id, x, y, z, w, h, d,
                            offset, level, stride, layer_stride) ? 0 : -EIO;
}
int virtio_gpu_3d_submit(uint32_t ctx_id, const void* cmds, uint32_t size) {
    if (!s_virgl) return -ENODEV;
    if (size == 0 || (size & 3u)) return -EINVAL;    // dword-aligned command stream
    if (size > VGPU_SUBMIT_MAX) return -EINVAL;
    return vgpu_submit_3d(ctx_id, (const uint32_t*)cmds, size / 4u) ? 0 : -EIO;
}

// Snapshot of "all work submitted so far" -- WAIT for this to become `done`.
uint64_t virtio_gpu_3d_fence_barrier(void) {
    return __atomic_load_n(&s_gpu_fence_submit, __ATOMIC_ACQUIRE);
}

// Block until the control queue has retired `target` commands.  Woken by the
// control-queue MSI-X AND by vgpu_send_ctrl's own completion notify -- so a
// waiter wakes even if the device IRQ never fires (poll authoritative).  On the
// synchronous path `done` has already caught up, so this returns immediately.
int virtio_gpu_3d_fence_wait(uint64_t target) {
    while (__atomic_load_n(&s_gpu_fence_done, __ATOMIC_ACQUIRE) < target) {
        if (g_virtio_gpu_irq == 0xFFu) break;   // no IRQ wired: poll already retired it
        irq_wait(g_virtio_gpu_irq);
    }
    return 0;
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

// Maximum scanout backing we will allocate (256 MiB).  The scanout w/h come from
// the device-reported mode (untrusted); a mode whose w*h*4 exceeds this -- or would
// wrap a u32 -- is rejected rather than under-allocated.
#define VGPU_MAX_FB_BYTES (256u * 1024u * 1024u)

// PRIMITIVE (scanout backing size): the w*h*4 bytes (B8G8R8X8) a w*h scanout
// resource needs, formed without a u32 wrap and capped to VGPU_MAX_FB_BYTES.  An
// undersized backing for a w*h resource lets the GPU (and the paint loop) read/write
// past it -> host OOB.  Returns false (reject the mode) on a 0 dimension or over-cap.
// Pure -> unit-tested (vgpu_fb_bytes_selftest).
static inline bool vgpu_fb_bytes(uint32_t w, uint32_t h, uint32_t* out_bytes) {
    uint32_t wh;
    if (w == 0 || h == 0) return false;
    if (!mul_within_u32(w, h, VGPU_MAX_FB_BYTES / 4u, &wh)) return false;
    *out_bytes = wh * 4u;   // safe: wh <= MAX/4 so wh*4 <= VGPU_MAX_FB_BYTES < 2^32
    return true;
}

// Allocate a buffer large enough for w*h*4 bytes, attach it, hand to
// scanout 0.  Idempotent -- calling again re-creates (destroys old).
// Page-aligned + order-aligned so virtio-gpu sees one contiguous range.
static int vgpu_setup_scanout_buffer(uint32_t w, uint32_t h) {
    uint32_t bytes;
    if (!vgpu_fb_bytes(w, h, &bytes)) {
        kprintf("[virtio-gpu] scanout %ux%u exceeds max framebuffer size\n", w, h);
        return 0;
    }
    // Round up to a power-of-two page count.
    uint32_t pages = (bytes + 4095) / 4096;
    uint8_t  order = 0;
    while (((uint32_t)1 << order) < pages) order++;
    phys_addr_t phys = pmm_buddy_alloc(order);
    if (!PMM_ALLOC_OK(phys)) { kprintf("[virtio-gpu] fb alloc fail (%u pages order=%u)\n",
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
        // res_id was created on the host above -- destroy it, else it leaks.
        virtio_gpu_resource_unref(res_id);
        pmm_buddy_free(phys, order); return 0;
    }
    if (!virtio_gpu_set_scanout(0, res_id, w, h)) {
        kprintf("[virtio-gpu] SET_SCANOUT failed\n");
        virtio_gpu_resource_unref(res_id);
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

// Last cursor resource latched per scanout.  CRITICAL: QEMU's
// update_cursor() computes cursor VISIBILITY from `resource_id != 0` on
// EVERY command — including MOVE_CURSOR.  A MOVE with resource_id=0
// therefore HIDES the cursor (this was the invisible/flickering cursor:
// every motion frame hid it again).  Linux's virtio-gpu driver keeps a
// persistent cursor struct so MOVEs carry the live resource id; mirror
// that by latching the id from the last UPDATE.
static uint32_t s_cursor_res_latched[VIRTIO_GPU_MAX_SCANOUTS];

static int vgpu_be_cursor_update(uint32_t scanout, uint32_t res_id,
                                 uint32_t hot_x, uint32_t hot_y,
                                 int32_t x, int32_t y) {
    virtio_gpu_update_cursor_t c = {
        .hdr  = { .type = VIRTIO_GPU_CMD_UPDATE_CURSOR },
        .pos  = { .scanout_id = scanout,
                  .x = (uint32_t)x, .y = (uint32_t)y },
        .resource_id = res_id,
        .hot_x = hot_x, .hot_y = hot_y,
    };
    if (scanout < VIRTIO_GPU_MAX_SCANOUTS)
        s_cursor_res_latched[scanout] = res_id;
    return vgpu_send_cursor(&c) ? 0 : -EIO;
}

static int vgpu_be_cursor_move(uint32_t scanout, int32_t x, int32_t y) {
    virtio_gpu_update_cursor_t c = {
        .hdr  = { .type = VIRTIO_GPU_CMD_MOVE_CURSOR },
        .pos  = { .scanout_id = scanout,
                  .x = (uint32_t)x, .y = (uint32_t)y },
        .resource_id = (scanout < VIRTIO_GPU_MAX_SCANOUTS)
                           ? s_cursor_res_latched[scanout] : 0,
    };
    return vgpu_send_cursor(&c) ? 0 : -EIO;
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
    .cursor_update           = vgpu_be_cursor_update,
    .cursor_move             = vgpu_be_cursor_move,
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

#ifdef MAKAOS_BOOT_SELFTESTS
// vgpu_fb_bytes forms w*h*4 without a u32 wrap and caps it, so a device-reported
// mode cannot under-allocate the scanout backing (-> GPU / paint-loop OOB).  Drive
// the wrap + over-cap reject paths that a naive `w*h*4` would slip through.
void vgpu_fb_bytes_selftest(void) {
    kprintf("[vgpu_fbsz] vgpu_fb_bytes must size w*h*4 without u32 wrap, capped\n");
    int fails = 0;
    struct { uint32_t w, h; uint8_t want_ok; uint32_t want_bytes; } c[] = {
        { 1280,    800,     1, 4096000u },       // normal mode
        { 7680,    4320,    1, 132710400u },     // 8K, large but valid
        { 8192,    8192,    1, 0x10000000u },    // exactly VGPU_MAX_FB_BYTES (256 MiB)
        { 8193,    8192,    0, 0 },              // one row over the cap -> reject
        { 0x10000, 0x10000, 0, 0 },              // w*h == 2^32: a u32 product wraps to 0
        { 0,       800,     0, 0 },              // zero width -> reject
        { 1280,    0,       0, 0 },              // zero height -> reject
    };
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        uint32_t got = 0xDEADBEEFu;
        bool ok = vgpu_fb_bytes(c[i].w, c[i].h, &got);
        if (ok != (c[i].want_ok != 0) || (ok && got != c[i].want_bytes)) {
            kprintf_atomic("[vgpu_fbsz] FAIL %ux%u ok=%d want=%d bytes=0x%lx want=0x%lx\n",
                    c[i].w, c[i].h, ok, c[i].want_ok,
                    (unsigned long)got, (unsigned long)c[i].want_bytes);
            fails++;
        }
    }
    kprintf_atomic(fails ? "[vgpu_fbsz] SELF-TEST FAILED\n"
                  : "[vgpu_fbsz] SELF-TEST PASSED (w*h*4 u64, no wrap, capped)\n");
}
#endif
