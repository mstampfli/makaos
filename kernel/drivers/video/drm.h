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

// io_uring op entry point.  Called from kernel/io/io_uring.c when a
// SQE with IORING_OP_DRM_COMMIT is processed.  `drm_fd_file` is the
// vfs_file_t pointed to by the SQE's fd field; `atomic_ptr` is the
// user pointer to a drm_mode_atomic_t.  Returns 0 or -errno.
// Reuses the same parsing + drm_commit_apply() path as the legacy
// DRM_IOCTL_MODE_ATOMIC ioctl — #3 atomic-first design means io_uring
// and ioctl paths share the same commit primitive.
int drm_ring_atomic(vfs_file_t* drm_fd_file, uint64_t atomic_ptr);

// OOM kill query.  Returns the task's current DRM memory charge.
// The future OOM killer enumerates tasks and kills the largest
// drm_bytes_charged holder as a tiebreaker when address-space
// pressure is GPU-bound.
struct task_t;
uint64_t drm_get_charged_bytes(struct task_t* t);
