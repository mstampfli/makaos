#pragma once
// ── Linux-compatible DRM/KMS uAPI on /dev/dri/card0 (Tier 2.5b) ─────
//
// Translates the DRM ioctl surface libdrm + wlroots use onto the
// virtio-gpu 2D primitives in drivers/video/virtio_gpu.c.  Only the
// subset that wlroots-pixman actually calls is implemented.
//
// Opened by /dev/dri/card0.  vfs_drm_open returns a vfs_file_t with
// read = NULL (DRM clients use ioctl + mmap), write = NULL, ioctl
// dispatching to drm_ioctl, close releasing the file's state.

#include "common.h"
#include "vfs.h"

vfs_file_t* vfs_drm_open(void);

// Predicate used by sys_mmap to recognise a DRM fd before calling
// drm_resolve_dumb_mmap.  Compares the close op under the hood.
int drm_is_drm_file(vfs_file_t* f);

// Resolve a MAP_DUMB "magic offset" → backing physical address + size.
// Callers: sys_mmap.  Returns 0 on success, -errno on failure.
int64_t drm_resolve_dumb_mmap(vfs_file_t* f, uint64_t offset,
                                uint64_t len, phys_addr_t* out_phys,
                                uint64_t* out_bytes);
