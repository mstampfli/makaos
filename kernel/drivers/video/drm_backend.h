#pragma once
// ── DRM backend vtable ──────────────────────────────────────────────
//
// Every GPU driver we can potentially support registers exactly one
// drm_backend_ops_t struct.  The DRM core (drm.c) calls only through
// this vtable — no driver-specific ifdefs, no inheritance, no dual
// GEM/TTM lineages.
//
// Linux's drm_driver has ~50 callbacks accumulated over 20 years.
// Ours has 10.  If we need a new capability, we add one slot — we
// don't keep a deprecated variant "for compatibility."
//
// Backends we ship today: virtio_gpu (kernel/drivers/video/virtio_gpu.c).

#include "common.h"

typedef struct drm_backend_ops {
    // ── Enumeration ──────────────────────────────────────────────
    // scanout_count: number of physically-present displays.  0 if
    // the device isn't present / init failed.
    uint32_t (*scanout_count)(void);

    // scanout_mode: preferred mode (w×h) of scanout `idx`.  Width 0
    // means "disabled or invalid index".
    void     (*scanout_mode)(uint32_t idx, uint32_t* w, uint32_t* h);

    // ── Resource lifecycle (GPU-side buffer, not kernel memory) ──
    // Format uses the driver's native pixel format enum — currently
    // B8G8R8X8 everywhere.  id 0 is reserved; backends allocate
    // their own nonzero ids.
    int (*resource_create)(uint32_t id, uint32_t format,
                             uint32_t w, uint32_t h);

    int (*resource_destroy)(uint32_t id);

    // Attach one contiguous physical range as backing pages.  The
    // DRM core always uses single-entry SG because our dumb buffers
    // come from pmm_buddy_alloc.  More complex SG lists would add
    // a nr_entries parameter here.
    int (*resource_attach_backing)(uint32_t id,
                                     phys_addr_t phys, uint32_t bytes);

    // ── Scanout ──────────────────────────────────────────────────
    // Atomic set: bind a resource to a scanout.  resource_id=0
    // disables the scanout.  Width/height are the visible region.
    int (*scanout_set)(uint32_t scanout, uint32_t resource_id,
                        uint32_t w, uint32_t h);

    // Push CPU-written bytes to the device.  Call this whenever the
    // backing memory has been modified and the display needs refresh.
    int (*resource_transfer)(uint32_t id, uint32_t w, uint32_t h);

    // Commit transferred bytes to the display.  After resource_flush
    // returns, the new pixels are visible.
    int (*resource_flush)(uint32_t id, uint32_t w, uint32_t h);

    // ── Hardware cursor (OPTIONAL — may be NULL) ─────────────────
    // When NULL the DRM core returns -ENOTSUP from the cursor ioctls,
    // which makes wlroots/sway fall back to software cursor rendering.
    // NEVER fake success without drawing: compositors trust a
    // successful cursor ioctl and skip the software fallback, leaving
    // the user with an invisible cursor (observed with sway).
    //
    // cursor_update: bind cursor image `resource_id` (ARGB format,
    // hotspot hot_x/hot_y) to `scanout` at position x,y.  id 0 hides.
    // cursor_move: reposition without changing the image.
    int (*cursor_update)(uint32_t scanout, uint32_t resource_id,
                          uint32_t hot_x, uint32_t hot_y,
                          int32_t x, int32_t y);
    int (*cursor_move)(uint32_t scanout, int32_t x, int32_t y);
} drm_backend_ops_t;

// The single backend registration.  Set by the backend's init code
// (virtio_gpu_init calls drm_backend_register(&virtio_gpu_backend)).
// drm.c reads this through an RCU-style consume; at most one backend
// is active per boot.
extern const drm_backend_ops_t* drm_backend;

void drm_backend_register(const drm_backend_ops_t* ops);
