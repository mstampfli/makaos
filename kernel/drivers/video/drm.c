// ── DRM/KMS uAPI layer (Tier 2.5b, part 1) ──────────────────────────
//
// Implements the Linux-compatible DRM ioctl surface for enumeration.
// /dev/dri/card0 opens to a vfs_file_t whose ioctl op dispatches here.
// Maps DRM concepts onto virtio-gpu:
//
//   DRM connector  ≈  virtio-gpu scanout (0..N)
//   DRM crtc       ≈  virtio-gpu scanout, managed by SET_SCANOUT
//   DRM encoder    ≈  1:1 with connector, always "virtual"
//   DRM framebuffer ≈ a virtio-gpu RESOURCE_CREATE_2D handle
//
// Part 1 (this file's initial drop): enumeration only — VERSION,
// GET_CAP, MODE_GETRESOURCES, MODE_GETCONNECTOR.  Enough for libdrm
// to `drmModeGetResources` + `drmModeGetConnector` without aborting.
// Dumb buffers / ADDFB2 / SETCRTC / PAGE_FLIP come next.

#include "drm.h"
#include "virtio_gpu.h"
#include "kheap.h"
#include "kprintf.h"
#include "errno.h"
#include "pmm.h"
#include "vmm.h"
#include "process.h"
#include "smp.h"
#include "syscall.h"   // copy_from_user / copy_to_user

extern int copy_from_user(void* dst, const void* src_u, uint64_t len);
extern int copy_to_user(void* dst_u, const void* src, uint64_t len);

// ── Linux DRM ioctl numbers (subset) ─────────────────────────────────
// Encoded via _IOC in Linux; we hardcode the exact request values
// libdrm emits so we don't need the _IOC macros here.  These match
// Linux drm/drm.h exactly.
#define DRM_IOCTL_VERSION             0xC0406400
#define DRM_IOCTL_GET_CAP             0xC010640C
#define DRM_IOCTL_SET_MASTER          0x0000641E
#define DRM_IOCTL_DROP_MASTER         0x0000641F
#define DRM_IOCTL_MODE_GETRESOURCES   0xC04064A0
#define DRM_IOCTL_MODE_GETCRTC        0xC06864A1
#define DRM_IOCTL_MODE_SETCRTC        0xC06864A2
#define DRM_IOCTL_MODE_GETENCODER     0xC01464A6
#define DRM_IOCTL_MODE_GETCONNECTOR   0xC05064A7
#define DRM_IOCTL_MODE_ADDFB2         0xC06864B8
#define DRM_IOCTL_MODE_RMFB           0xC00464AF
#define DRM_IOCTL_MODE_PAGE_FLIP      0xC01864B0
#define DRM_IOCTL_MODE_CREATE_DUMB    0xC02064B2
#define DRM_IOCTL_MODE_MAP_DUMB       0xC01064B3
#define DRM_IOCTL_MODE_DESTROY_DUMB   0xC00464B4

// ── DRM capability IDs ──────────────────────────────────────────────
#define DRM_CAP_DUMB_BUFFER          0x1
#define DRM_CAP_VBLANK_HIGH_CRTC     0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define DRM_CAP_DUMB_PREFER_SHADOW   0x4
#define DRM_CAP_PRIME                0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC  0x6
#define DRM_CAP_ASYNC_PAGE_FLIP      0x7
#define DRM_CAP_CURSOR_WIDTH         0x8
#define DRM_CAP_CURSOR_HEIGHT        0x9
#define DRM_CAP_ADDFB2_MODIFIERS     0x10
#define DRM_CAP_PAGE_FLIP_TARGET     0x11

#define DRM_PRIME_CAP_IMPORT 0x1
#define DRM_PRIME_CAP_EXPORT 0x2

// ── ioctl argument structs (Linux drm_mode.h / drm.h compatible) ────
typedef struct {
    int32_t  version_major;
    int32_t  version_minor;
    int32_t  version_patchlevel;
    uint64_t name_len;        // in-out: buffer size / actual name length
    uint64_t name;            // user pointer
    uint64_t date_len;
    uint64_t date;
    uint64_t desc_len;
    uint64_t desc;
} drm_version_t;

