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
#define DRM_IOCTL_MODE_GETENCODER     0xC01464A6
#define DRM_IOCTL_MODE_GETCONNECTOR   0xC05064A7

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
    (void)self;
    switch (req) {
    case DRM_IOCTL_VERSION:           return drm_ioctl_version(arg);
    case DRM_IOCTL_GET_CAP:           return drm_ioctl_get_cap(arg);
    case DRM_IOCTL_MODE_GETRESOURCES: return drm_ioctl_mode_getresources(arg);
    case DRM_IOCTL_MODE_GETCONNECTOR: return drm_ioctl_mode_getconnector(arg);
    case DRM_IOCTL_MODE_GETENCODER:   return drm_ioctl_mode_getencoder(arg);
    case DRM_IOCTL_MODE_GETCRTC:      return drm_ioctl_mode_getcrtc(arg);
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
    // Nothing stateful yet; part 2 will track per-fd dumb buffers + fbs.
    kfree(self);
}

vfs_file_t* vfs_drm_open(void) {
    vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(*f));
    if (!f) return NULL;
    __builtin_memset(f, 0, sizeof(*f));
    f->ioctl    = drm_ioctl;
    f->close    = drm_close;
    f->refcount = 1;
    f->rights   = 0xFFFFFFFFu;   // full rights; access-controlled by file mode
    return f;
}
