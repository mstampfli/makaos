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