typedef struct {
    uint64_t capability;
    uint64_t value;
} drm_get_cap_t;

typedef struct {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
} drm_mode_card_res_t;

// drm_mode_modeinfo — 68 bytes, Linux-ABI
typedef struct __attribute__((packed)) {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char     name[32];
} drm_mode_modeinfo_t;

typedef struct {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;

    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;

    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;

    uint32_t connection;       // 1=connected, 2=disconnected, 3=unknown
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    uint32_t pad;
} drm_mode_get_connector_t;

// ── MakaOS ID scheme ────────────────────────────────────────────────
// Each scanout N gets:
//   connector_id = 100 + N
//   encoder_id   = 200 + N
//   crtc_id      = 300 + N
// Plenty of headroom, trivially decodable.  fb_ids are allocated
// dynamically in part 2 starting at 1.
#define CONN_BASE  100u
#define ENC_BASE   200u
#define CRTC_BASE  300u

// ── Per-fd state: dumb buffers + framebuffers ────────────────────────
// Each open of /dev/dri/card0 gets a private drm_client_t in f->ctx.
// Dumb buffer handles + fb ids are scoped to the fd (Linux semantics).
// Resource IDs (virtio-gpu-global) are allocated from a separate
// monotonically-increasing counter — the hardware shares them across
// all clients but we enforce per-client handle isolation.

typedef struct drm_dumb {
    uint32_t           handle;         // per-fd ID (1..)
    uint32_t           width, height;
    uint32_t           pitch;          // bytes per row
    uint64_t           size;           // total bytes
    uint32_t           vgpu_res_id;    // device-global resource id
    phys_addr_t        phys;           // backing pages (contiguous)
    uint32_t           bytes_alloc;    // pow-2 page-rounded allocation
    uint8_t            order;          // buddy order actually used
    struct drm_dumb*   next;
} drm_dumb_t;

typedef struct drm_fb {
    uint32_t           fb_id;          // per-fd ID (1..)
    uint32_t           handle;         // matching drm_dumb_t.handle
    uint32_t           width, height;
    uint32_t           pitch;
    uint32_t           format;         // DRM_FORMAT_*
    struct drm_fb*     next;
} drm_fb_t;

typedef struct drm_client {
    drm_dumb_t* dumbs;
    drm_fb_t*   fbs;
    uint32_t    next_dumb_handle;      // starts at 1
    uint32_t    next_fb_id;            // starts at 1
} drm_client_t;

// Device-global resource id allocator.  Never returns 0 (reserved).
static uint32_t s_next_res_id = 1;
static uint32_t alloc_res_id(void) {
    uint32_t r;
    do { r = __atomic_fetch_add(&s_next_res_id, 1, __ATOMIC_RELAXED); }
    while (r == 0);
    return r;
}

static drm_client_t* client_of(vfs_file_t* f) { return (drm_client_t*)f->ctx; }

static drm_dumb_t* find_dumb(drm_client_t* c, uint32_t handle) {
    for (drm_dumb_t* d = c->dumbs; d; d = d->next)
        if (d->handle == handle) return d;
    return NULL;
}

static drm_fb_t* find_fb(drm_client_t* c, uint32_t fb_id) {
    for (drm_fb_t* fb = c->fbs; fb; fb = fb->next)
        if (fb->fb_id == fb_id) return fb;
    return NULL;
}

// ── CREATE_DUMB ──────────────────────────────────────────────────────
// Linux uses: u32 height, u32 width, u32 bpp, u32 flags, u32 handle,
// u32 pitch, u64 size.  Driver fills handle/pitch/size.
typedef struct {
    uint32_t height, width, bpp, flags;
    uint32_t handle, pitch;
    uint64_t size;
} drm_mode_create_dumb_t;

