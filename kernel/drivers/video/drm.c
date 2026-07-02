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
#include "drm_backend.h"
#include "uaccess.h"   // copy_from_user / copy_to_user (shared decls)
#include "virtio_gpu.h"
#include "kheap.h"
#include "kprintf.h"
#include "errno.h"
#include "checked.h"   // ckd_mul_u64 / mul_within_u32 (overflow-safe sizing)
#include "pmm.h"
#include "vmm.h"
#include "process.h"
#include "smp.h"
#include "sched.h"      // g_current
#include "seqlock.h"
#include "syscall.h"   // copy_from_user / copy_to_user
#include "log.h"
#include "trace.h"
#include "assert.h"

/* Per-subsystem debug gate.  Keep at 1 while diagnosing wlroots DRM
 * path; flip to 0 once the pipeline is stable.  See DEBUGGING.md §2.3. */
#define CONFIG_DEBUG_DRM 1

#if CONFIG_DEBUG_DRM
#define drm_dbg(fmt, ...) pr_debug("drm", fmt, ##__VA_ARGS__)
#else
#define drm_dbg(fmt, ...) do { } while (0)
#endif


// ── Global modeset state ─────────────────────────────────────────────
// Protected by a seqlock: readers (GETCRTC + atomic-commit readers in
// future multi-client paths) spin on a sequence counter rather than
// taking a lock.  Writers (drm_commit_apply) take the seqlock's
// internal spinlock so multiple concurrent commits serialize.
//
// This is #2 of the DRM design improvements — Linux's DRM core uses
// drm_modeset_lock_all + per-CRTC locks with a documented acquisition
// order; we use one seqlock for the entire modeset state and get
// wait-free reads for free.
#define DRM_MAX_SCANOUTS  VIRTIO_GPU_MAX_SCANOUTS_CAP

typedef struct drm_scanout_state {
    uint32_t active;          // 1 if a resource is currently scanning out
    uint32_t resource_id;     // backend resource; 0 means disabled
    uint32_t fb_id;           // per-client fb_id used to bind; 0 if none
    uint32_t w, h;
    uint64_t generation;      // bumps on every successful apply
} drm_scanout_state_t;

static seqlock_t          s_state_sl;
static drm_scanout_state_t s_scanouts[DRM_MAX_SCANOUTS];
static int                s_state_init_done = 0;

static void drm_state_init_once(void) {
    if (__atomic_load_n(&s_state_init_done, __ATOMIC_ACQUIRE)) return;
    int expect = 0;
    if (__atomic_compare_exchange_n(&s_state_init_done, &expect, 2,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        seqlock_init(&s_state_sl);
        for (uint32_t i = 0; i < DRM_MAX_SCANOUTS; i++)
            s_scanouts[i] = (drm_scanout_state_t){0};
        __atomic_store_n(&s_state_init_done, 1, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&s_state_init_done, __ATOMIC_ACQUIRE) != 1)
            __builtin_ia32_pause();
    }
}

// Wait-free snapshot of one scanout's state.  Call pattern for
// read-mostly paths (GETCRTC, page-flip event builders).
static drm_scanout_state_t drm_scanout_snapshot(uint32_t sc) {
    drm_scanout_state_t s;
    uint32_t seq;
    do {
        seq = seq_begin(&s_state_sl);
        if (sc < DRM_MAX_SCANOUTS) s = s_scanouts[sc];
        else { __builtin_memset(&s, 0, sizeof(s)); }
    } while (seq_retry(&s_state_sl, seq));
    return s;
}

// ── Memory accounting ────────────────────────────────────────────────
// Every DRM dumb buffer charges its backing allocation to the owning
// task's drm_bytes_charged counter.  Hard cap enforced on create;
// OOM path (kernel/mm/oom.c, future) will consult this to pick
// kill victims.
#define DRM_PER_TASK_LIMIT  (256ull * 1024 * 1024)  // 256 MiB

