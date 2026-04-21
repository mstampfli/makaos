#pragma once
// ── virtio-gpu kernel driver ────────────────────────────────────────
//
// Implements the virtio 1.1 GPU device (device ID 0x1050).  Two
// virtqueues: control (commands) and cursor.  We drive the 2D path
// only — scanout via host-resident resources + TRANSFER_TO_HOST_2D +
// RESOURCE_FLUSH.  3D (virgl) deferred to a later phase.
//
// Layered on top of this, kernel/drivers/video/drm.c (Tier 2.5b)
// exposes the Linux DRM/KMS ioctl uAPI on /dev/dri/card0.

#include "common.h"

#define VIRTIO_GPU_MAX_SCANOUTS_CAP 16  // matches VIRTIO_GPU_MAX_SCANOUTS in .c

// Initialise the virtio-gpu device.  Returns 1 if a device was found
// and initialised, 0 otherwise (system then falls back to the GOP
// framebuffer for scanout).  Calls GET_DISPLAY_INFO once at init and
// caches the enumerated scanouts.
int virtio_gpu_init(void);

// Number of scanouts the device reported.  0 if no device / not init.
uint32_t virtio_gpu_num_scanouts(void);

// Preferred mode (w×h) of scanout `idx` — zero-fills out_w/out_h on
// any error.  Width 0 means "this scanout is disabled".
void virtio_gpu_get_mode(uint32_t idx, uint32_t* out_w, uint32_t* out_h);

// ── Full pipeline exerciser ──────────────────────────────────────────
// Creates a 2D resource sized to scanout 0, attaches host-backing
// pages, SET_SCANOUT's it, paints a verification pattern, then does
// TRANSFER_TO_HOST_2D + RESOURCE_FLUSH so the pattern actually shows
// up on the QEMU window.  Returns 1 on success.  Runs from
// init_kthread so it exercises the happy path at every boot.
int virtio_gpu_present_test(void);

// ── Resource lifecycle API (used by the DRM/KMS uAPI layer) ─────────
// All five calls return 1 on success, 0 on failure.  Caller is
// responsible for assigning a nonzero resource_id (conventionally
// monotonically increasing); 0 is reserved by the spec.

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  2
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM  1

int virtio_gpu_resource_create_2d(uint32_t res_id, uint32_t format,
                                   uint32_t w, uint32_t h);
int virtio_gpu_resource_unref(uint32_t res_id);
int virtio_gpu_resource_attach_backing_single(uint32_t res_id,
                                               phys_addr_t phys, uint32_t len);
int virtio_gpu_set_scanout(uint32_t scanout_id, uint32_t res_id,
                            uint32_t w, uint32_t h);
int virtio_gpu_transfer_to_host_2d(uint32_t res_id, uint32_t w, uint32_t h);
int virtio_gpu_resource_flush(uint32_t res_id, uint32_t w, uint32_t h);

// Called at subsys init after virtio_gpu_init succeeds.  Registers
// this device as the DRM core's active backend via drm_backend_register.
void virtio_gpu_register_backend(void);

// Point scanout 0 back at the startup banner resource (allocated by
// virtio_gpu_present_test / vgpu_setup_scanout_buffer).  Used by the
// DRM core when a client exits and we need the text-console
// framebuffer visible again.  No-op if the banner resource never got
// created (e.g. boot-time virtio-gpu initialisation did not finish).
// Returns 1 on success, 0 on failure.
int virtio_gpu_restore_default_scanout(void);

// ── fbcon-as-DRM-client wiring ───────────────────────────────────────
// The text console (kernel/drivers/video/fb.c) cannot rely on QEMU's
// virtio-vga VGA-compat BAR — SET_SCANOUT with resource_id=0 leaves
// the display *off*, not mirroring the legacy FB.  Instead the text
// console writes into a kernel-owned virtio-gpu resource, and we
// explicitly TRANSFER_TO_HOST_2D + RESOURCE_FLUSH on each scroll /
// newline.  These two calls own that wiring:
//
//   virtio_gpu_fbcon_init(out_phys, out_virt, out_w, out_h, out_pitch)
//       Allocates a resource sized to scanout 0's preferred mode,
//       attaches a physically-contiguous backing, SET_SCANOUT's it,
//       and hands back both the phys address (for fb_init) and the
//       HHDM virt pointer (for text-console writes).  Returns 1.
//
//   virtio_gpu_fbcon_flush(void)
//       Transfers the full backing to the host resource + flushes.
//       Called from fb.c after any visible write.  Safe to call
//       concurrently with DRM client ioctls — serialised by the same
//       control-queue mutex as every other command.
int virtio_gpu_fbcon_init(phys_addr_t* out_phys,
                           uint8_t**   out_virt,
                           uint32_t*   out_w,
                           uint32_t*   out_h,
                           uint32_t*   out_pitch);
void virtio_gpu_fbcon_flush(void);