static int drm_ioctl_create_dumb(vfs_file_t* f, uint64_t arg) {
    drm_mode_create_dumb_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    if (!a.width || !a.height) return -EINVAL;
    if (a.bpp != 32) return -EINVAL;     // we only support 32-bpp

    uint32_t pitch = a.width * 4;
    uint64_t size  = (uint64_t)pitch * a.height;
    // Round allocation up to a power-of-two page count so virtio-gpu
    // can attach it as a single contiguous range.
    uint64_t pages = (size + 4095) / 4096;
    uint8_t  order = 0;
    while (((uint64_t)1 << order) < pages) order++;
    phys_addr_t phys = pmm_buddy_alloc(order);
    if (!phys) return -ENOMEM;
    uint32_t bytes_alloc = (uint32_t)((uint64_t)1 << order) * 4096u;

    // Zero backing pages (prevents info leak).  HHDM mapping.
    uint8_t* virt = (uint8_t*)((uintptr_t)phys + HHDM_OFFSET);
    __builtin_memset(virt, 0, bytes_alloc);

    // Ref-count each backing page so our own user VA maps don't free
    // pages from under us if the process exits before DESTROY_DUMB.
    for (uint32_t i = 0; i < bytes_alloc / 4096u; i++)
        pmm_ref_inc(phys + (phys_addr_t)i * 4096u);

    uint32_t res_id = alloc_res_id();
    if (!virtio_gpu_resource_create_2d(res_id,
                                        VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
                                        a.width, a.height)) {
        pmm_buddy_free(phys, order);
        return -EIO;
    }
    if (!virtio_gpu_resource_attach_backing_single(res_id, phys, bytes_alloc)) {
        virtio_gpu_resource_unref(res_id);
        pmm_buddy_free(phys, order);
        return -EIO;
    }

    drm_dumb_t* d = (drm_dumb_t*)kmalloc(sizeof(*d));
    if (!d) {
        virtio_gpu_resource_unref(res_id);
        pmm_buddy_free(phys, order);
        return -ENOMEM;
    }
    drm_client_t* c = client_of(f);
    d->handle      = c->next_dumb_handle++;
    d->width       = a.width;
    d->height      = a.height;
    d->pitch       = pitch;
    d->size        = size;
    d->vgpu_res_id = res_id;
    d->phys        = phys;
    d->bytes_alloc = bytes_alloc;
    d->order       = order;
    d->next        = c->dumbs;
    c->dumbs       = d;

    a.handle = d->handle;
    a.pitch  = pitch;
    a.size   = size;
    if (copy_to_user((void*)arg, &a, sizeof(a)) != 0) return -EFAULT;
    return 0;
}

typedef struct {
    uint32_t handle, pad;
    uint64_t offset;
} drm_mode_map_dumb_t;

// MAP_DUMB returns a magic offset that encodes the handle.  Userland
// mmaps the DRM fd at that offset; sys_mmap detects a DRM fd and calls
// drm_resolve_dumb_mmap to translate offset → backing pages.
#define DRM_DUMB_OFFSET_SHIFT 32
#define DRM_DUMB_OFFSET_MARK  0xDD00000000000000ull

static int drm_ioctl_map_dumb(vfs_file_t* f, uint64_t arg) {
    drm_mode_map_dumb_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    drm_client_t* c = client_of(f);
    drm_dumb_t* d = find_dumb(c, a.handle);
    if (!d) return -ENOENT;
    a.offset = DRM_DUMB_OFFSET_MARK
             | ((uint64_t)a.handle << DRM_DUMB_OFFSET_SHIFT);
    if (copy_to_user((void*)arg, &a, sizeof(a)) != 0) return -EFAULT;
    return 0;
}

typedef struct {
    uint32_t handle;
} drm_mode_destroy_dumb_t;

static void dumb_free(drm_dumb_t* d) {
    virtio_gpu_resource_unref(d->vgpu_res_id);
    // Drop our extra ref; if user unmapped, this was the last ref and
    // pmm_ref_dec frees the pages.  If not unmapped, the unmap path
    // will take them down.
    for (uint32_t i = 0; i < d->bytes_alloc / 4096u; i++)
        pmm_ref_dec(d->phys + (phys_addr_t)i * 4096u);
    // Also release the buddy allocation back.  pmm_ref_dec freed the
    // individual pages but the buddy bookkeeping still owns them —
    // this returns them to the free list.  Safe because we held a
    // ref for every page above.
    pmm_buddy_free(d->phys, d->order);
    kfree(d);
}