static int drm_charge(task_t* t, uint64_t bytes) {
    if (!t) return 0;
    uint64_t cur = __atomic_load_n(&t->drm_bytes_charged, __ATOMIC_ACQUIRE);
    for (;;) {
        if (cur + bytes > DRM_PER_TASK_LIMIT) return -ENOMEM;
        if (__atomic_compare_exchange_n(&t->drm_bytes_charged, &cur, cur + bytes,
                                          0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 0;
    }
}

static void drm_uncharge(task_t* t, uint64_t bytes) {
    if (!t) return;
    __atomic_fetch_sub(&t->drm_bytes_charged, bytes, __ATOMIC_ACQ_REL);
}

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
#define DRM_IOCTL_MODE_ATOMIC         0xC03864BC
#define DRM_IOCTL_SET_CLIENT_CAP      0x4010640D
#define DRM_IOCTL_GEM_CLOSE           0x40086409
#define DRM_IOCTL_PRIME_HANDLE_TO_FD  0xC00C642D
#define DRM_IOCTL_PRIME_FD_TO_HANDLE  0xC00C642E
// Property + plane subsystem (Linux-compatible KMS surface).
// Size byte in each code was hand-verified against a sizeof() dump of
// the upstream drm_mode.h structs; the three that were wrong caused
// every GETPROPBLOB / OBJ_SETPROPERTY / CURSOR call to hit the ENOTTY
// default branch and silently break EDID / atomic / cursor paths.
#define DRM_IOCTL_MODE_GETPROPERTY          0xC04064AA  // 64B
#define DRM_IOCTL_MODE_SETPROPERTY          0xC01064AB  // 16B (connector-legacy)
#define DRM_IOCTL_MODE_GETPROPBLOB          0xC01064AC  // 16B
#define DRM_IOCTL_MODE_GETFB                0xC01C64AD  // 28B (legacy)
#define DRM_IOCTL_MODE_CLOSEFB              0xC00864D0  // 8B  (Linux 6.0+)
#define DRM_IOCTL_MODE_GETPLANERESOURCES    0xC01064B5  // 16B
#define DRM_IOCTL_MODE_GETPLANE             0xC02064B6  // 32B
#define DRM_IOCTL_MODE_SETPLANE             0xC03064B7  // 48B
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES    0xC02064B9  // 32B
#define DRM_IOCTL_MODE_OBJ_SETPROPERTY      0xC01864BA  // 24B
#define DRM_IOCTL_MODE_CURSOR               0xC01C64A3  // 28B
#define DRM_IOCTL_MODE_CURSOR2              0xC02464BB  // 36B
// Master + lease — both stubbed.  Returning -EACCES from AUTH_MAGIC
// makes libdrm's drmIsMaster() report "not master", steering wlroots'
// reopen_drm_node() straight to plain-open instead of the lease path.
// CREATE_LEASE returns -EOPNOTSUPP so callers that bypass drmIsMaster
// fall back gracefully rather than aborting.
#define DRM_IOCTL_AUTH_MAGIC                0x40046411
#define DRM_IOCTL_GET_MAGIC                 0x80046402
#define DRM_IOCTL_MODE_CREATE_LEASE         0xC01864C6
#define DRM_IOCTL_MODE_LIST_LESSEES         0xC01064C7
#define DRM_IOCTL_MODE_GET_LEASE            0xC01064C8
#define DRM_IOCTL_MODE_REVOKE_LEASE         0xC00464C9

// ── DRM capability IDs ──────────────────────────────────────────────
#define DRM_CAP_DUMB_BUFFER          0x1
#define DRM_CAP_VBLANK_HIGH_CRTC     0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define DRM_CAP_DUMB_PREFER_SHADOW   0x4
#define DRM_CAP_PRIME                0x5
#define  DRM_PRIME_CAP_IMPORT         0x1
#define  DRM_PRIME_CAP_EXPORT         0x2
#define DRM_CAP_TIMESTAMP_MONOTONIC  0x6
#define DRM_CAP_ASYNC_PAGE_FLIP      0x7
#define DRM_CAP_CURSOR_WIDTH         0x8
#define DRM_CAP_CURSOR_HEIGHT        0x9
#define DRM_CAP_ADDFB2_MODIFIERS     0x10
#define DRM_CAP_PAGE_FLIP_TARGET     0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT 0x12
#define DRM_CAP_SYNCOBJ              0x13
#define DRM_CAP_SYNCOBJ_TIMELINE     0x14

// Client caps (set via DRM_IOCTL_SET_CLIENT_CAP).  Compositor-facing
// flags that ask the kernel to expose more objects or semantics.
#define DRM_CLIENT_CAP_STEREO_3D             1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES      2
#define DRM_CLIENT_CAP_ATOMIC                3
#define DRM_CLIENT_CAP_ASPECT_RATIO          4
#define DRM_CLIENT_CAP_WRITEBACK_CONNECTORS  5
#define DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT  6

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
// Two planes per CRTC: PRIMARY at PLANE_BASE + 2*idx, CURSOR at +1.
// wlroots' cursor rendering expects a dedicated CURSOR plane; without it
// drm_connector_set_cursor silently fails and the cursor never shows.
#define PLANES_PER_CRTC            2u
#define PLANE_BASE                 400u

// Object type tags (DRM_IOCTL_MODE_OBJ_GETPROPERTIES.obj_type).
#define DRM_MODE_OBJECT_CRTC       0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR  0xc0c0c0c0
#define DRM_MODE_OBJECT_ENCODER    0xe0e0e0e0
#define DRM_MODE_OBJECT_PLANE      0xeeeeeeee
#define DRM_MODE_OBJECT_ANY        0

// ── Property IDs ──────────────────────────────────────────────────────
// Fixed numeric IDs; wlroots bsearches by name to resolve them anyway.
// Plane properties: required by atomic + useful for legacy plane queries.
#define PROP_PLANE_TYPE            50u
#define PROP_PLANE_CRTC_ID         51u
#define PROP_PLANE_FB_ID           52u
#define PROP_PLANE_CRTC_X          53u
#define PROP_PLANE_CRTC_Y          54u
#define PROP_PLANE_CRTC_W          55u
#define PROP_PLANE_CRTC_H          56u
#define PROP_PLANE_SRC_X           57u
#define PROP_PLANE_SRC_Y           58u
#define PROP_PLANE_SRC_W           59u
#define PROP_PLANE_SRC_H           60u
#define PROP_PLANE_IN_FORMATS      61u  // blob
// CRTC properties.
#define PROP_CRTC_ACTIVE           70u
#define PROP_CRTC_MODE_ID          71u  // blob
#define PROP_CRTC_GAMMA_LUT_SIZE   72u
#define PROP_CRTC_VRR_ENABLED      73u
// Connector properties.
#define PROP_CONN_DPMS             80u
#define PROP_CONN_EDID             81u  // blob
#define PROP_CONN_CRTC_ID          82u
#define PROP_CONN_NON_DESKTOP      83u
#define PROP_CONN_LINK_STATUS      84u
#define PROP_CONN_PANEL_ORIENTATION 85u
#define PROP_CONN_CONTENT_TYPE     86u
#define PROP_CONN_MAX_BPC          87u

// drm_mode_property.flags (Linux drm_mode.h, kept numerically identical).
#define DRM_MODE_PROP_PENDING      (1u << 0)
#define DRM_MODE_PROP_RANGE        (1u << 1)
#define DRM_MODE_PROP_IMMUTABLE    (1u << 2)
#define DRM_MODE_PROP_ENUM         (1u << 3)
#define DRM_MODE_PROP_BLOB         (1u << 4)
#define DRM_MODE_PROP_BITMASK      (1u << 5)
#define DRM_MODE_PROP_OBJECT      ((1u << 6) | (1u << 1))
#define DRM_MODE_PROP_SIGNED_RANGE ((1u << 6) | (1u << 1))
#define DRM_MODE_PROP_ATOMIC       (1u << 31)

// Plane type enum (Linux drm_plane.h).
#define DRM_PLANE_TYPE_OVERLAY     0u
#define DRM_PLANE_TYPE_PRIMARY     1u
#define DRM_PLANE_TYPE_CURSOR      2u

// DPMS enum.
#define DRM_MODE_DPMS_ON           0u
#define DRM_MODE_DPMS_STANDBY      1u
#define DRM_MODE_DPMS_SUSPEND      2u
#define DRM_MODE_DPMS_OFF          3u

// Well-known blob IDs.  Numbered high enough that they can't collide
// with dumb-buffer handles or future per-fd blobs.
#define BLOB_ID_BASE               0x10000u
#define BLOB_EDID                  (BLOB_ID_BASE + 1)
#define BLOB_IN_FORMATS            (BLOB_ID_BASE + 2)
#define BLOB_MODE_ID               (BLOB_ID_BASE + 3)

// DRM_FORMAT constants (Linux drm_fourcc.h).
#define DRM_FORMAT_XRGB8888        0x34325258u
#define DRM_FORMAT_ARGB8888        0x34325241u
#define DRM_FORMAT_MOD_LINEAR      0ULL

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

// An fb holds its OWN reference to the backing phys + virtio-gpu
// resource so the fb survives after the GEM handle is closed.  This
// matches Linux semantics: wlroots calls close_all_bo_handles right
// after ADDFB2, leaving only the fb_id as the scan-out handle.
typedef struct drm_fb {
    uint32_t           fb_id;          // per-fd ID (1..)
    uint32_t           handle;         // source GEM handle (may be stale)
    uint32_t           width, height;
    uint32_t           pitch;
    uint32_t           format;         // DRM_FORMAT_*
    uint32_t           vgpu_res_id;    // independent scan-out ref
    phys_addr_t        phys;           // backing pages
    uint32_t           bytes_alloc;
    uint8_t            order;
    struct drm_fb*     next;
} drm_fb_t;

// ── DRM event queue ────────────────────────────────────────────────
// read() on a DRM fd delivers struct drm_event_vblank records generated
// after PAGE_FLIP / ATOMIC commits that set DRM_MODE_PAGE_FLIP_EVENT.
// libdrm's drmHandleEvent dispatches these to the compositor's
// page_flip_handler; wlroots needs them to know when to submit the
// next frame.  Without this, wlroots' drmHandleEvent read returns -1
// and wlroots destroys the DRM backend (observed cascade 2026-04-21).
//
// Ring layout: byte-granular so read() can service arbitrary consumer
// buffer sizes.  head = read pointer, tail = write pointer.  Size is
// power-of-two so mod reduces to mask.  256 bytes = 8 page-flip
// events queued simultaneously, plenty for double/triple buffering.

#define DRM_EVENT_VBLANK         0x01
#define DRM_EVENT_FLIP_COMPLETE  0x02
#define DRM_MODE_PAGE_FLIP_EVENT 0x01

#define DRM_EVQ_SIZE 256u
#define DRM_EVQ_MASK (DRM_EVQ_SIZE - 1u)

typedef struct drm_event {
    uint32_t type;
    uint32_t length;
} __attribute__((packed)) drm_event_t;

typedef struct drm_event_vblank {
    drm_event_t base;
    uint64_t    user_data;
    uint32_t    tv_sec;
    uint32_t    tv_usec;
    uint32_t    sequence;
    uint32_t    crtc_id;
} __attribute__((packed)) drm_event_vblank_t;

typedef struct drm_client {
    drm_dumb_t* dumbs;
    drm_fb_t*   fbs;
    uint32_t    next_dumb_handle;      // starts at 1
    uint32_t    next_fb_id;            // starts at 1
    // Event ring (byte buffer).  head/tail are unbounded counters;
    // (tail - head) is live bytes.  Wrap with DRM_EVQ_MASK.
    uint8_t     evq[DRM_EVQ_SIZE];
    uint32_t    evq_head;
    uint32_t    evq_tail;
} drm_client_t;

// Device-global resource id allocator.  Never returns 0 (reserved).
// Start above the IDs the virtio-gpu scanout test reserves (res=1 for the
// startup banner buffer — see virtio_gpu.c:625).  Using 256+ leaves
// headroom for future kernel-reserved IDs without colliding with DRM
// clients' dumb buffers.
static uint32_t s_next_res_id = 256;
static uint32_t alloc_res_id(void) {
    uint32_t r;
    do { r = __atomic_fetch_add(&s_next_res_id, 1, __ATOMIC_RELAXED); }
    while (r == 0);
    return r;
}

static drm_client_t* client_of(vfs_file_t* f) { return (drm_client_t*)f->ctx; }

// Forward-decls for event-queue helpers used by PAGE_FLIP / ATOMIC
// commit paths; full bodies live alongside drm_read_op below.
static void drm_queue_flip_event(vfs_file_t* f, uint32_t crtc_id, uint64_t user_data);

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

// Maximum dumb-buffer width/height.  Caps w*(bpp/8) <= 65536 (fits u32 pitch
// with room) and w*h*(bpp/8) well within u64, so the size math below cannot
// overflow.  Generous: above the advertised GETRESOURCES max (8192), so a
// well-behaved compositor never trips it.
#define DRM_MAX_FB_DIM 16384u

// Compute pitch (bytes/row) and total byte size for a dumb buffer with all
// multiplies in 64-bit, rejecting unsupported bpp, zero/oversized dimensions,
// and any overflow.  Returns 0 + fills *pitch/*size on success, -EINVAL else.
// Pure -> unit-tested (drm_dumb_size_selftest).  Fixes the prior
// `uint32_t pitch = width * 4` which overflowed in 32-bit BEFORE the cast,
// yielding a tiny backing buffer for a huge resource (host OOB on transfer).
static int drm_dumb_size(uint32_t w, uint32_t h, uint32_t bpp,
                         uint32_t* pitch, uint64_t* size) {
    if (!w || !h || bpp != 32)                    return -EINVAL;
    if (w > DRM_MAX_FB_DIM || h > DRM_MAX_FB_DIM)  return -EINVAL;
    uint64_t p = (uint64_t)w * (bpp / 8u);   // 64-bit: no truncation
    uint64_t s = p * (uint64_t)h;
    *pitch = (uint32_t)p;                    // safe: p <= 16384*4 = 65536
    *size  = s;
    return 0;
}

// PRIMITIVE (drm cursor backing-size check).  An ARGB (32bpp) cursor of
// width*height pixels needs width*height*4 bytes, which must fit the dumb
// buffer that backs it.  Returns true + the byte size in *bytes on success.
// Overflow-safe by construction: width*height is a u64 product of two u32 (it
// can never overflow u64), and the *4 is guarded by ckd_mul_u64.  A bare
// `(uint64_t)w*h*4 > backing` guard WRAPS u64 once w*h >= 2^62 (e.g.
// w=h=0x80000000 -> *4 == 2^64 -> 0), so the bound is bypassed and absurd
// dimensions reach resource_create/resource_transfer (host-side OOB) and a
// wrapped-to-zero pin-page count.  Pure -> unit-tested (drm_cursor_bytes_
// selftest).  NOT for non-32bpp formats.
static inline bool drm_cursor_bytes(uint32_t width, uint32_t height,
                                    uint64_t backing_size, uint64_t* bytes) {
    if (!width || !height) return false;
    uint64_t need;
    if (!ckd_mul_u64((uint64_t)width * height, 4u, &need)) return false;
    if (need > backing_size) return false;
    *bytes = need;
    return true;
}

static int drm_ioctl_create_dumb(vfs_file_t* f, uint64_t arg) {
    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    if (!b) return -ENODEV;

    drm_mode_create_dumb_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;

    // Validate dimensions + compute pitch/size with overflow-safe 64-bit math
    // (rejects zero/oversized w/h and bpp != 32).  a.width/height come straight
    // from copy_from_user, so this is the gate that keeps the backing allocation
    // consistent with the width/height handed to resource_create below.
    uint32_t pitch;
    uint64_t size;
    if (drm_dumb_size(a.width, a.height, a.bpp, &pitch, &size) != 0) return -EINVAL;
    uint64_t pages = (size + 4095) / 4096;
    uint8_t  order = 0;
    while (((uint64_t)1 << order) < pages) order++;
    uint32_t bytes_alloc = (uint32_t)((uint64_t)1 << order) * 4096u;

    // Memory-accounting check BEFORE the buddy alloc — reject early
    // if this client is over quota.  Applies to both real userland
    // tasks and kthreads; kthreads have drm_bytes_charged=0 by default.
    int rc = drm_charge(g_current, bytes_alloc);
    if (rc < 0) return rc;

    phys_addr_t phys = pmm_buddy_alloc(order);
    if (phys == PMM_INVALID_ADDR) { drm_uncharge(g_current, bytes_alloc); return -ENOMEM; }

    uint8_t* virt = (uint8_t*)((uintptr_t)phys + HHDM_OFFSET);
    __builtin_memset(virt, 0, bytes_alloc);

    // The allocator already stamps every page of the block at rc=1 — that
    // IS this dumb buffer's reference.  An extra per-page pmm_ref_inc here
    // double-counted, so DESTROY_DUMB's single pmm_ref_dec only reached
    // rc=1 and the framebuffer pages leaked forever.  fb (ADDFB2) and
    // GETFB-minted handles take their OWN +1 each, so the page frees only
    // when the dumb AND every fb/handle have released — exactly right.

    uint32_t res_id = alloc_res_id();
    // Error paths free the block via per-page pmm_ref_dec (alloc rc=1 →
    // 0), NOT pmm_buddy_free at block order — pmm_ref_dec already returns
    // each page to the buddy when its refcount hits zero.
    if (b->resource_create(res_id, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
                             a.width, a.height) != 0) {
        for (uint32_t i = 0; i < bytes_alloc / 4096u; i++)
            pmm_ref_dec(phys + (phys_addr_t)i * 4096u);
        drm_uncharge(g_current, bytes_alloc);
        return -EIO;
    }
    if (b->resource_attach_backing(res_id, phys, bytes_alloc) != 0) {
        b->resource_destroy(res_id);
        for (uint32_t i = 0; i < bytes_alloc / 4096u; i++)
            pmm_ref_dec(phys + (phys_addr_t)i * 4096u);
        drm_uncharge(g_current, bytes_alloc);
        return -EIO;
    }

    drm_dumb_t* d = (drm_dumb_t*)kmalloc(sizeof(*d));
    if (!d) {
        b->resource_destroy(res_id);
        pmm_buddy_free(phys, order);
        drm_uncharge(g_current, bytes_alloc);
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

static void dumb_free(drm_dumb_t* d, task_t* owner) {
    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    // vgpu_res_id==0 marks a handle minted by GETFB around an fb's
    // backing — it owns page refs but no virtio-gpu resource.
    if (b && d->vgpu_res_id) b->resource_destroy(d->vgpu_res_id);
    // pmm_ref_dec is the authoritative free path: it returns each
    // page to the buddy allocator when its refcount hits 0.  A fb
    // created via ADDFB2 took its OWN ref on every backing page, so
    // when the dumb's handle is destroyed while the fb still holds
    // the memory (wlroots' close_all_bo_handles pattern), ref counts
    // go 2→1 and the pages stay live until drm_fb_free drops the
    // last ref.  The previous code also called pmm_buddy_free here
    // which double-freed the pages while they were still in use —
    // buddy's freelist `next` pointer at offset 0 of the block got
    // overwritten by the live fb's pixel writes → the next buddy
    // alloc chased a corrupted `next` → #GP.
    for (uint32_t i = 0; i < d->bytes_alloc / 4096u; i++)
        pmm_ref_dec(d->phys + (phys_addr_t)i * 4096u);
    drm_uncharge(owner, d->bytes_alloc);
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
            // Linux semantics: closing the GEM handle does NOT remove
            // framebuffers that reference it — each fb holds its own
            // ref on the backing pages (taken at ADDFB2) and survives
            // independently.  wlroots' close_all_bo_handles relies on
            // this to drop its handles right after ADDFB2.
            *pp = d->next;
            dumb_free(d, g_current);
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
    drm_dbg("ADDFB2 %ux%u fmt=0x%x handles[0]=%u pitches[0]=%u",
            a.width, a.height, a.pixel_format, a.handles[0], a.pitches[0]);
    if (!a.handles[0]) {
        pr_warn("drm", "ADDFB2: handles[0]=0");
        return -EINVAL;
    }
    drm_client_t* c = client_of(f);
    drm_dumb_t* d = find_dumb(c, a.handles[0]);
    if (!d) {
        pr_warn("drm", "ADDFB2: handle %u not found", a.handles[0]);
        return -ENOENT;
    }
    if (a.width > d->width || a.height > d->height) {
        pr_warn("drm", "ADDFB2: size %ux%u > dumb %ux%u",
                a.width, a.height, d->width, d->height);
        return -EINVAL;
    }
    TRACE(TRACE_DRM_ADDFB, a.width, a.height, a.pixel_format, a.handles[0]);

        // Allocate an INDEPENDENT virtio-gpu resource for this fb, backed
    // by the same phys pages.  This lets dumb and fb be destroyed on
    // their own schedules (wlroots closes the GEM handle immediately
    // after ADDFB2; the fb must survive).
    const drm_backend_ops_t* b2 = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t fb_res_id = alloc_res_id();
    if (!b2 ||
        b2->resource_create(fb_res_id,
                            VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
                            a.width, a.height) != 0 ||
        b2->resource_attach_backing(fb_res_id, d->phys, d->bytes_alloc) != 0) {
        if (b2) b2->resource_destroy(fb_res_id);
        return -EIO;
    }

    drm_fb_t* fb = (drm_fb_t*)kmalloc(sizeof(*fb));
    if (!fb) {
        b2->resource_destroy(fb_res_id);
        return -ENOMEM;
    }
    fb->fb_id       = c->next_fb_id++;
    fb->handle      = a.handles[0];
    fb->width       = a.width;
    fb->height      = a.height;
    fb->pitch       = a.pitches[0] ? a.pitches[0] : d->pitch;
    fb->format      = a.pixel_format;
    fb->vgpu_res_id = fb_res_id;
    fb->phys        = d->phys;
    fb->bytes_alloc = d->bytes_alloc;
    fb->order       = d->order;
    // Take independent refs on every backing page so the fb survives
    // close_all_bo_handles() even if the dumb (and its refs) is freed.
    for (uint32_t i = 0; i < d->bytes_alloc / 4096u; i++)
        pmm_ref_inc(d->phys + (phys_addr_t)i * 4096u);
    fb->next   = c->fbs;
    c->fbs     = fb;
    pr_info("drm", "ADDFB2 fb=%u res=%u phys=%p %ux%u",
            fb->fb_id, fb->vgpu_res_id, (void*)fb->phys, a.width, a.height);

    a.fb_id = fb->fb_id;
    if (copy_to_user((void*)arg, &a, sizeof(a)) != 0) return -EFAULT;
    return 0;
}

typedef struct { uint32_t fb_id; } drm_mode_rmfb_t;

// Release an fb: destroy its virtio-gpu resource + drop per-page refs.
// If the resource is STILL active on a scanout, do NOT destroy it — the
// host would lose the backing and the screen would revert mid-frame.
// wlroots legitimately closes the fb after the commit (the swapchain
// rotates buffers while the scanout keeps the old one visible); the
// fb is reaped later when a new commit replaces this resource on the
// scanout, via drm_commit_apply's prior-state handoff.  The kfree(fb)
// here is safe because the resource is still tracked by s_scanouts.
static void drm_fb_free(drm_fb_t* fb) {
    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    int active = 0;
    for (uint32_t i = 0; i < DRM_MAX_SCANOUTS; i++)
        if (s_scanouts[i].resource_id == fb->vgpu_res_id) { active = 1; break; }
    if (!active) {
        if (b && fb->vgpu_res_id) b->resource_destroy(fb->vgpu_res_id);
        for (uint32_t i = 0; i < fb->bytes_alloc / 4096u; i++)
            pmm_ref_dec(fb->phys + (phys_addr_t)i * 4096u);
    }
    // else: resource kept alive for the scanout; on the next commit
    // that replaces this resource, drm_commit_apply's prior hand-off
    // should destroy it (TODO: wire that path).  Leak-bound: one fb
    // per scanout steady-state.
    kfree(fb);
}

static int drm_ioctl_rmfb(vfs_file_t* f, uint64_t arg) {
    drm_mode_rmfb_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    drm_client_t* c = client_of(f);
    drm_fb_t** pp = &c->fbs;
    while (*pp) {
        if ((*pp)->fb_id == a.fb_id) {
            drm_fb_t* fb = *pp;
            *pp = fb->next;
            drm_fb_free(fb);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -ENOENT;
}

// ── Atomic commit — the single state-changing primitive ─────────────
//
// Every modeset (SETCRTC, PAGE_FLIP, MODE_ATOMIC) goes through this
// function.  Legacy sync ioctls are thin wrappers that build a
// one-scanout commit and call drm_commit_apply().  Linux carries
// parallel sync + atomic code paths forever; we only have one.
//
// Snapshot + rollback semantics: we snapshot s_scanouts BEFORE
// touching the backend.  Any backend call failing rolls every prior
// change back with the backend's own primitives (SET_SCANOUT + old
// resource_id).  Either the full commit applies, or nothing did.

typedef struct drm_commit_entry {
    uint32_t scanout;          // 0..n-1
    uint32_t resource_id;      // backend id; 0 disables
    uint32_t fb_id;            // tracked for observability
    uint32_t w, h;
} drm_commit_entry_t;

typedef struct drm_commit {
    uint32_t n;
    drm_commit_entry_t entries[DRM_MAX_SCANOUTS];
} drm_commit_t;

static int drm_commit_apply(const drm_commit_t* commit) {
    drm_state_init_once();
    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    if (!b) {
        pr_warn("drm", "commit denied: no backend registered");
        return -ENODEV;
    }
    pr_info("drm", "commit n=%u scanout[0]={res=%u fb=%u %ux%u}",
            commit->n,
            commit->n ? commit->entries[0].resource_id : 0,
            commit->n ? commit->entries[0].fb_id       : 0,
            commit->n ? commit->entries[0].w           : 0,
            commit->n ? commit->entries[0].h           : 0);
    TRACE(TRACE_DRM_COMMIT,
          commit->n,
          commit->n ? commit->entries[0].resource_id : 0,
          commit->n ? ((uint64_t)commit->entries[0].w << 32 | commit->entries[0].h) : 0,
          0);

    // Writer: takes the seqlock's internal spinlock.  During
    // seq_write_begin..seq_write_end the seq counter is odd; readers
    // spin-retry until we finish.  Every bumped field is visible on
    // seq_write_end via the built-in release fence.
    seq_write_begin(&s_state_sl);

    drm_scanout_state_t prior[DRM_MAX_SCANOUTS];
    for (uint32_t i = 0; i < commit->n; i++) {
        uint32_t sc = commit->entries[i].scanout;
        if (sc >= DRM_MAX_SCANOUTS) { seq_write_end(&s_state_sl); return -EINVAL; }
        prior[i] = s_scanouts[sc];
    }

    for (uint32_t i = 0; i < commit->n; i++) {
        const drm_commit_entry_t* e = &commit->entries[i];
        if (b->scanout_set(e->scanout, e->resource_id, e->w, e->h) != 0) {
            pr_err("drm", "commit: backend scanout_set(sc=%u res=%u) failed",
                   e->scanout, e->resource_id);
            goto rollback;
        }
        if (e->resource_id) {
            if (b->resource_transfer(e->resource_id, e->w, e->h) != 0) {
                pr_err("drm", "commit: resource_transfer(res=%u) failed", e->resource_id);
                goto rollback;
            }
            if (b->resource_flush   (e->resource_id, e->w, e->h) != 0) {
                pr_err("drm", "commit: resource_flush(res=%u) failed", e->resource_id);
                goto rollback;
            }
            drm_dbg("commit: scanout=%u res=%u %ux%u applied",
                     e->scanout, e->resource_id, e->w, e->h);
        } else {
            drm_dbg("commit: scanout=%u disabled", e->scanout);
        }
        s_scanouts[e->scanout] = (drm_scanout_state_t){
            .active      = (e->resource_id != 0),
            .resource_id = e->resource_id,
            .fb_id       = e->fb_id,
            .w = e->w, .h = e->h,
            .generation  = s_scanouts[e->scanout].generation + 1,
        };
    }

    seq_write_end(&s_state_sl);
    return 0;

rollback:
    for (uint32_t j = 0; j < commit->n; j++) {
        const drm_commit_entry_t* e = &commit->entries[j];
        if (s_scanouts[e->scanout].generation != prior[j].generation) {
            b->scanout_set(e->scanout, prior[j].resource_id, prior[j].w, prior[j].h);
            s_scanouts[e->scanout] = prior[j];
        }
    }
    seq_write_end(&s_state_sl);
    return -EIO;
}

// ── Legacy sync ioctls — one-entry commits on top of atomic ─────────

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

// Resolve a (client, fb_id) pair to the backend data the commit needs.
// Uses the fb's own snapshot — does NOT chase fb->handle (the source
// GEM handle may have been closed via close_all_bo_handles).
static int resolve_fb(drm_client_t* c, uint32_t fb_id,
                       uint32_t* out_res, uint32_t* out_w, uint32_t* out_h) {
    if (fb_id == 0) { *out_res = 0; *out_w = 0; *out_h = 0; return 0; }
    drm_fb_t* fb = find_fb(c, fb_id);
    if (!fb) return -ENOENT;
    *out_res = fb->vgpu_res_id;
    *out_w   = fb->width;
    *out_h   = fb->height;
    return 0;
}

static int drm_ioctl_setcrtc(vfs_file_t* f, uint64_t arg) {
    drm_mode_crtc_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    if (!b) return -ENODEV;
    uint32_t n_sc = b->scanout_count();
    if (a.crtc_id < CRTC_BASE || a.crtc_id >= CRTC_BASE + n_sc) return -ENOENT;

    drm_commit_t commit = { .n = 1 };
    commit.entries[0].scanout = a.crtc_id - CRTC_BASE;
    commit.entries[0].fb_id   = a.fb_id;
    int rc = resolve_fb(client_of(f), a.fb_id,
                         &commit.entries[0].resource_id,
                         &commit.entries[0].w,
                         &commit.entries[0].h);
    if (rc != 0) return rc;
    return drm_commit_apply(&commit);
}

typedef struct {
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
} drm_mode_page_flip_t;

static int drm_ioctl_page_flip(vfs_file_t* f, uint64_t arg) {
    drm_mode_page_flip_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    if (!b) return -ENODEV;
    uint32_t n_sc = b->scanout_count();
    if (a.crtc_id < CRTC_BASE || a.crtc_id >= CRTC_BASE + n_sc) return -ENOENT;

    drm_commit_t commit = { .n = 1 };
    commit.entries[0].scanout = a.crtc_id - CRTC_BASE;
    commit.entries[0].fb_id   = a.fb_id;
    int rc = resolve_fb(client_of(f), a.fb_id,
                         &commit.entries[0].resource_id,
                         &commit.entries[0].w,
                         &commit.entries[0].h);
    if (rc != 0) return rc;
    int r = drm_commit_apply(&commit);
    // Userland asked for a flip-complete event (DRM_MODE_PAGE_FLIP_EVENT).
    // Our commit is synchronous — queue the event now so libdrm's
    // drmHandleEvent reads it on the fd and dispatches to the
    // compositor's page_flip_handler.  Without this, wlroots reads
    // nothing, drmHandleEvent returns -1, wlroots destroys the whole
    // backend.
    if (r == 0 && (a.flags & DRM_MODE_PAGE_FLIP_EVENT)) {
        drm_queue_flip_event(f, a.crtc_id, a.user_data);
    }
    return r;
}

// ── MODE_ATOMIC ioctl — the first-class path ────────────────────────
// Linux struct drm_mode_atomic.  For this initial drop we accept the
// struct but only parse enough to build a drm_commit_t: objects array
// mapping (crtc_id → fb_id).  Full property graph parsing comes when
// wlroots actually exercises it.
typedef struct {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;         // array of u32 object ids
    uint64_t count_props_ptr;  // array of u32 per object
    uint64_t props_ptr;        // flat array of u32 property ids
    uint64_t prop_values_ptr;  // flat array of u64 property values
    uint64_t reserved;
    uint64_t user_data;
} drm_mode_atomic_t;

#define DRM_MODE_PROP_CRTC_FB   1
#define DRM_MODE_PROP_CRTC_MODE 2

// Max per-object property count an atomic commit may carry.  A real CRTC/plane
// has well under 32 properties; 256 is generous headroom for any compositor
// while bounding the per-object prop loop so an attacker-supplied counts[i]
// cannot drive a multi-billion-iteration syscall (DoS) or overflow props_off /
// values_off.  Pure -> unit-tested (drm_atomic_count_selftest).
#define DRM_MAX_PROPS_PER_OBJ 256u
static inline int drm_atomic_count_ok(uint32_t count) {
    return count <= DRM_MAX_PROPS_PER_OBJ;
}

static int drm_ioctl_atomic(vfs_file_t* f, uint64_t arg) {
    drm_mode_atomic_t a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    if (a.count_objs == 0) return 0;
    if (a.count_objs > DRM_MAX_SCANOUTS) return -EINVAL;

    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    if (!b) return -ENODEV;
    uint32_t n_sc = b->scanout_count();

    // Pull the objs array into a kernel buffer.
    uint32_t objs[DRM_MAX_SCANOUTS];
    if (copy_from_user(objs, (void*)a.objs_ptr,
                        a.count_objs * sizeof(uint32_t)) != 0) return -EFAULT;
    // count_props_ptr: per-object u32 property count.
    uint32_t counts[DRM_MAX_SCANOUTS];
    if (copy_from_user(counts, (void*)a.count_props_ptr,
                        a.count_objs * sizeof(uint32_t)) != 0) return -EFAULT;
    // Bound each per-object property count: an unbounded counts[i] would drive
    // the inner prop loop up to 2^32 iterations (DoS) and overflow props_off.
    for (uint32_t i = 0; i < a.count_objs; i++)
        if (!drm_atomic_count_ok(counts[i])) return -EINVAL;

    // For each CRTC object, pull its properties and extract the fb id.
    drm_commit_t commit = { .n = 0 };
    drm_client_t* c = client_of(f);
    uint64_t props_off   = 0;
    uint64_t values_off  = 0;
    for (uint32_t i = 0; i < a.count_objs; i++) {
        uint32_t obj_id = objs[i];
        if (obj_id < CRTC_BASE || obj_id >= CRTC_BASE + n_sc) {
            // Not a CRTC — skip its props (we don't yet model planes).
            props_off  += counts[i] * sizeof(uint32_t);
            values_off += counts[i] * sizeof(uint64_t);
            continue;
        }
        uint32_t fb_id = 0;
        for (uint32_t j = 0; j < counts[i]; j++) {
            uint32_t prop_id;
            uint64_t prop_val;
            if (copy_from_user(&prop_id, (void*)(a.props_ptr + props_off),
                                sizeof(prop_id)) != 0) return -EFAULT;
            if (copy_from_user(&prop_val, (void*)(a.prop_values_ptr + values_off),
                                sizeof(prop_val)) != 0) return -EFAULT;
            props_off  += sizeof(uint32_t);
            values_off += sizeof(uint64_t);
            if (prop_id == DRM_MODE_PROP_CRTC_FB) fb_id = (uint32_t)prop_val;
        }
        drm_commit_entry_t* e = &commit.entries[commit.n++];
        e->scanout = obj_id - CRTC_BASE;
        e->fb_id   = fb_id;
        int rc = resolve_fb(c, fb_id, &e->resource_id, &e->w, &e->h);
        if (rc != 0) return rc;
    }
    int r = drm_commit_apply(&commit);
    // DRM_MODE_ATOMIC_EVENT_FLIP flag (0x02) requests a flip-complete
    // event per CRTC, user_data=a.user_data.  Same constant as PAGE_FLIP
    // flags DRM_MODE_PAGE_FLIP_EVENT=0x1 wait — the atomic flag set is:
    //   DRM_MODE_ATOMIC_TEST_ONLY  0x0100
    //   DRM_MODE_ATOMIC_NONBLOCK   0x0200
    //   DRM_MODE_PAGE_FLIP_EVENT   0x0001  (reused from legacy)
    //   DRM_MODE_PAGE_FLIP_ASYNC   0x0002
    //   DRM_MODE_ATOMIC_ALLOW_MODESET 0x0400
    if (r == 0 && (a.flags & DRM_MODE_PAGE_FLIP_EVENT)) {
        for (uint32_t i = 0; i < commit.n; i++) {
            drm_queue_flip_event(f, commit.entries[i].scanout + CRTC_BASE,
                                 a.user_data);
        }
    }
    return r;
}

// ── mmap resolver (called from sys_mmap) ─────────────────────────────
// Given a DRM fd and the offset from MAP_DUMB, return the backing
// physical address + byte count.  Returns 0 on success, -errno on
// failure.  The caller's sys_mmap installs the PTEs.
int64_t drm_resolve_dumb_mmap(vfs_file_t* f, uint64_t offset,
                                uint64_t len, phys_addr_t* out_phys,
                                uint64_t* out_bytes) {
    if ((offset & 0xFF00000000000000ull) != DRM_DUMB_OFFSET_MARK) return -EINVAL;
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
    case DRM_CAP_PRIME:                c.value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; break;
    case DRM_CAP_TIMESTAMP_MONOTONIC:  c.value = 1; break;
    case DRM_CAP_ASYNC_PAGE_FLIP:      c.value = 0; break;
    case DRM_CAP_CURSOR_WIDTH:         c.value = 64; break;
    case DRM_CAP_CURSOR_HEIGHT:        c.value = 64; break;
    case DRM_CAP_ADDFB2_MODIFIERS:     c.value = 0; break;
    case DRM_CAP_PAGE_FLIP_TARGET:     c.value = 0; break;
    case DRM_CAP_CRTC_IN_VBLANK_EVENT: c.value = 1; break;
    case DRM_CAP_SYNCOBJ:              c.value = 0; break;
    case DRM_CAP_SYNCOBJ_TIMELINE:     c.value = 0; break;
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

    const drm_backend_ops_t* b_vtbl = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t n_sc = b_vtbl ? b_vtbl->scanout_count() : 0;
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
    if (b_vtbl) b_vtbl->scanout_mode(0, &w0, &h0);
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

    const drm_backend_ops_t* b_vtbl = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t n_sc = b_vtbl ? b_vtbl->scanout_count() : 0;
    if (c.connector_id < CONN_BASE || c.connector_id >= CONN_BASE + n_sc)
        return -ENOENT;
    uint32_t idx = c.connector_id - CONN_BASE;

    uint32_t w = 0, h = 0;
    if (b_vtbl) b_vtbl->scanout_mode(idx, &w, &h);

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

    const drm_backend_ops_t* b_vtbl = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t n_sc = b_vtbl ? b_vtbl->scanout_count() : 0;
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

    const drm_backend_ops_t* b_vtbl = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t n_sc = b_vtbl ? b_vtbl->scanout_count() : 0;
    if (c.crtc_id < CRTC_BASE || c.crtc_id >= CRTC_BASE + n_sc) return -ENOENT;
    uint32_t scanout = c.crtc_id - CRTC_BASE;

    // Wait-free seqlock read.  No writers are delayed by us; if a
    // commit is in flight, we spin-retry inside drm_scanout_snapshot.
    drm_state_init_once();
    drm_scanout_state_t cur = drm_scanout_snapshot(scanout);

    c.fb_id        = cur.fb_id;
    c.x = c.y      = 0;
    c.gamma_size   = 0;
    c.mode_valid   = cur.active ? 1 : 0;
    __builtin_memset(&c.mode, 0, sizeof(c.mode));
    if (cur.active) {
        c.mode.hdisplay = (uint16_t)cur.w;
        c.mode.vdisplay = (uint16_t)cur.h;
        c.mode.vrefresh = 60;
        c.mode.type     = 0x40;   // DRM_MODE_TYPE_PREFERRED
    }
    c.count_connectors = 0;

    if (copy_to_user((void*)arg, &c, sizeof(c)) != 0) return -EFAULT;
    return 0;
}

// ── SET_CLIENT_CAP ──────────────────────────────────────────────────
// Accepts UNIVERSAL_PLANES, ASPECT_RATIO, CURSOR_PLANE_HOTSPOT (all
// either already true for us or no-op).  Rejects ATOMIC with -EOPNOTSUPP
// so wlroots falls back to the legacy DRM path — our kernel implements
// drmModeSetCrtc + drmModePageFlip + drmModeAddFB2 but not the full
// property/blob/plane subsystem the atomic interface demands.  Rejects
// STEREO_3D and WRITEBACK_CONNECTORS (we don't model them).
static int drm_ioctl_set_client_cap(uint64_t arg) {
    struct { uint64_t cap; uint64_t val; } a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    switch (a.cap) {
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
    case DRM_CLIENT_CAP_ASPECT_RATIO:
    case DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT:
        return 0;
    case DRM_CLIENT_CAP_ATOMIC:
        return -EOPNOTSUPP;
    default:
        return -EINVAL;
    }
}

// ── GEM_CLOSE ───────────────────────────────────────────────────────
// Release a buffer-object handle.  Delegates to the existing dumb-buffer
// destruction path — in our model every GEM handle is a dumb buffer.
static int drm_ioctl_destroy_dumb(vfs_file_t* f, uint64_t arg);   // fwd
static int drm_ioctl_gem_close(vfs_file_t* self, uint64_t arg) {
    return drm_ioctl_destroy_dumb(self, arg);
}

// ── PRIME fd type ───────────────────────────────────────────────────
// A PRIME-exported fd is a small vfs_file_t that carries (client, handle).
// When passed back to PRIME_FD_TO_HANDLE on the same DRM fd, the handle
// is recovered.  The dmabuf fd does NOT keep the underlying buffer alive
// — lifetime is managed by the DRM client's GEM handle table, matching
// Linux semantics where handle + PRIME fd are independent refs.
typedef struct {
    drm_client_t* owner;   // back-ref to the DRM client that exported us
    uint32_t      handle;
} drm_prime_ctx_t;

static void drm_prime_close(vfs_file_t* self) {
    if (self->ctx) kfree(self->ctx);
    kfree(self);
}

// Distinctive close-pointer lets FD_TO_HANDLE confirm an fd came from
// our PRIME export (as opposed to an arbitrary fd the caller passed in).
static int drm_prime_is_ours(vfs_file_t* f) {
    return f && f->close == drm_prime_close;
}

// Install a ready vfs_file_t into the current task's fd_table at the
// lowest free slot.  Returns the new fd, or -errno.
static int drm_install_fd(vfs_file_t* f) {
    task_files_t* tf = g_current->files_shared;
    if (!tf) return -EBADF;
    spin_lock(&tf->lock);
    for (uint32_t fd = 0; ; fd++) {
        if (fd >= tf->ft->cap) {
            if (!fd_table_grow(tf)) { spin_unlock(&tf->lock); return -ENFILE; }
        }
        if (!tf->ft->fd_table[fd]) {
            tf->ft->fd_table[fd] = f;
            tf->ft->fd_flags[fd] = 0;
            spin_unlock(&tf->lock);
            return (int)fd;
        }
    }
}

static int drm_ioctl_prime_handle_to_fd(vfs_file_t* drm_f, uint64_t arg) {
    struct { uint32_t handle; uint32_t flags; int32_t fd; } a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;

    drm_client_t* c = client_of(drm_f);
    if (!find_dumb(c, a.handle)) return -ENOENT;

    // Allocate the PRIME vfs_file_t.
    vfs_file_t* pf = vfs_alloc_file();   // zeroed, waitq wired, refcount=1
    if (!pf) return -ENOMEM;
    pf->close  = drm_prime_close;
    pf->rights = 0xFFFFFFFFu;   // non-default: DRM PRIME fd carries all rights

    drm_prime_ctx_t* pc = (drm_prime_ctx_t*)kmalloc(sizeof(*pc));
    if (!pc) { kfree(pf); return -ENOMEM; }
    pc->owner  = c;
    pc->handle = a.handle;
    pf->ctx = pc;

    int fd = drm_install_fd(pf);
    if (fd < 0) { kfree(pc); kfree(pf); return fd; }

    a.fd = fd;
    if (copy_to_user((void*)arg, &a, sizeof(a)) != 0) return -EFAULT;
    return 0;
}

// Cross-fd PRIME import: wlroots reopens /dev/dri/card0 for its dumb
// allocator, then passes the exported dmabuf fd to the DRM backend's
// original fd for scan-out.  Those fds are different drm_client_t's
// backed by the same device, so importing must CLONE the underlying
// dumb buffer into the destination client's handle table (sharing the
// phys pages, with refcounts bumped per page).  Same-client import
// just returns the original handle.
static int drm_ioctl_prime_fd_to_handle(vfs_file_t* drm_f, uint64_t arg) {
    struct { uint32_t handle; uint32_t flags; int32_t fd; } a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;

    task_files_t* tf = g_current->files_shared;
    if (!tf || a.fd < 0 || (uint32_t)a.fd >= tf->ft->cap) return -EBADF;
    vfs_file_t* pf = tf->ft->fd_table[a.fd];
    if (!drm_prime_is_ours(pf)) return -EINVAL;

    drm_prime_ctx_t* pc = (drm_prime_ctx_t*)pf->ctx;
    drm_client_t* dst = client_of(drm_f);

    if (pc->owner == dst) {
        a.handle = pc->handle;
    } else {
        // Different DRM client — import.  Locate the source dumb in the
        // exporting client's list; clone a drm_dumb_t in the destination
        // referencing the same phys pages.  Bump per-page refcounts so
        // destroy_dumb on either side leaves the other's view intact.
        drm_dumb_t* src = find_dumb(pc->owner, pc->handle);
        if (!src) return -ENOENT;
        // Charge the importer: dumb_free unconditionally uncharges, so
        // an uncharged clone would drift the quota negative over time.
        if (drm_charge(g_current, src->bytes_alloc) != 0) return -ENOMEM;
        drm_dumb_t* nd = (drm_dumb_t*)kmalloc(sizeof(*nd));
        if (!nd) { drm_uncharge(g_current, src->bytes_alloc); return -ENOMEM; }
        *nd = *src;
        nd->handle = dst->next_dumb_handle++;
        // The clone is a HANDLE around shared pages, not a second
        // owner of the source's virtio-gpu resource: if it inherited
        // vgpu_res_id, GEM_CLOSE of the clone would destroy the
        // source's host resource, and the source's own destroy would
        // then double-UNREF it.  ADDFB2 mints its own resource anyway.
        nd->vgpu_res_id = 0;
        nd->next   = dst->dumbs;
        dst->dumbs = nd;
        for (uint32_t i = 0; i < nd->bytes_alloc / 4096u; i++)
            pmm_ref_inc(nd->phys + (phys_addr_t)i * 4096u);
        a.handle = nd->handle;
    }

    if (copy_to_user((void*)arg, &a, sizeof(a)) != 0) return -EFAULT;
    return 0;
}

// ──────────────────────────────────────────────────────────────────────
// Property / plane / blob subsystem — full legacy+atomic-readable KMS
// surface.  Every property wlroots / dwl / tinywl scan for is answered
// with real metadata (name, flags, enum tables) and real values.
// Connectors advertise an EDID blob, CRTCs advertise a MODE_ID blob
// for the current mode, planes advertise an IN_FORMATS blob listing
// their supported fourcc+modifier pairs.
// ──────────────────────────────────────────────────────────────────────

static inline uint32_t plane_of(uint32_t crtc_idx, uint8_t is_cursor) {
    return PLANE_BASE + crtc_idx * PLANES_PER_CRTC + (is_cursor ? 1u : 0u);
}

// ── EDID blob (128 bytes) for a generic 1280x800 @ 60Hz monitor ───────
// Hand-computed: 8-byte magic, manufacturer "MAK" (MakaOS), product 1,
// serial 1, week 1 year 2024, EDID 1.4, digital input, 10-bit color,
// 310×170 mm (13.8in), sRGB default, 1280x800 60Hz detailed timing,
// one extension block flag cleared, checksum correct.
static uint8_t g_edid[128] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,   // magic
    0x33, 0x21, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,   // mfg "MAK" + product/serial
    0x01, 0x22, 0x01, 0x04,                           // week 1 / year 2024 / EDID 1.4
    0xA5, 0x1F, 0x11, 0x78,                           // digital, 10bpc, 31x17cm, sRGB
    0x06, 0xEE, 0x95, 0xA3, 0x54, 0x4C, 0x99, 0x26,
    0x0F, 0x50, 0x54, 0x00, 0x00, 0x00,               // chroma + established timings=0
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,   // standard timings unused
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    // Detailed timing #1 — 1280x800 @ 60Hz reduced-blanking-ish.
    0x64, 0x19, 0x00, 0x40, 0x41, 0x00, 0x26, 0x30,
    0x18, 0x88, 0x36, 0x00, 0x28, 0x8E, 0x21, 0x00,
    0x00, 0x1E,
    // Monitor name "MakaOS" — 18-byte descriptor: 5-byte header
    // (00 00 00 FC 00) + 13 bytes of name padded with 0x0a then 0x20.
    0x00, 0x00, 0x00, 0xFC, 0x00, 'M','a','k','a','O','S','\n',' ',' ',' ',' ',' ',' ',
    // Monitor range: 30-60Hz
    0x00, 0x00, 0x00, 0xFD, 0x00, 0x1E, 0x3C, 0x1E, 0x3C, 0x1E, 0x01, 0x0A,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    // Serial string
    0x00, 0x00, 0x00, 0xFF, 0x00, 'M','A','K','A','0','0','0','1','\n',' ',' ',' ',' ',
    0x00,                                             // extension count
    0x00                                              // checksum (fixed up at init)
};

// ── IN_FORMATS blob — XRGB8888 with DRM_FORMAT_MOD_LINEAR ───────────
// Wire format: drm_format_modifier_blob header, then formats[] then
// modifiers[] with per-modifier formats bitmask.  Linux drm_mode.h
// encodes this verbatim.
struct __attribute__((packed)) drm_format_modifier {
    uint64_t formats;           // bitmask over formats[]
    uint32_t offset;            // unused
    uint32_t pad;
    uint64_t modifier;
};
struct __attribute__((packed)) drm_format_modifier_blob_hdr {
    uint32_t version;           // 1
    uint32_t flags;             // 0
    uint32_t count_formats;
    uint32_t formats_offset;
    uint32_t count_modifiers;
    uint32_t modifiers_offset;
};
struct __attribute__((packed)) in_formats_blob {
    struct drm_format_modifier_blob_hdr hdr;
    uint32_t formats[2];        // XRGB8888, ARGB8888
    struct drm_format_modifier mods[1];
};
static struct in_formats_blob g_in_formats = {
    .hdr = {
        .version          = 1,
        .flags            = 0,
        .count_formats    = 2,
        .formats_offset   = sizeof(struct drm_format_modifier_blob_hdr),
        .count_modifiers  = 1,
        .modifiers_offset = sizeof(struct drm_format_modifier_blob_hdr)
                          + 2 * sizeof(uint32_t),
    },
    .formats = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 },
    .mods    = {
        { .formats = 0x3, .offset = 0, .pad = 0,
          .modifier = DRM_FORMAT_MOD_LINEAR },
    },
};

// ── MODE_ID blob: the current preferred mode of connector 0 ────────
// Populated lazily (scanout_mode query) on first getpropblob call.
typedef struct __attribute__((packed)) {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char     name[32];
} drm_modeinfo_blob_t;
static drm_modeinfo_blob_t g_mode_blob;
static uint8_t             g_mode_blob_valid = 0;

static void drm_ensure_mode_blob(void) {
    if (g_mode_blob_valid) return;
    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t w = 0, h = 0;
    if (b) b->scanout_mode(0, &w, &h);
    if (!w || !h) { w = 1280; h = 800; }
    __builtin_memset(&g_mode_blob, 0, sizeof(g_mode_blob));
    g_mode_blob.clock       = (uint32_t)((uint64_t)w * h * 60u / 1000u);
    g_mode_blob.hdisplay    = (uint16_t)w;
    g_mode_blob.hsync_start = (uint16_t)(w + 8);
    g_mode_blob.hsync_end   = (uint16_t)(w + 16);
    g_mode_blob.htotal      = (uint16_t)(w + 32);
    g_mode_blob.vdisplay    = (uint16_t)h;
    g_mode_blob.vsync_start = (uint16_t)(h + 2);
    g_mode_blob.vsync_end   = (uint16_t)(h + 4);
    g_mode_blob.vtotal      = (uint16_t)(h + 8);
    g_mode_blob.vrefresh    = 60;
    g_mode_blob.type        = 0x40;  // DRM_MODE_TYPE_PREFERRED
    const char* nm = "preferred";
    for (int i = 0; i < 31 && nm[i]; i++) g_mode_blob.name[i] = nm[i];
    g_mode_blob_valid = 1;
}

// One-time EDID checksum fix-up at first use.  Can't run at file scope
// because it mutates g_edid[127].
static void drm_ensure_edid(void) {
    static uint8_t done = 0;
    if (done) return;
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++) sum = (uint8_t)(sum + g_edid[i]);
    g_edid[127] = (uint8_t)(0x100u - sum);
    done = 1;
}

// ── Blob registry ─────────────────────────────────────────────────────
static int drm_resolve_blob(uint32_t blob_id, const void** out, uint32_t* len) {
    switch (blob_id) {
    case BLOB_EDID:
        drm_ensure_edid();
        *out = g_edid; *len = sizeof(g_edid);
        return 0;
    case BLOB_IN_FORMATS:
        *out = &g_in_formats; *len = sizeof(g_in_formats);
        return 0;
    case BLOB_MODE_ID:
        drm_ensure_mode_blob();
        *out = &g_mode_blob; *len = sizeof(g_mode_blob);
        return 0;
    default:
        return -ENOENT;
    }
}

static int drm_ioctl_mode_getpropblob(uint64_t arg) {
    struct __attribute__((packed)) {
        uint32_t blob_id;
        uint32_t length;
        uint64_t data;
    } a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    const void* src; uint32_t blob_len;
    int rc = drm_resolve_blob(a.blob_id, &src, &blob_len);
    if (rc) return rc;
    uint32_t to_copy = a.length < blob_len ? a.length : blob_len;
    if (to_copy && a.data) {
        if (copy_to_user((void*)a.data, src, to_copy) != 0) return -EFAULT;
    }
    a.length = blob_len;
    if (copy_to_user((void*)arg, &a, sizeof(a)) != 0) return -EFAULT;
    return 0;
}

// ── GETPLANERESOURCES / GETPLANE ─────────────────────────────────────
static int drm_ioctl_mode_getplaneres(uint64_t arg) {
    struct __attribute__((packed)) {
        uint64_t plane_id_ptr;
        uint32_t count_planes;
    } r;
    if (copy_from_user(&r, (void*)arg, sizeof(r)) != 0) return -EFAULT;

    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t n_sc = b ? b->scanout_count() : 0;
    uint32_t total = n_sc * PLANES_PER_CRTC;

    uint32_t ids[VIRTIO_GPU_MAX_SCANOUTS_CAP * PLANES_PER_CRTC];
    for (uint32_t i = 0; i < n_sc; i++) {
        ids[i * 2 + 0] = plane_of(i, 0);  // primary
        ids[i * 2 + 1] = plane_of(i, 1);  // cursor
    }

    int rc = drm_write_id_array(r.plane_id_ptr, &r.count_planes, ids, total);
    if (rc) return rc;
    if (copy_to_user((void*)arg, &r, sizeof(r)) != 0) return -EFAULT;
    return 0;
}

static int drm_ioctl_mode_getplane(uint64_t arg) {
    struct __attribute__((packed)) {
        uint32_t plane_id;
        uint32_t crtc_id;
        uint32_t fb_id;
        uint32_t possible_crtcs;
        uint32_t gamma_size;
        uint32_t count_format_types;
        uint64_t format_type_ptr;
    } p;
    if (copy_from_user(&p, (void*)arg, sizeof(p)) != 0) return -EFAULT;

    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t n_sc = b ? b->scanout_count() : 0;
    if (p.plane_id < PLANE_BASE) return -ENOENT;
    uint32_t raw = p.plane_id - PLANE_BASE;
    if (raw >= n_sc * PLANES_PER_CRTC) return -ENOENT;
    uint32_t crtc_idx = raw / PLANES_PER_CRTC;
    int is_cursor = (raw % PLANES_PER_CRTC) == 1;

    p.crtc_id        = CRTC_BASE + crtc_idx;
    p.fb_id          = 0;
    p.possible_crtcs = 1u << crtc_idx;
    p.gamma_size     = 0;

    // Primary plane: XRGB8888.  Cursor plane: ARGB8888.
    uint32_t fmts[1];
    fmts[0] = is_cursor ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
    int rc = drm_write_id_array(p.format_type_ptr, &p.count_format_types,
                                fmts, 1);
    if (rc) return rc;
    if (copy_to_user((void*)arg, &p, sizeof(p)) != 0) return -EFAULT;
    return 0;
}

// ── Property dispatch (metadata + per-object value) ──────────────────
struct drm_mode_property_enum_kern {
    uint64_t value;
    char     name[32];
} __attribute__((packed));

// Each prop has a name, a type-category, and (for enums) an enum table.
typedef enum {
    PTY_RANGE,   // inclusive [min, max] range
    PTY_SRANGE,  // signed range
    PTY_ENUM,    // enum with fixed values
    PTY_BLOB,    // blob reference
    PTY_OBJECT,  // object id range
} drm_prop_kind_t;

typedef struct {
    uint32_t                   id;
    const char*                name;
    drm_prop_kind_t            kind;
    uint8_t                    immutable;   // 1 → flag ORed
    uint64_t                   range_min;
    uint64_t                   range_max;
    uint32_t                   obj_type;    // for PTY_OBJECT (0=ANY)
    uint32_t                   n_enum;
    const struct drm_mode_property_enum_kern* enums;
} drm_prop_def_t;

// Enum tables.
static const struct drm_mode_property_enum_kern e_plane_type[] = {
    { DRM_PLANE_TYPE_OVERLAY, "Overlay" },
    { DRM_PLANE_TYPE_PRIMARY, "Primary" },
    { DRM_PLANE_TYPE_CURSOR,  "Cursor"  },
};
static const struct drm_mode_property_enum_kern e_dpms[] = {
    { DRM_MODE_DPMS_ON,      "On"      },
    { DRM_MODE_DPMS_STANDBY, "Standby" },
    { DRM_MODE_DPMS_SUSPEND, "Suspend" },
    { DRM_MODE_DPMS_OFF,     "Off"     },
};
static const struct drm_mode_property_enum_kern e_link[] = {
    { 0, "Good" }, { 1, "Bad" },
};
static const struct drm_mode_property_enum_kern e_panel[] = {
    { 0, "Normal"   }, { 1, "Upside Down" },
    { 2, "Left Side Up" }, { 3, "Right Side Up" },
};
static const struct drm_mode_property_enum_kern e_ctype[] = {
    { 0, "No Data" }, { 1, "Graphics" },
    { 2, "Photo"   }, { 3, "Cinema"   }, { 4, "Game" },
};

// Property table, shared across planes/crtcs/connectors.
static const drm_prop_def_t g_props[] = {
    // --- Plane ---
    { PROP_PLANE_TYPE,        "type",
      PTY_ENUM, 1, 0,0, 0, 3, e_plane_type },
    { PROP_PLANE_CRTC_ID,     "CRTC_ID",
      PTY_OBJECT, 0, 0,0, DRM_MODE_OBJECT_CRTC, 0, 0 },
    { PROP_PLANE_FB_ID,       "FB_ID",
      PTY_OBJECT, 0, 0,0, 0, 0, 0 },
    { PROP_PLANE_CRTC_X,      "CRTC_X",
      PTY_SRANGE, 0, 0,0x7FFFFFFFu, 0, 0, 0 },
    { PROP_PLANE_CRTC_Y,      "CRTC_Y",
      PTY_SRANGE, 0, 0,0x7FFFFFFFu, 0, 0, 0 },
    { PROP_PLANE_CRTC_W,      "CRTC_W",
      PTY_RANGE,  0, 0,0x7FFFFFFFu, 0, 0, 0 },
    { PROP_PLANE_CRTC_H,      "CRTC_H",
      PTY_RANGE,  0, 0,0x7FFFFFFFu, 0, 0, 0 },
    { PROP_PLANE_SRC_X,       "SRC_X",
      PTY_RANGE,  0, 0,0xFFFFFFFFu, 0, 0, 0 },
    { PROP_PLANE_SRC_Y,       "SRC_Y",
      PTY_RANGE,  0, 0,0xFFFFFFFFu, 0, 0, 0 },
    { PROP_PLANE_SRC_W,       "SRC_W",
      PTY_RANGE,  0, 0,0xFFFFFFFFu, 0, 0, 0 },
    { PROP_PLANE_SRC_H,       "SRC_H",
      PTY_RANGE,  0, 0,0xFFFFFFFFu, 0, 0, 0 },
    { PROP_PLANE_IN_FORMATS,  "IN_FORMATS",
      PTY_BLOB,   1, 0,0, 0, 0, 0 },
    // --- CRTC ---
    { PROP_CRTC_ACTIVE,       "ACTIVE",
      PTY_RANGE,  0, 0,1, 0, 0, 0 },
    { PROP_CRTC_MODE_ID,      "MODE_ID",
      PTY_BLOB,   0, 0,0, 0, 0, 0 },
    { PROP_CRTC_GAMMA_LUT_SIZE, "GAMMA_LUT_SIZE",
      PTY_RANGE,  1, 0,0xFFFFu, 0, 0, 0 },
    { PROP_CRTC_VRR_ENABLED,  "VRR_ENABLED",
      PTY_RANGE,  0, 0,1, 0, 0, 0 },
    // --- Connector ---
    { PROP_CONN_DPMS,                "DPMS",
      PTY_ENUM, 0, 0,0, 0, 4, e_dpms },
    { PROP_CONN_EDID,                "EDID",
      PTY_BLOB, 1, 0,0, 0, 0, 0 },
    { PROP_CONN_CRTC_ID,             "CRTC_ID",
      PTY_OBJECT, 0, 0,0, DRM_MODE_OBJECT_CRTC, 0, 0 },
    { PROP_CONN_NON_DESKTOP,         "non-desktop",
      PTY_RANGE, 1, 0,1, 0, 0, 0 },
    { PROP_CONN_LINK_STATUS,         "link-status",
      PTY_ENUM, 1, 0,0, 0, 2, e_link },
    { PROP_CONN_PANEL_ORIENTATION,   "panel orientation",
      PTY_ENUM, 1, 0,0, 0, 4, e_panel },
    { PROP_CONN_CONTENT_TYPE,        "content type",
      PTY_ENUM, 0, 0,0, 0, 5, e_ctype },
    { PROP_CONN_MAX_BPC,             "max bpc",
      PTY_RANGE, 0, 8,10, 0, 0, 0 },
};
static const uint32_t g_props_count = sizeof(g_props) / sizeof(g_props[0]);

static const drm_prop_def_t* drm_find_prop(uint32_t id) {
    for (uint32_t i = 0; i < g_props_count; i++)
        if (g_props[i].id == id) return &g_props[i];
    return 0;
}

// Fill the metadata reply for drmModeGetProperty.
static int drm_emit_prop_meta(uint64_t arg, const drm_prop_def_t* d) {
    struct __attribute__((packed)) {
        uint64_t values_ptr;
        uint64_t enum_blob_ptr;
        uint32_t prop_id;
        uint32_t flags;
        char     name[32];
        uint32_t count_values;
        uint32_t count_enum_blobs;
    } r;
    if (copy_from_user(&r, (void*)arg, sizeof(r)) != 0) return -EFAULT;

    r.prop_id = d->id;
    r.flags   = 0;
    if (d->immutable) r.flags |= DRM_MODE_PROP_IMMUTABLE;
    switch (d->kind) {
    case PTY_RANGE:  r.flags |= DRM_MODE_PROP_RANGE; break;
    case PTY_SRANGE: r.flags |= DRM_MODE_PROP_SIGNED_RANGE; break;
    case PTY_ENUM:   r.flags |= DRM_MODE_PROP_ENUM; break;
    case PTY_BLOB:   r.flags |= DRM_MODE_PROP_BLOB; break;
    case PTY_OBJECT: r.flags |= DRM_MODE_PROP_OBJECT; break;
    }

    for (int i = 0; i < 32; i++) r.name[i] = 0;
    for (int i = 0; i < 31 && d->name[i]; i++) r.name[i] = d->name[i];

    // count_values / values_ptr: two u64s for RANGE (min,max) or one u64 (obj type).
    uint64_t vals[2] = { 0 };
    uint32_t nv = 0;
    if (d->kind == PTY_RANGE || d->kind == PTY_SRANGE) {
        vals[0] = d->range_min; vals[1] = d->range_max; nv = 2;
    } else if (d->kind == PTY_OBJECT) {
        vals[0] = d->obj_type; nv = 1;
    }
    uint32_t to_copy_v = r.count_values < nv ? r.count_values : nv;
    if (to_copy_v && r.values_ptr) {
        if (copy_to_user((void*)r.values_ptr, vals,
                         to_copy_v * sizeof(uint64_t)) != 0) return -EFAULT;
    }
    r.count_values = nv;

    // count_enum_blobs / enum_blob_ptr: enum list (value+name pairs).
    uint32_t to_copy_e = r.count_enum_blobs < d->n_enum ? r.count_enum_blobs : d->n_enum;
    if (to_copy_e && r.enum_blob_ptr && d->enums) {
        if (copy_to_user((void*)r.enum_blob_ptr, d->enums,
                         to_copy_e * sizeof(struct drm_mode_property_enum_kern)) != 0)
            return -EFAULT;
    }
    r.count_enum_blobs = d->n_enum;

    if (copy_to_user((void*)arg, &r, sizeof(r)) != 0) return -EFAULT;
    return 0;
}

static int drm_ioctl_mode_getproperty(uint64_t arg) {
    struct __attribute__((packed)) {
        uint64_t values_ptr;
        uint64_t enum_blob_ptr;
        uint32_t prop_id;
        uint32_t flags;
        char     name[32];
        uint32_t count_values;
        uint32_t count_enum_blobs;
    } r;
    if (copy_from_user(&r, (void*)arg, sizeof(r)) != 0) return -EFAULT;
    const drm_prop_def_t* d = drm_find_prop(r.prop_id);
    if (!d) return -ENOENT;
    return drm_emit_prop_meta(arg, d);
}

// Per-CRTC runtime state kept for properties that actually mutate.
typedef struct {
    uint8_t  active;     // 0/1
    uint32_t mode_blob;  // BLOB_MODE_ID or caller-supplied (we only store)

    // ── Hardware-cursor state ────────────────────────────────────────
    // The cursor BO is a client dumb buffer, but the dumb's device
    // resource is X8 (no alpha); the cursor needs ARGB so the device
    // alpha-blends the pointer shape.  We mint a SEPARATE B8G8R8A8
    // resource aliasing the same backing pages, owned by this crtc.
    //
    // CACHE KEY = backing PHYS, not the dumb handle.  wlroots' legacy
    // cursor path re-GETFBs a FRESH dumb handle for the SAME cursor
    // pages on every commit (then closes it); keying on the handle made
    // the cache miss every frame → resource_destroy+recreate each frame
    // → a RESOURCE_UNREF on the latched cursor every frame = the flicker
    // (and unbounded res-id growth).  The pages are stable, so we key on
    // phys: same phys → cache hit → no churn → cursor persists when
    // static and is stable in motion.  We PIN the aliased pages (own
    // ref, like an fb) so the per-commit handle close / eventual BO free
    // can't return them to the buddy allocator while the device scans
    // them out.
    uint32_t cursor_res;       // device resource id (0 = none)
    phys_addr_t cursor_phys;   // backing phys the res aliases (cache key)
    uint32_t cursor_pin_pages; // pages we pmm_ref_inc'd (drop on re-mint)
    uint32_t cursor_w, cursor_h;
    uint32_t hot_x, hot_y;
    int32_t  cur_x, cur_y;   // last position (UPDATE_CURSOR needs it)
    uint8_t  cursor_latched; // 1 = device shows cursor_res (UPDATE sent OK)
} drm_crtc_state_t;
static drm_crtc_state_t g_crtc_state[VIRTIO_GPU_MAX_SCANOUTS_CAP];

// ── OBJ_GETPROPERTIES — emit all props for the given object ──────────
// Kept in one place so atomic callers, legacy object queries, and
// wlroots' name-based bsearch all see the same list + values.
static int drm_obj_collect(uint32_t obj_type, uint32_t obj_id,
                           uint32_t* ids, uint64_t* vals,
                           uint32_t max, uint32_t* out_count) {
    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t n_sc = b ? b->scanout_count() : 0;
    uint32_t n = 0;

    uint32_t t = obj_type;
    if (t == DRM_MODE_OBJECT_ANY) {
        if (obj_id >= PLANE_BASE && obj_id < PLANE_BASE + n_sc * PLANES_PER_CRTC)
            t = DRM_MODE_OBJECT_PLANE;
        else if (obj_id >= CRTC_BASE && obj_id < CRTC_BASE + n_sc)
            t = DRM_MODE_OBJECT_CRTC;
        else if (obj_id >= CONN_BASE && obj_id < CONN_BASE + n_sc)
            t = DRM_MODE_OBJECT_CONNECTOR;
        else if (obj_id >= ENC_BASE && obj_id < ENC_BASE + n_sc)
            t = DRM_MODE_OBJECT_ENCODER;
        else
            return -ENOENT;
    }

#define EMIT(pid, v) do {                                  \
    if (max > n && ids && vals) { ids[n] = (pid); vals[n] = (uint64_t)(v); } \
    n++;                                                   \
} while (0)

    switch (t) {
    case DRM_MODE_OBJECT_PLANE: {
        if (obj_id < PLANE_BASE || obj_id >= PLANE_BASE + n_sc * PLANES_PER_CRTC)
            return -ENOENT;
        uint32_t raw = obj_id - PLANE_BASE;
        uint32_t crtc_idx = raw / PLANES_PER_CRTC;
        int is_cursor = (raw % PLANES_PER_CRTC) == 1;
        EMIT(PROP_PLANE_TYPE, is_cursor ? DRM_PLANE_TYPE_CURSOR : DRM_PLANE_TYPE_PRIMARY);
        EMIT(PROP_PLANE_CRTC_ID, CRTC_BASE + crtc_idx);
        EMIT(PROP_PLANE_FB_ID,   0);
        EMIT(PROP_PLANE_CRTC_X,  0);
        EMIT(PROP_PLANE_CRTC_Y,  0);
        EMIT(PROP_PLANE_CRTC_W,  0);
        EMIT(PROP_PLANE_CRTC_H,  0);
        EMIT(PROP_PLANE_SRC_X,   0);
        EMIT(PROP_PLANE_SRC_Y,   0);
        EMIT(PROP_PLANE_SRC_W,   0);
        EMIT(PROP_PLANE_SRC_H,   0);
        EMIT(PROP_PLANE_IN_FORMATS, BLOB_IN_FORMATS);
        break;
    }
    case DRM_MODE_OBJECT_CRTC: {
        if (obj_id < CRTC_BASE || obj_id >= CRTC_BASE + n_sc) return -ENOENT;
        uint32_t idx = obj_id - CRTC_BASE;
        EMIT(PROP_CRTC_ACTIVE,         g_crtc_state[idx].active);
        EMIT(PROP_CRTC_MODE_ID,        g_crtc_state[idx].mode_blob
                                        ? g_crtc_state[idx].mode_blob
                                        : BLOB_MODE_ID);
        EMIT(PROP_CRTC_GAMMA_LUT_SIZE, 0);
        EMIT(PROP_CRTC_VRR_ENABLED,    0);
        break;
    }
    case DRM_MODE_OBJECT_CONNECTOR: {
        if (obj_id < CONN_BASE || obj_id >= CONN_BASE + n_sc) return -ENOENT;
        uint32_t idx = obj_id - CONN_BASE;
        EMIT(PROP_CONN_DPMS,               DRM_MODE_DPMS_ON);
        EMIT(PROP_CONN_EDID,               BLOB_EDID);
        EMIT(PROP_CONN_CRTC_ID,            CRTC_BASE + idx);
        EMIT(PROP_CONN_NON_DESKTOP,        0);
        EMIT(PROP_CONN_LINK_STATUS,        0);
        EMIT(PROP_CONN_PANEL_ORIENTATION,  0);
        EMIT(PROP_CONN_CONTENT_TYPE,       0);
        EMIT(PROP_CONN_MAX_BPC,            8);
        break;
    }
    case DRM_MODE_OBJECT_ENCODER:
        if (obj_id < ENC_BASE || obj_id >= ENC_BASE + n_sc) return -ENOENT;
        break;
    default:
        return -EINVAL;
    }
    *out_count = n;
    return 0;
#undef EMIT
}

static int drm_ioctl_mode_obj_getprops(uint64_t arg) {
    struct __attribute__((packed)) {
        uint64_t props_ptr;
        uint64_t prop_values_ptr;
        uint32_t count_props;
        uint32_t obj_id;
        uint32_t obj_type;
    } a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;

    uint32_t ids[24];
    uint64_t vals[24];
    uint32_t n = 0;
    int rc = drm_obj_collect(a.obj_type, a.obj_id, ids, vals,
                             (uint32_t)(sizeof(ids) / sizeof(ids[0])), &n);
    if (rc) return rc;

    uint32_t to_copy = a.count_props < n ? a.count_props : n;
    if (to_copy && a.props_ptr) {
        if (copy_to_user((void*)a.props_ptr, ids,
                         to_copy * sizeof(uint32_t)) != 0) return -EFAULT;
    }
    if (to_copy && a.prop_values_ptr) {
        if (copy_to_user((void*)a.prop_values_ptr, vals,
                         to_copy * sizeof(uint64_t)) != 0) return -EFAULT;
    }
    a.count_props = n;
    if (copy_to_user((void*)arg, &a, sizeof(a)) != 0) return -EFAULT;
    return 0;
}

// ── SETPROPERTY (legacy connector form) + OBJ_SETPROPERTY ─────────────
// Mutable props accepted; immutable props rejected with EINVAL so
// misuse surfaces.
static int drm_prop_is_immutable(uint32_t prop_id) {
    const drm_prop_def_t* d = drm_find_prop(prop_id);
    return d ? d->immutable : 0;
}

static int drm_ioctl_mode_setproperty(uint64_t arg) {
    struct __attribute__((packed)) {
        uint64_t value;
        uint32_t prop_id;
        uint32_t connector_id;
    } a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    if (!drm_find_prop(a.prop_id)) return -ENOENT;
    if (drm_prop_is_immutable(a.prop_id)) return -EINVAL;
    // DPMS is the only mutating connector prop wlroots sets — accept all
    // values; our virtio-gpu is single-state but callers expect success.
    return 0;
}

static int drm_ioctl_mode_obj_setproperty(uint64_t arg) {
    struct __attribute__((packed)) {
        uint64_t value;
        uint32_t prop_id;
        uint32_t obj_id;
        uint32_t obj_type;
    } a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    if (!drm_find_prop(a.prop_id)) return -ENOENT;
    if (drm_prop_is_immutable(a.prop_id)) return -EINVAL;

    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t n_sc = b ? b->scanout_count() : 0;
    if (a.obj_type == DRM_MODE_OBJECT_CRTC &&
        a.obj_id >= CRTC_BASE && a.obj_id < CRTC_BASE + n_sc) {
        uint32_t idx = a.obj_id - CRTC_BASE;
        if (a.prop_id == PROP_CRTC_ACTIVE)  g_crtc_state[idx].active    = (uint8_t)!!a.value;
        if (a.prop_id == PROP_CRTC_MODE_ID) g_crtc_state[idx].mode_blob = (uint32_t)a.value;
    }
    return 0;
}

// ── SETPLANE — wlroots uses it for atomic path; legacy still works. ──
// Minimum useful behaviour: on primary plane with a valid fb, route
// through SETCRTC-style scanout update.  On cursor plane, defer to
// drmModeSetCursor semantics (no-op for now — virtio-gpu cursor is
// done via software blit in the compositor's own frame).
static int drm_ioctl_mode_setplane(uint64_t arg) {
    struct __attribute__((packed)) {
        uint32_t plane_id;
        uint32_t crtc_id;
        uint32_t fb_id;
        uint32_t flags;
        int32_t  crtc_x, crtc_y;
        uint32_t crtc_w, crtc_h;
        uint32_t src_x, src_y;
        uint32_t src_w, src_h;
    } a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t n_sc = b ? b->scanout_count() : 0;
    if (a.plane_id < PLANE_BASE) return -ENOENT;
    uint32_t raw = a.plane_id - PLANE_BASE;
    if (raw >= n_sc * PLANES_PER_CRTC) return -ENOENT;
    // Accept; real scanout update happens via SETCRTC / PAGE_FLIP.
    return 0;
}

// ── GETFB — legacy drmModeGetFB.  Returns fb metadata + GEM handle.
// wlroots' legacy cursor path depends on this: it GETFBs the cursor
// fb, drmModeSetCursor()s the returned handle, then GEM_CLOSEs it.
// The fb's creation handle is typically ALREADY CLOSED by the time
// GETFB runs (wlroots drops every bo handle right after ADDFB2), so
// the old lookup-by-stale-handle returned -ENOENT — and wlroots'
// legacy.c crtc_commit treats any GETFB failure as a failed COMMIT,
// dropping the whole frame.  Net effect: with a visible cursor, every
// frame was dropped and the screen stayed on the initial black
// modeset buffer.  Linux semantics: mint a FRESH GEM handle that
// references the fb's buffer; the caller closes it when done.
static int drm_ioctl_mode_getfb(vfs_file_t* self, uint64_t arg) {
    struct __attribute__((packed)) {
        uint32_t fb_id;
        uint32_t width, height, pitch, bpp, depth, handle;
    } a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    drm_client_t* c = client_of(self);
    drm_fb_t* fb = find_fb(c, a.fb_id);
    if (!fb) return -ENOENT;

    // Cheap path: the creation handle still exists and still points at
    // the same backing — hand it back rather than minting a duplicate.
    drm_dumb_t* d = find_dumb(c, fb->handle);
    uint32_t out_handle;
    if (d && d->phys == fb->phys) {
        out_handle = d->handle;
    } else {
        // Mint a new handle around the fb's backing pages.  No
        // independent virtio-gpu resource (vgpu_res_id=0): the handle
        // exists for SetCursor/mmap-style consumers, not scan-out —
        // the fb keeps its own resource.  Page refs taken here are
        // dropped by the GEM_CLOSE the caller owes us (dumb_free).
        if (drm_charge(g_current, fb->bytes_alloc) != 0) return -ENOMEM;
        drm_dumb_t* nd = (drm_dumb_t*)kmalloc(sizeof(*nd));
        if (!nd) { drm_uncharge(g_current, fb->bytes_alloc); return -ENOMEM; }
        nd->handle      = c->next_dumb_handle++;
        nd->width       = fb->width;
        nd->height      = fb->height;
        nd->pitch       = fb->pitch;
        nd->size        = (uint64_t)fb->pitch * fb->height;
        nd->vgpu_res_id = 0;
        nd->phys        = fb->phys;
        nd->bytes_alloc = fb->bytes_alloc;
        nd->order       = fb->order;
        for (uint32_t i = 0; i < nd->bytes_alloc / 4096u; i++)
            pmm_ref_inc(nd->phys + (phys_addr_t)i * 4096u);
        nd->next = c->dumbs;
        c->dumbs = nd;
        out_handle = nd->handle;
    }

    a.width  = fb->width;
    a.height = fb->height;
    a.pitch  = fb->pitch;
    a.bpp    = 32;
    a.depth  = 24;
    a.handle = out_handle;
    if (copy_to_user((void*)arg, &a, sizeof(a)) != 0) return -EFAULT;
    return 0;
}

// ── CLOSEFB — Linux 6.0+ alternative to RMFB.  Same semantics for us.
static int drm_ioctl_mode_closefb(vfs_file_t* self, uint64_t arg) {
    struct __attribute__((packed)) { uint32_t fb_id; uint32_t pad; } a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    drm_client_t* c = client_of(self);
    drm_fb_t** pp = &c->fbs;
    while (*pp) {
        if ((*pp)->fb_id == a.fb_id) {
            drm_fb_t* x = *pp;
            *pp = x->next;
            drm_fb_free(x);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -ENOENT;
}

// ── Legacy cursor ioctl — accepted (virtio-gpu compositor-side cursor). ──
#define DRM_MODE_CURSOR_BO   0x01u
#define DRM_MODE_CURSOR_MOVE 0x02u

// Legacy CURSOR (28B) and CURSOR2 (36B, adds hotspot).  Wired to the
// backend hardware cursor (virtio-gpu cursor queue).  Backends without
// cursor ops get -ENOTSUP so wlroots falls back to software cursors —
// returning fake success here leaves the user with an invisible
// pointer (sway trusts the ioctl and skips the software path).
// Drop the current ARGB cursor alias: destroy the device resource and
// release the page pin taken when it was minted.  Idempotent.
static void cursor_drop_alias(const drm_backend_ops_t* b, drm_crtc_state_t* cs) {
    if (!cs->cursor_res) return;
    if (b) b->resource_destroy(cs->cursor_res);
    for (uint32_t i = 0; i < cs->cursor_pin_pages; i++)
        pmm_ref_dec(cs->cursor_phys + (phys_addr_t)i * 4096u);
    cs->cursor_res       = 0;
    cs->cursor_phys      = 0;
    cs->cursor_pin_pages = 0;
    cs->cursor_latched   = 0;
}

static int drm_ioctl_mode_cursor(vfs_file_t* f, uint64_t arg, int has_hotspot) {
    struct __attribute__((packed)) {
        uint32_t flags;
        uint32_t crtc_id;
        uint32_t x, y;             // MOVE: position
        uint32_t width, height;    // BO: cursor dimensions
        uint32_t handle;           // BO: dumb handle; 0 = hide
        int32_t  hot_x, hot_y;     // CURSOR2 only
    } a = {0};
    uint64_t copy_len = has_hotspot ? sizeof(a) : sizeof(a) - 8;
    if (copy_from_user(&a, (void*)arg, copy_len) != 0) return -EFAULT;

    const drm_backend_ops_t* b = __atomic_load_n(&drm_backend, __ATOMIC_ACQUIRE);
    uint32_t n_sc = b ? b->scanout_count() : 0;
    if (a.crtc_id < CRTC_BASE || a.crtc_id >= CRTC_BASE + n_sc) return -ENOENT;
    if (!b->cursor_update || !b->cursor_move) return -EOPNOTSUPP;

    uint32_t idx = a.crtc_id - CRTC_BASE;
    drm_crtc_state_t* cs = &g_crtc_state[idx];

    if (a.flags & DRM_MODE_CURSOR_MOVE) {
        cs->cur_x = (int32_t)a.x;
        cs->cur_y = (int32_t)a.y;
        return b->cursor_move(idx, cs->cur_x, cs->cur_y);
    }

    if (!(a.flags & DRM_MODE_CURSOR_BO)) return -EINVAL;

    if (a.handle == 0) {
        // Hide: unbind device cursor, drop our aliased resource + pin.
        b->cursor_update(idx, 0, 0, 0, cs->cur_x, cs->cur_y);
        cursor_drop_alias(b, cs);
        return 0;
    }

    drm_client_t* c = client_of(f);
    drm_dumb_t* d = find_dumb(c, a.handle);
    if (!d) return -ENOENT;
    // Overflow-safe: width*height*4 must fit the backing.  The old bare
    // (uint64_t)w*h*4 > d->size guard wrapped u64 (w=h=2^31 -> *4 == 2^64 -> 0)
    // and accepted absurd dimensions.  cur_bytes is reused for the pin count.
    uint64_t cur_bytes;
    if (!drm_cursor_bytes(a.width, a.height, d->size, &cur_bytes)) return -EINVAL;

    // (Re)mint the ARGB alias ONLY when the backing PHYS or the cursor
    // dimensions actually change — never merely because wlroots handed us
    // a fresh GETFB handle for the same pages (that per-commit churn was
    // the flicker).  Keyed on phys → cache hits every frame in motion and
    // when static, so the latched cursor is never destroyed mid-display.
    if (cs->cursor_res &&
        (cs->cursor_phys != d->phys ||
         cs->cursor_w != a.width || cs->cursor_h != a.height))
        cursor_drop_alias(b, cs);

    uint8_t fresh = 0;
    if (!cs->cursor_res) {
        fresh = 1;
        uint32_t id = alloc_res_id();
        if (b->resource_create(id, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                               a.width, a.height) != 0)
            return -EIO;
        if (b->resource_attach_backing(id, d->phys, d->bytes_alloc) != 0) {
            b->resource_destroy(id);
            return -EIO;
        }
        // Pin the pages the device scans out for this cursor (own ref,
        // like an fb) so the per-commit close of the transient GETFB
        // handle — and the eventual cursor-BO teardown — can't return
        // them to the buddy allocator while the alias is still latched.
        uint32_t pages = (uint32_t)((cur_bytes + 4095u) / 4096u);
        for (uint32_t i = 0; i < pages; i++)
            pmm_ref_inc(d->phys + (phys_addr_t)i * 4096u);
        cs->cursor_res       = id;
        cs->cursor_phys      = d->phys;
        cs->cursor_pin_pages = pages;
        cs->cursor_w         = a.width;
        cs->cursor_h         = a.height;
    }
    uint8_t hot_changed = 0;
    if (has_hotspot && ((uint32_t)a.hot_x != cs->hot_x ||
                        (uint32_t)a.hot_y != cs->hot_y)) {
        cs->hot_x = (uint32_t)a.hot_x;
        cs->hot_y = (uint32_t)a.hot_y;
        hot_changed = 1;
    }

    // DEDUPE: wlroots' legacy cursor path re-issues SetCursor with the
    // SAME buffer on every output commit (~per frame during motion).
    // Re-sending UPDATE_CURSOR makes the host (QEMU SDL) redefine its
    // cursor sprite each time → visible flicker while moving.  The image
    // only actually changes when the compositor hands us a different
    // buffer (different phys → re-mint above), a new size, or a new
    // hotspot — wlroots never mutates a displayed cursor buffer in place
    // (it swaps swapchain slots).  Identical re-binds are no-ops: skip
    // the transfer + update entirely and leave the latched cursor alone.
    if (!fresh && !hot_changed && cs->cursor_latched)
        return 0;

    // Push the (possibly re-rendered) cursor pixels, then bind.
    if (b->resource_transfer(cs->cursor_res, a.width, a.height) != 0)
        return -EIO;
    int rc = b->cursor_update(idx, cs->cursor_res, cs->hot_x, cs->hot_y,
                              cs->cur_x, cs->cur_y);
    if (rc == 0) cs->cursor_latched = 1;
    return rc;
}

// ── AUTH_MAGIC / GET_MAGIC (master-token semantics) ─────────────────
// Linux semantics drive libdrm's drmIsMaster() heuristic: it calls
// drmAuthMagic(fd, 0) and infers "master" iff that returns NOT -EACCES.
// Masters (opener of the DRM fd) see -EINVAL for magic=0; non-masters
// see -EACCES.  MakaOS is single-client, so every open is master —
// report -EINVAL for magic=0 and success for any non-zero token so
// that wlroots' allocator takes the DMABUF path, then gracefully
// skips the magic-auth step once drmGetNodeTypeFromFd returns -1 for
// our non-Linux major/minor pairs.
static int drm_ioctl_auth_magic(uint64_t arg) {
    struct { uint32_t magic; } __attribute__((packed)) a;
    if (copy_from_user(&a, (void*)arg, sizeof(a)) != 0) return -EFAULT;
    if (a.magic == 0) return -EINVAL;
    return 0;
}

// GET_MAGIC: invent a per-fd token.  wlroots only round-trips the
// value through drmAuthMagic, which accepts any non-zero; any unique
// non-zero constant works.  A real multi-client DRM would derive a
// per-open nonce from an atomic counter — a one-liner we'll add the
// moment we have a second DRM consumer.
static int drm_ioctl_get_magic(uint64_t arg) {
    struct { uint32_t magic; } __attribute__((packed)) a = { .magic = 0xC0DEFACEu };
    if (copy_to_user((void*)arg, &a, sizeof(a)) != 0) return -EFAULT;
    return 0;
}

// ── Event queue helpers (single-writer/single-reader per fd) ───────
// Writer: commit path (drm_queue_flip_event).  Reader: userspace read()
// via drm_read_op.  Serialised by: commits run in the calling task's
// syscall context, reads in the same fd owner's syscall context.  No
// SMP writer concurrency possible on one fd.  The counters are
// atomic-relaxed so a reader waking via wait_queue sees the tail.

static uint32_t drm_evq_used(const drm_client_t* c) {
    return __atomic_load_n(&c->evq_tail, __ATOMIC_ACQUIRE)
         - __atomic_load_n(&c->evq_head, __ATOMIC_ACQUIRE);
}
static uint32_t drm_evq_free(const drm_client_t* c) {
    return DRM_EVQ_SIZE - drm_evq_used(c);
}

static void drm_evq_push(drm_client_t* c, const void* src, uint32_t n) {
    const uint8_t* s = (const uint8_t*)src;
    uint32_t t = __atomic_load_n(&c->evq_tail, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < n; i++) c->evq[(t + i) & DRM_EVQ_MASK] = s[i];
    __atomic_store_n(&c->evq_tail, t + n, __ATOMIC_RELEASE);
}

static uint32_t drm_evq_pop(drm_client_t* c, uint8_t* dst, uint32_t max) {
    uint32_t have = drm_evq_used(c);
    if (have > max) have = max;
    uint32_t h = __atomic_load_n(&c->evq_head, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < have; i++) dst[i] = c->evq[(h + i) & DRM_EVQ_MASK];
    __atomic_store_n(&c->evq_head, h + have, __ATOMIC_RELEASE);
    return have;
}

// Called by commit paths (PAGE_FLIP, ATOMIC) when the client requested
// DRM_MODE_PAGE_FLIP_EVENT.  Synthesises a drm_event_vblank with the
// type = DRM_EVENT_FLIP_COMPLETE so libdrm's drmHandleEvent dispatches
// to the compositor's page_flip_handler2.  Our pageflip is synchronous
// (the virtio-gpu set_scanout returns after the resource is staged),
// so we queue "flip done" immediately after commit.
static void drm_queue_flip_event(vfs_file_t* f, uint32_t crtc_id, uint64_t user_data) {
    drm_client_t* c = client_of(f);
    if (drm_evq_free(c) < sizeof(drm_event_vblank_t)) return;  // drop silently

    extern uint64_t tsc_read_ns(void);
    uint64_t ns  = tsc_read_ns();
    uint32_t sec = (uint32_t)(ns / 1000000000ULL);
    uint32_t us  = (uint32_t)((ns % 1000000000ULL) / 1000ULL);

    drm_event_vblank_t ev = {
        .base = { .type = DRM_EVENT_FLIP_COMPLETE,
                  .length = sizeof(drm_event_vblank_t) },
        .user_data = user_data,
        .tv_sec    = sec,
        .tv_usec   = us,
        .sequence  = 0,
        .crtc_id   = crtc_id,
    };
    drm_evq_push(c, &ev, sizeof(ev));
    drm_dbg("flipev QUEUED crtc=%u", crtc_id);   /* AUTOFIX: repaint-loop trace */
    if (f->waitq) wait_queue_wake_all(f->waitq);
}

// Returns queued event bytes into the user's buffer.  Non-blocking:
// empty queue returns 0 (libdrm treats as "no events to process").
// Partial reads are OK — libdrm reads in a loop.
static int64_t drm_read_op(vfs_file_t* self, void* buf, uint64_t len) {
    drm_client_t* c = client_of(self);
    uint32_t want = (len > DRM_EVQ_SIZE) ? DRM_EVQ_SIZE : (uint32_t)len;
    uint8_t  kbuf[DRM_EVQ_SIZE];
    uint32_t got = drm_evq_pop(c, kbuf, want);
    if (got == 0) return 0;
    drm_dbg("flipev READ %u bytes", got);   /* AUTOFIX: repaint-loop trace */
    if (copy_to_user(buf, kbuf, got) != 0) return -EFAULT;
    return (int64_t)got;
}

static int drm_poll_op(vfs_file_t* self, int events) {
    drm_client_t* c = client_of(self);
    if (events & 0x1 /*POLLIN*/)  return drm_evq_used(c) > 0;
    if (events & 0x4 /*POLLOUT*/) return 0;  // not writable
    if (events & 0x10 /*POLLHUP*/) return 0;
    return 0;
}

/* Map an ioctl request number to a short human-readable tag.  Used by
 * the dispatcher tracing so serial.txt reads as a commit pipeline
 * rather than a wall of hex. */
static const char* drm_ioctl_name(uint64_t req) {
    switch (req) {
    case DRM_IOCTL_VERSION:                return "VERSION";
    case DRM_IOCTL_GET_CAP:                return "GET_CAP";
    case DRM_IOCTL_MODE_GETRESOURCES:      return "GETRESOURCES";
    case DRM_IOCTL_MODE_GETCONNECTOR:      return "GETCONNECTOR";
    case DRM_IOCTL_MODE_GETENCODER:        return "GETENCODER";
    case DRM_IOCTL_MODE_GETCRTC:           return "GETCRTC";
    case DRM_IOCTL_MODE_SETCRTC:           return "SETCRTC";
    case DRM_IOCTL_MODE_PAGE_FLIP:         return "PAGE_FLIP";
    case DRM_IOCTL_MODE_ATOMIC:            return "ATOMIC";
    case DRM_IOCTL_MODE_ADDFB2:            return "ADDFB2";
    case DRM_IOCTL_MODE_RMFB:              return "RMFB";
    case DRM_IOCTL_MODE_CREATE_DUMB:       return "CREATE_DUMB";
    case DRM_IOCTL_MODE_MAP_DUMB:          return "MAP_DUMB";
    case DRM_IOCTL_MODE_DESTROY_DUMB:      return "DESTROY_DUMB";
    case DRM_IOCTL_SET_CLIENT_CAP:         return "SET_CLIENT_CAP";
    case DRM_IOCTL_GEM_CLOSE:              return "GEM_CLOSE";
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:     return "PRIME_HANDLE_TO_FD";
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:     return "PRIME_FD_TO_HANDLE";
    case DRM_IOCTL_MODE_GETPLANERESOURCES: return "GETPLANERESOURCES";
    case DRM_IOCTL_MODE_GETPLANE:          return "GETPLANE";
    case DRM_IOCTL_MODE_SETPLANE:          return "SETPLANE";
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES: return "OBJ_GETPROPERTIES";
    case DRM_IOCTL_MODE_GETPROPERTY:       return "GETPROPERTY";
    case DRM_IOCTL_MODE_SETPROPERTY:       return "SETPROPERTY";
    case DRM_IOCTL_MODE_OBJ_SETPROPERTY:   return "OBJ_SETPROPERTY";
    case DRM_IOCTL_MODE_GETPROPBLOB:       return "GETPROPBLOB";
    case DRM_IOCTL_MODE_GETFB:             return "GETFB";
    case DRM_IOCTL_MODE_CLOSEFB:           return "CLOSEFB";
    case DRM_IOCTL_MODE_CURSOR:            return "CURSOR";
    case DRM_IOCTL_MODE_CURSOR2:           return "CURSOR2";
    case DRM_IOCTL_AUTH_MAGIC:             return "AUTH_MAGIC";
    case DRM_IOCTL_GET_MAGIC:              return "GET_MAGIC";
    case DRM_IOCTL_MODE_CREATE_LEASE:      return "CREATE_LEASE";
    case DRM_IOCTL_MODE_LIST_LESSEES:      return "LIST_LESSEES";
    case DRM_IOCTL_MODE_GET_LEASE:         return "GET_LEASE";
    case DRM_IOCTL_MODE_REVOKE_LEASE:      return "REVOKE_LEASE";
    case DRM_IOCTL_SET_MASTER:             return "SET_MASTER";
    case DRM_IOCTL_DROP_MASTER:            return "DROP_MASTER";
    }
    return "?";
}

// ── ioctl dispatch ──────────────────────────────────────────────────
static int64_t drm_ioctl_impl(vfs_file_t* self, uint64_t req, uint64_t arg) {
    switch (req) {
    case DRM_IOCTL_VERSION:           return drm_ioctl_version(arg);
    case DRM_IOCTL_GET_CAP:           return drm_ioctl_get_cap(arg);
    case DRM_IOCTL_MODE_GETRESOURCES: return drm_ioctl_mode_getresources(arg);
    case DRM_IOCTL_MODE_GETCONNECTOR: return drm_ioctl_mode_getconnector(arg);
    case DRM_IOCTL_MODE_GETENCODER:   return drm_ioctl_mode_getencoder(arg);
    case DRM_IOCTL_MODE_GETCRTC:      return drm_ioctl_mode_getcrtc(arg);
    case DRM_IOCTL_MODE_SETCRTC:      return drm_ioctl_setcrtc(self, arg);
    case DRM_IOCTL_MODE_PAGE_FLIP:    return drm_ioctl_page_flip(self, arg);
    case DRM_IOCTL_MODE_ATOMIC:       return drm_ioctl_atomic(self, arg);
    case DRM_IOCTL_MODE_ADDFB2:       return drm_ioctl_addfb2(self, arg);
    case DRM_IOCTL_MODE_RMFB:         return drm_ioctl_rmfb(self, arg);
    case DRM_IOCTL_MODE_CREATE_DUMB:  return drm_ioctl_create_dumb(self, arg);
    case DRM_IOCTL_MODE_MAP_DUMB:     return drm_ioctl_map_dumb(self, arg);
    case DRM_IOCTL_MODE_DESTROY_DUMB: return drm_ioctl_destroy_dumb(self, arg);
    case DRM_IOCTL_SET_CLIENT_CAP:    return drm_ioctl_set_client_cap(arg);
    case DRM_IOCTL_GEM_CLOSE:         return drm_ioctl_gem_close(self, arg);
    case DRM_IOCTL_PRIME_HANDLE_TO_FD:return drm_ioctl_prime_handle_to_fd(self, arg);
    case DRM_IOCTL_PRIME_FD_TO_HANDLE:return drm_ioctl_prime_fd_to_handle(self, arg);
    case DRM_IOCTL_MODE_GETPLANERESOURCES: return drm_ioctl_mode_getplaneres(arg);
    case DRM_IOCTL_MODE_GETPLANE:          return drm_ioctl_mode_getplane(arg);
    case DRM_IOCTL_MODE_SETPLANE:          return drm_ioctl_mode_setplane(arg);
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES: return drm_ioctl_mode_obj_getprops(arg);
    case DRM_IOCTL_MODE_GETPROPERTY:       return drm_ioctl_mode_getproperty(arg);
    case DRM_IOCTL_MODE_SETPROPERTY:       return drm_ioctl_mode_setproperty(arg);
    case DRM_IOCTL_MODE_OBJ_SETPROPERTY:   return drm_ioctl_mode_obj_setproperty(arg);
    case DRM_IOCTL_MODE_GETPROPBLOB:       return drm_ioctl_mode_getpropblob(arg);
    case DRM_IOCTL_MODE_GETFB:             return drm_ioctl_mode_getfb(self, arg);
    case DRM_IOCTL_MODE_CLOSEFB:           return drm_ioctl_mode_closefb(self, arg);
    case DRM_IOCTL_MODE_CURSOR:            return drm_ioctl_mode_cursor(self, arg, 0);
    case DRM_IOCTL_MODE_CURSOR2:           return drm_ioctl_mode_cursor(self, arg, 1);
    case DRM_IOCTL_AUTH_MAGIC:             return drm_ioctl_auth_magic(arg);
    case DRM_IOCTL_GET_MAGIC:              return drm_ioctl_get_magic(arg);
    case DRM_IOCTL_MODE_CREATE_LEASE:      return -EOPNOTSUPP;
    case DRM_IOCTL_MODE_LIST_LESSEES:      return -EOPNOTSUPP;
    case DRM_IOCTL_MODE_GET_LEASE:         return -EOPNOTSUPP;
    case DRM_IOCTL_MODE_REVOKE_LEASE:      return -EOPNOTSUPP;
    case DRM_IOCTL_SET_MASTER:        return 0;  // single-client for now
    case DRM_IOCTL_DROP_MASTER:       return 0;
    default:
        pr_warn("drm", "unknown ioctl req=0x%08x arg=0x%lx → ENOTTY",
                (uint32_t)req, arg);
        return -ENOTTY;
    }
}

/* Tracing wrapper.  Every ioctl enters here — the switch inside
 * drm_ioctl_impl fans out by request.  Both the call and the result
 * are logged + traced; the dwl render pipeline is fully visible in
 * build/serial.txt from this one hook. */
static int64_t drm_ioctl(vfs_file_t* self, uint64_t req, uint64_t arg) {
    drm_dbg("ioctl %s req=0x%08x arg=0x%lx",
            drm_ioctl_name(req), (uint32_t)req, arg);
    TRACE(TRACE_DRM_IOCTL, (uint32_t)req, arg, 0, 0);
    int64_t rc = drm_ioctl_impl(self, req, arg);
    if (rc != 0) {
        /* WARN, not DEBUG — any nonzero return may be the first
         * point where the pipeline diverges from the happy path. */
        pr_warn("drm", "ioctl %s req=0x%08x → %ld",
                drm_ioctl_name(req), (uint32_t)req, rc);
    } else {
        drm_dbg("ioctl %s → 0", drm_ioctl_name(req));
    }
    TRACE(TRACE_DRM_IOCTL, (uint32_t)req, arg, (uint64_t)rc, 1);
    return rc;
}

// Track open DRM clients so the scanout can be restored to the
// text-console banner when the last one closes.  Atomic because
// open/close can run from different CPUs; no lock needed — any
// open/close pair sees a stable snapshot, and the restore only
// fires when the counter hits 0.
static uint32_t s_drm_open_count;

static void drm_close(vfs_file_t* self) {
    // Tear down every dumb buffer + fb this client still holds.
    drm_client_t* c = client_of(self);
    if (c) {
        drm_fb_t* fb = c->fbs;
        while (fb) { drm_fb_t* n = fb->next; drm_fb_free(fb); fb = n; }
        drm_dumb_t* d = c->dumbs;
        while (d)  { drm_dumb_t* n = d->next; dumb_free(d, g_current); d = n; }
        kfree(c);
        self->ctx = NULL;
    }
    kfree(self);

    // Last open DRM fd gone — hand the display back to the text
    // console.  Otherwise the hardware scanout keeps showing
    // whatever the compositor last flipped (usually its own freed
    // framebuffer), so `bash` output after the compositor exits is
    // invisible even though the TTY keeps receiving keypresses.
    uint32_t now = __atomic_sub_fetch(&s_drm_open_count, 1, __ATOMIC_ACQ_REL);
    if (now == 0) {
        extern int virtio_gpu_restore_default_scanout(void);
        virtio_gpu_restore_default_scanout();
    }
}

vfs_file_t* vfs_drm_open(void) {
    drm_client_t* c = (drm_client_t*)kmalloc(sizeof(*c));
    if (!c) return NULL;
    __builtin_memset(c, 0, sizeof(*c));
    c->next_dumb_handle = 1;
    c->next_fb_id       = 1;

    vfs_file_t* f = vfs_alloc_file();   // zeroed, waitq wired (poll-ready), refcount=1
    if (!f) { kfree(c); return NULL; }
    f->ioctl    = drm_ioctl;
    f->read     = drm_read_op;
    f->poll     = drm_poll_op;
    f->close    = drm_close;
    f->ctx      = c;
    f->rights   = 0xFFFFFFFFu;   // non-default: DRM fd carries all rights
    // Linux DRM major = 226, minor = 0 for card0.  Matches libudev's
    // advertised devnum so wlroots' fstat-then-udev cross-check
    // resolves.
    f->rdev     = (226u << 8) | 0u;
    // Count the open ONLY once it has fully succeeded -- bumping before the
    // vfs_file_t alloc (as before) over-counted on a kmalloc failure, so the
    // last-close restore-default-scanout heuristic could never reach 0.
    __atomic_add_fetch(&s_drm_open_count, 1, __ATOMIC_ACQ_REL);
    return f;
}

// ── io_uring DRM commit op (#4) ──────────────────────────────────────
// Clients submit a drm_mode_atomic-shaped request via an io_uring
// SQE with opcode IORING_OP_DRM_COMMIT.  sqe->fd is the DRM fd,
// sqe->addr is the user pointer to drm_mode_atomic_t.  This function
// reuses the ioctl path below the parse layer — atomic-first design
// means there's exactly one commit primitive no matter how it was
// submitted.  Returns 0 or -errno; caller (io_uring dispatcher)
// writes the value into the CQE's res field.
int drm_ring_atomic(vfs_file_t* drm_fd_file, uint64_t atomic_ptr) {
    if (!drm_fd_file || !drm_is_drm_file(drm_fd_file)) return -EBADF;
    if (!atomic_ptr) return -EFAULT;
    // Re-enter the ioctl parser; same struct shape, same commit path.
    return drm_ioctl_atomic(drm_fd_file, atomic_ptr);
}

// ── OOM query (#6) ───────────────────────────────────────────────────
uint64_t drm_get_charged_bytes(struct task_t* t) {
    if (!t) return 0;
    return __atomic_load_n(&t->drm_bytes_charged, __ATOMIC_ACQUIRE);
}

// ── drm_dumb_size selftest ────────────────────────────────────────────────
// Deterministic check of the overflow-safe dumb-buffer size math.  The 0x4000-
// 0000 / 0x40000001 rows are exactly the 32-bit `width * 4` overflow the old
// code hit (pitch wrapped to 0 / 4 for a billion-pixel-wide resource).
void drm_dumb_size_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;

    uint32_t pitch; uint64_t size;
    struct { uint32_t w, h, bpp; int eret; uint32_t epitch; uint64_t esize; } c[] = {
        { 1920, 1080, 32,  0, 7680, 8294400ULL },     // normal: pitch 1920*4, size *1080
        { 1,    1,    32,  0, 4,    4ULL },            // minimal
        { DRM_MAX_FB_DIM, DRM_MAX_FB_DIM, 32, 0, 65536u, 1073741824ULL }, // max ok: 16384*4=65536, *16384=2^30
        { 0x40000000u, 1, 32, -EINVAL, 0, 0 },         // 32-bit width*4 overflow -> reject
        { 0x40000001u, 1, 32, -EINVAL, 0, 0 },         // pitch would wrap to 4 -> reject
        { 0,    1080, 32, -EINVAL, 0, 0 },             // zero width
        { 1920, 0,    32, -EINVAL, 0, 0 },             // zero height
        { 1920, 1080, 24, -EINVAL, 0, 0 },             // unsupported bpp
        { DRM_MAX_FB_DIM + 1u, 1, 32, -EINVAL, 0, 0 }, // one past the dim cap
    };
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        pitch = 0xDEADu; size = 0xDEADULL;
        int r = drm_dumb_size(c[i].w, c[i].h, c[i].bpp, &pitch, &size);
        int ok = (r == c[i].eret) &&
                 (r != 0 || (pitch == c[i].epitch && size == c[i].esize));
        if (!ok) {
            kprintf("[drm_dumb] FAIL i=%u r=%d pitch=%lu size=%lu\n",
                    i, r, (unsigned long)pitch, (unsigned long)size);
            fails++;
        }
    }
    kprintf(fails ? "[drm_dumb] SELF-TEST FAILED\n"
                  : "[drm_dumb] SELF-TEST PASSED (overflow-safe dumb size)\n");
}

// ── drm_cursor_bytes selftest ─────────────────────────────────────────────
// Deterministic check of the overflow-safe SET_CURSOR backing-size guard.  The
// 0x80000000 / 0xFFFFFFFF rows are exactly the u64 `w*h*4` wrap the old guard
// hit: (uint64_t)0x80000000*0x80000000*4 == 2^64 -> 0, so `0 > d->size` was
// false and absurd cursor dimensions slipped through to resource_transfer.
void drm_cursor_bytes_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    uint64_t b;
    struct { uint32_t w, h; uint64_t backing; bool eok; uint64_t ebytes; } c[] = {
        { 64,  64,  16384,      true,  16384ULL },   // exact 64x64 ARGB fits
        { 32,  32,  16384,      true,  4096ULL },    // fits with room
        { 64,  65,  16384,      false, 0 },          // 16640 > 16384 -> reject
        { 1,   1,   4,          true,  4ULL },        // minimal
        { 1,   1,   3,          false, 0 },          // 4 > 3 backing -> reject
        { 0,   64,  16384,      false, 0 },          // zero width
        { 64,  0,   16384,      false, 0 },          // zero height
        { 0x80000000u, 0x80000000u, 4096, false, 0 },// THE bug: *4 == 2^64 wraps to 0
        { 0x80000000u, 0x80000000u, 0xFFFFFFFFFFFFFFFFULL, false, 0 }, // even a max backing: *4 overflows u64 -> reject
        { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFFFFFFFFFULL, false, 0 }, // w*h*4 overflows u64
        { 0x40000000u, 0x4u, 0xFFFFFFFFFFFFFFFFULL, true, 0x400000000ULL }, // 2^30*4 px *4 = 2^34 bytes, no wrap, fits huge backing
    };
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        b = 0xDEADULL;
        bool ok = drm_cursor_bytes(c[i].w, c[i].h, c[i].backing, &b);
        int good = (ok == c[i].eok) && (!ok || b == c[i].ebytes);
        if (!good) {
            kprintf("[drm_cursor] FAIL i=%u ok=%d bytes=%lu\n",
                    i, (int)ok, (unsigned long)b);
            fails++;
        }
    }
    kprintf(fails ? "[drm_cursor] SELF-TEST FAILED\n"
                  : "[drm_cursor] SELF-TEST PASSED (overflow-safe cursor backing-size)\n");
}

// ── drm_atomic per-object prop-count bound selftest ───────────────────────
// Deterministic check that an atomic commit's per-object property count is
// bounded, so a hostile counts[i] cannot drive the prop loop up to 2^32
// iterations (DoS) or overflow props_off / values_off.
void drm_atomic_count_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    struct { uint32_t count; int want; } c[] = {
        { 0,                          1 },   // zero props -> ok
        { 1,                          1 },
        { DRM_MAX_PROPS_PER_OBJ,      1 },   // exactly at the cap -> ok
        { DRM_MAX_PROPS_PER_OBJ + 1u, 0 },   // one past the cap -> reject
        { 0xFFFFFFFFu,                0 },   // the DoS value -> reject
    };
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        int got = drm_atomic_count_ok(c[i].count);
        if (got != c[i].want) {
            kprintf("[drm_atomic] FAIL count=%u got=%d want=%d\n",
                    c[i].count, got, c[i].want);
            fails++;
        }
    }
    kprintf(fails ? "[drm_atomic] SELF-TEST FAILED\n"
                  : "[drm_atomic] SELF-TEST PASSED (per-object prop count bounded)\n");
}