static int drm_ioctl_destroy_dumb(vfs_file_t* f, uint64_t arg) {
    drm_mode_destroy_dumb_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    drm_client_t* c = client_of(f);
    drm_dumb_t** pp = &c->dumbs;
    while (*pp) {
        if ((*pp)->handle == a.handle) {
            drm_dumb_t* d = *pp;
            // Remove any fbs that reference this dumb handle.
            drm_fb_t** fpp = &c->fbs;
            while (*fpp) {
                if ((*fpp)->handle == a.handle) {
                    drm_fb_t* fb = *fpp;
                    *fpp = fb->next;
                    kfree(fb);
                } else {
                    fpp = &(*fpp)->next;
                }
            }
            *pp = d->next;
            dumb_free(d);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -ENOENT;
}

// ── ADDFB2 / RMFB ────────────────────────────────────────────────────
// Linux layout (drm_mode_fb_cmd2):
typedef struct {
    uint32_t fb_id, width, height, pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
} drm_mode_fb_cmd2_t;

static int drm_ioctl_addfb2(vfs_file_t* f, uint64_t arg) {
    drm_mode_fb_cmd2_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    if (!a.handles[0]) return -EINVAL;
    drm_client_t* c = client_of(f);
    drm_dumb_t* d = find_dumb(c, a.handles[0]);
    if (!d) return -ENOENT;
    if (a.width > d->width || a.height > d->height) return -EINVAL;

    drm_fb_t* fb = (drm_fb_t*)kmalloc(sizeof(*fb));
    if (!fb) return -ENOMEM;
    fb->fb_id  = c->next_fb_id++;
    fb->handle = a.handles[0];
    fb->width  = a.width;
    fb->height = a.height;
    fb->pitch  = a.pitches[0] ? a.pitches[0] : d->pitch;
    fb->format = a.pixel_format;
    fb->next   = c->fbs;
    c->fbs     = fb;

    a.fb_id = fb->fb_id;
    if (copy_to_user((void*)arg, &a, sizeof(a)) != 0) return -EFAULT;
    return 0;
}

typedef struct { uint32_t fb_id; } drm_mode_rmfb_t;

static int drm_ioctl_rmfb(vfs_file_t* f, uint64_t arg) {
    drm_mode_rmfb_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    drm_client_t* c = client_of(f);
    drm_fb_t** pp = &c->fbs;
    while (*pp) {
        if ((*pp)->fb_id == a.fb_id) {
            drm_fb_t* fb = *pp;
            *pp = fb->next;
            kfree(fb);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -ENOENT;
}

// ── SETCRTC / PAGE_FLIP ──────────────────────────────────────────────
// SETCRTC binds fb_id to crtc_id and forces an initial upload+flush.
// Linux drm_mode_crtc layout:
typedef struct {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    drm_mode_modeinfo_t mode;
} drm_mode_crtc_t;

static int drm_ioctl_setcrtc(vfs_file_t* f, uint64_t arg) {
    drm_mode_crtc_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    uint32_t n_sc = virtio_gpu_num_scanouts();
    if (a.crtc_id < CRTC_BASE || a.crtc_id >= CRTC_BASE + n_sc) return -ENOENT;
    uint32_t scanout = a.crtc_id - CRTC_BASE;

    if (a.fb_id == 0) {
        // Disable — zero the scanout by pointing to resource 0.
        virtio_gpu_set_scanout(scanout, 0, 0, 0);
        return 0;
    }
    drm_client_t* c = client_of(f);
    drm_fb_t* fb = find_fb(c, a.fb_id);
    if (!fb) return -ENOENT;
    drm_dumb_t* d = find_dumb(c, fb->handle);
    if (!d) return -ENOENT;

    if (!virtio_gpu_set_scanout(scanout, d->vgpu_res_id, fb->width, fb->height))
        return -EIO;
    if (!virtio_gpu_transfer_to_host_2d(d->vgpu_res_id, fb->width, fb->height))
        return -EIO;
    if (!virtio_gpu_resource_flush(d->vgpu_res_id, fb->width, fb->height))
        return -EIO;
    return 0;
}

// PAGE_FLIP: same shape as SETCRTC for the fb swap, minus mode info.
// We transfer + flush synchronously; the Linux async event isn't
// emitted yet (wlroots handles missing event via timeout).
typedef struct {
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
} drm_mode_page_flip_t;

static int drm_ioctl_page_flip(vfs_file_t* f, uint64_t arg) {
    drm_mode_page_flip_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    uint32_t n_sc = virtio_gpu_num_scanouts();
    if (a.crtc_id < CRTC_BASE || a.crtc_id >= CRTC_BASE + n_sc) return -ENOENT;
    uint32_t scanout = a.crtc_id - CRTC_BASE;

    drm_client_t* c = client_of(f);
    drm_fb_t* fb = find_fb(c, a.fb_id);
    if (!fb) return -ENOENT;
    drm_dumb_t* d = find_dumb(c, fb->handle);
    if (!d) return -ENOENT;

    if (!virtio_gpu_set_scanout(scanout, d->vgpu_res_id, fb->width, fb->height))
        return -EIO;
    if (!virtio_gpu_transfer_to_host_2d(d->vgpu_res_id, fb->width, fb->height))
        return -EIO;
    if (!virtio_gpu_resource_flush(d->vgpu_res_id, fb->width, fb->height))
        return -EIO;
    return 0;
}

// ── mmap resolver (called from sys_mmap) ─────────────────────────────
// Given a DRM fd and the offset from MAP_DUMB, return the backing
// physical address + byte count.  Returns 0 on success, -errno on
// failure.  The caller's sys_mmap installs the PTEs.
int64_t drm_resolve_dumb_mmap(vfs_file_t* f, uint64_t offset,
                                uint64_t len, phys_addr_t* out_phys,
                                uint64_t* out_bytes) {
    if ((offset & 0xFF00000000000000ull) != DRM_DUMB_OFFSET_MARK)
        return -EINVAL;
    uint32_t handle = (uint32_t)(offset >> DRM_DUMB_OFFSET_SHIFT) & 0xFFFFFF;
    drm_client_t* c = client_of(f);
    drm_dumb_t* d = find_dumb(c, handle);
    if (!d) return -ENOENT;
    if (len > d->bytes_alloc) return -EINVAL;
    *out_phys  = d->phys;
    *out_bytes = d->bytes_alloc;
    return 0;
}

// Predicate used by sys_mmap to detect a DRM fd without exposing
// drm_close externally.  static-in-.c comparison is reliable: every
// vfs_file_t we return from vfs_drm_open has this specific pointer.
static void drm_close(vfs_file_t* self);
int drm_is_drm_file(vfs_file_t* f) {
    return f && f->close == drm_close;
}

static int drm_ioctl_version(uint64_t arg) {
    drm_version_t v;
    if (copy_from_user(&v, (void*)arg, sizeof(v)) != 0) return -EFAULT;
    static const char name[] = "makaos-drm";
    static const char date[] = "20260420";
    static const char desc[] = "MakaOS virtio-gpu DRM layer";
    v.version_major     = 1;
    v.version_minor     = 0;
    v.version_patchlevel = 0;

    // name_len / date_len / desc_len act as both in (buffer size) and
    // out (actual length).  Linux convention: if the provided buffer
    // is 0, we fill the *_len out-param so the caller can retry with
    // a sized buffer.
    uint64_t n_out = sizeof(name) - 1;
    uint64_t d_out = sizeof(date) - 1;
    uint64_t s_out = sizeof(desc) - 1;
    if (v.name && v.name_len >= n_out + 1) {
        if (copy_to_user((void*)v.name, name, n_out + 1) != 0) return -EFAULT;
    }
    if (v.date && v.date_len >= d_out + 1) {
        if (copy_to_user((void*)v.date, date, d_out + 1) != 0) return -EFAULT;
    }
    if (v.desc && v.desc_len >= s_out + 1) {
        if (copy_to_user((void*)v.desc, desc, s_out + 1) != 0) return -EFAULT;
    }
    v.name_len = n_out;
    v.date_len = d_out;
    v.desc_len = s_out;
    if (copy_to_user((void*)arg, &v, sizeof(v)) != 0) return -EFAULT;
    return 0;
}

static int drm_ioctl_get_cap(uint64_t arg) {
    drm_get_cap_t c;
    if (copy_from_user(&c, (void*)arg, sizeof(c)) != 0) return -EFAULT;
    switch (c.capability) {
    case DRM_CAP_DUMB_BUFFER:          c.value = 1; break;
    case DRM_CAP_DUMB_PREFERRED_DEPTH: c.value = 32; break;
    case DRM_CAP_DUMB_PREFER_SHADOW:   c.value = 0; break;
    case DRM_CAP_VBLANK_HIGH_CRTC:     c.value = 1; break;
    case DRM_CAP_PRIME:                c.value = 0; break;  // no PRIME yet
    case DRM_CAP_TIMESTAMP_MONOTONIC:  c.value = 1; break;
    case DRM_CAP_ASYNC_PAGE_FLIP:      c.value = 0; break;
    case DRM_CAP_CURSOR_WIDTH:         c.value = 64; break;
    case DRM_CAP_CURSOR_HEIGHT:        c.value = 64; break;
    case DRM_CAP_ADDFB2_MODIFIERS:     c.value = 0; break;
    case DRM_CAP_PAGE_FLIP_TARGET:     c.value = 0; break;
    default:                           return -EINVAL;
    }
    if (copy_to_user((void*)arg, &c, sizeof(c)) != 0) return -EFAULT;
    return 0;
}

// Copy up to `avail` u32 IDs to a user pointer, updating the count.
// in: *user_count = how many slots the caller provided.
// out: *user_count = actual count.  Returns 0 or -errno.
static int drm_write_id_array(uint64_t ptr, uint32_t* user_count,
                                const uint32_t* ids, uint32_t avail) {
    uint32_t n = *user_count < avail ? *user_count : avail;
    if (ptr && n) {
        if (copy_to_user((void*)ptr, ids, n * sizeof(uint32_t)) != 0)
            return -EFAULT;
    }
    *user_count = avail;
    return 0;
}

static int drm_ioctl_mode_getresources(uint64_t arg) {
    drm_mode_card_res_t r;
    if (copy_from_user(&r, (void*)arg, sizeof(r)) != 0) return -EFAULT;

    uint32_t n_sc = virtio_gpu_num_scanouts();
    if (n_sc == 0) {
        // No virtio-gpu display: report an empty resource set so
        // libdrm doesn't crash.  min/max stays 0 which means "no
        // pipeline" to clients that check.
        r.count_fbs        = 0;
        r.count_crtcs      = 0;
        r.count_connectors = 0;
        r.count_encoders   = 0;
        r.min_width = r.max_width  = 0;
        r.min_height = r.max_height = 0;
        if (copy_to_user((void*)arg, &r, sizeof(r)) != 0) return -EFAULT;
        return 0;
    }

    uint32_t crtcs[VIRTIO_GPU_MAX_SCANOUTS_CAP];
    uint32_t conns[VIRTIO_GPU_MAX_SCANOUTS_CAP];
    uint32_t encs[VIRTIO_GPU_MAX_SCANOUTS_CAP];
    for (uint32_t i = 0; i < n_sc; i++) {
        crtcs[i] = CRTC_BASE + i;
        conns[i] = CONN_BASE + i;
        encs[i]  = ENC_BASE  + i;
    }

    // Probe scanout 0 for max dimensions (all scanouts share the same
    // bounds in virtio-gpu practice — but we conservatively take the
    // enumerated mode as both min and max).
    uint32_t w0 = 0, h0 = 0;
    virtio_gpu_get_mode(0, &w0, &h0);
    r.min_width  = 1;
    r.max_width  = w0 ? w0 : 8192;
    r.min_height = 1;
    r.max_height = h0 ? h0 : 8192;

    int rc;
    rc = drm_write_id_array(r.crtc_id_ptr,      &r.count_crtcs,      crtcs, n_sc);
    if (rc) return rc;
    rc = drm_write_id_array(r.connector_id_ptr, &r.count_connectors, conns, n_sc);
    if (rc) return rc;
    rc = drm_write_id_array(r.encoder_id_ptr,   &r.count_encoders,   encs,  n_sc);
    if (rc) return rc;
    r.count_fbs = 0;  // no fbs until ADDFB2 lands

    if (copy_to_user((void*)arg, &r, sizeof(r)) != 0) return -EFAULT;
    return 0;
}

static int drm_ioctl_mode_getconnector(uint64_t arg) {
    drm_mode_get_connector_t c;
    if (copy_from_user(&c, (void*)arg, sizeof(c)) != 0) return -EFAULT;

    uint32_t n_sc = virtio_gpu_num_scanouts();
    if (c.connector_id < CONN_BASE || c.connector_id >= CONN_BASE + n_sc)
        return -ENOENT;
    uint32_t idx = c.connector_id - CONN_BASE;

    uint32_t w = 0, h = 0;
    virtio_gpu_get_mode(idx, &w, &h);

    c.encoder_id        = ENC_BASE + idx;
    c.connector_type    = 11;             // DRM_MODE_CONNECTOR_VIRTUAL
    c.connector_type_id = idx + 1;
    c.connection        = w && h ? 1 : 2; // connected / disconnected
    c.mm_width          = 0;
    c.mm_height         = 0;
    c.subpixel          = 0;              // DRM_MODE_SUBPIXEL_UNKNOWN

    // Modes: report a single preferred mode.  Simple timing —
    // virtio-gpu doesn't do real CRTC timing, host SDL compositor
    // handles actual rendering.  60 Hz refresh.
    drm_mode_modeinfo_t mode = {0};
    if (w && h) {
        mode.clock      = (uint32_t)((uint64_t)w * h * 60u / 1000u);
        mode.hdisplay   = (uint16_t)w;
        mode.hsync_start = (uint16_t)(w + 8);
        mode.hsync_end   = (uint16_t)(w + 16);
        mode.htotal      = (uint16_t)(w + 32);
        mode.vdisplay    = (uint16_t)h;
        mode.vsync_start = (uint16_t)(h + 2);
        mode.vsync_end   = (uint16_t)(h + 4);
        mode.vtotal      = (uint16_t)(h + 8);
        mode.vrefresh    = 60;
        mode.type        = 0x40;   // DRM_MODE_TYPE_PREFERRED
        const char* src = "preferred";
        int i = 0;
        for (; src[i] && i < 31; i++) mode.name[i] = src[i];
        mode.name[i] = 0;
    }

    // Write mode (up to 1 mode).  libdrm passes NULL first to get
    // count_modes, then allocates and calls again.
    uint32_t avail_modes = (w && h) ? 1 : 0;
    uint32_t emit_modes  = c.count_modes < avail_modes ? c.count_modes : avail_modes;
    if (c.modes_ptr && emit_modes) {
        if (copy_to_user((void*)c.modes_ptr, &mode, sizeof(mode)) != 0)
            return -EFAULT;
    }
    c.count_modes = avail_modes;

    // Single encoder (our own encoder_id).
    uint32_t enc = ENC_BASE + idx;
    uint32_t emit_enc = c.count_encoders < 1 ? c.count_encoders : 1;
    if (c.encoders_ptr && emit_enc) {
        if (copy_to_user((void*)c.encoders_ptr, &enc, sizeof(enc)) != 0)
            return -EFAULT;
    }
    c.count_encoders = 1;

    // No properties.
    c.count_props = 0;

    if (copy_to_user((void*)arg, &c, sizeof(c)) != 0) return -EFAULT;
    return 0;
}

static int drm_ioctl_mode_getencoder(uint64_t arg) {
    struct {
        uint32_t encoder_id;
        uint32_t encoder_type;
        uint32_t crtc_id;
        uint32_t possible_crtcs;
        uint32_t possible_clones;
    } e;
    if (copy_from_user(&e, (void*)arg, sizeof(e)) != 0) return -EFAULT;

    uint32_t n_sc = virtio_gpu_num_scanouts();
    if (e.encoder_id < ENC_BASE || e.encoder_id >= ENC_BASE + n_sc)
        return -ENOENT;
    uint32_t idx = e.encoder_id - ENC_BASE;

    e.encoder_type    = 2;            // DRM_MODE_ENCODER_TMDS — "virtual"
    e.crtc_id         = CRTC_BASE + idx;
    e.possible_crtcs  = 1u << idx;    // this encoder can drive this crtc
    e.possible_clones = 0;

    if (copy_to_user((void*)arg, &e, sizeof(e)) != 0) return -EFAULT;
    return 0;
}

static int drm_ioctl_mode_getcrtc(uint64_t arg) {
    struct {
        uint64_t set_connectors_ptr;
        uint32_t count_connectors;
        uint32_t crtc_id;
        uint32_t fb_id;
        uint32_t x, y;
        uint32_t gamma_size;
        uint32_t mode_valid;
        drm_mode_modeinfo_t mode;
    } c;
    if (copy_from_user(&c, (void*)arg, sizeof(c)) != 0) return -EFAULT;

    uint32_t n_sc = virtio_gpu_num_scanouts();
    if (c.crtc_id < CRTC_BASE || c.crtc_id >= CRTC_BASE + n_sc)
        return -ENOENT;

    c.fb_id       = 0;       // no framebuffer bound yet (part 1)
    c.x = c.y     = 0;
    c.gamma_size  = 0;       // no gamma in virtio-gpu
    c.mode_valid  = 0;
    __builtin_memset(&c.mode, 0, sizeof(c.mode));
    c.count_connectors = 0;

    if (copy_to_user((void*)arg, &c, sizeof(c)) != 0) return -EFAULT;
    return 0;
}

// ── ioctl dispatch ──────────────────────────────────────────────────
static int64_t drm_ioctl(vfs_file_t* self, uint64_t req, uint64_t arg) {
    switch (req) {
    case DRM_IOCTL_VERSION:           return drm_ioctl_version(arg);
    case DRM_IOCTL_GET_CAP:           return drm_ioctl_get_cap(arg);
    case DRM_IOCTL_MODE_GETRESOURCES: return drm_ioctl_mode_getresources(arg);
    case DRM_IOCTL_MODE_GETCONNECTOR: return drm_ioctl_mode_getconnector(arg);
    case DRM_IOCTL_MODE_GETENCODER:   return drm_ioctl_mode_getencoder(arg);
    case DRM_IOCTL_MODE_GETCRTC:      return drm_ioctl_mode_getcrtc(arg);
    case DRM_IOCTL_MODE_SETCRTC:      return drm_ioctl_setcrtc(self, arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:    return drm_ioctl_page_flip(self, arg);
    case DRM_IOCTL_MODE_ADDFB2:       return drm_ioctl_addfb2(self, arg);
    case DRM_IOCTL_MODE_RMFB:         return drm_ioctl_rmfb(self, arg);
    case DRM_IOCTL_MODE_CREATE_DUMB:  return drm_ioctl_create_dumb(self, arg);
    case DRM_IOCTL_MODE_MAP_DUMB:     return drm_ioctl_map_dumb(self, arg);
    case DRM_IOCTL_MODE_DESTROY_DUMB: return drm_ioctl_destroy_dumb(self, arg);
    case DRM_IOCTL_SET_MASTER:        return 0;  // single-client for now
    case DRM_IOCTL_DROP_MASTER:       return 0;
    default:
        // Unknown ioctl — log once so we learn what wlroots actually calls,
        // then return ENOTTY (Linux DRM's standard "unsupported").
        kprintf("[drm] ENOTTY: req=0x%08x\n", (uint32_t)req);
        return -ENOTTY;
    }
}

static void drm_close(vfs_file_t* self) {
    // Tear down every dumb buffer + fb this client still holds.
    drm_client_t* c = client_of(self);
    if (c) {
        drm_fb_t* fb = c->fbs;
        while (fb) { drm_fb_t* n = fb->next; kfree(fb); fb = n; }
        drm_dumb_t* d = c->dumbs;
        while (d)  { drm_dumb_t* n = d->next; dumb_free(d); d = n; }
        kfree(c);
        self->ctx = NULL;
    }
    kfree(self);
}

vfs_file_t* vfs_drm_open(void) {
    drm_client_t* c = (drm_client_t*)kmalloc(sizeof(*c));
    if (!c) return NULL;
    __builtin_memset(c, 0, sizeof(*c));
    c->next_dumb_handle = 1;
    c->next_fb_id       = 1;

    vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(*f));
    if (!f) { kfree(c); return NULL; }
    __builtin_memset(f, 0, sizeof(*f));
    f->ioctl    = drm_ioctl;
    f->close    = drm_close;
    f->ctx      = c;
    f->refcount = 1;
    f->rights   = 0xFFFFFFFFu;
    return f;
}
