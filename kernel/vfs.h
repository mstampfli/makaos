#pragma once
#include "common.h"

// ── VFS — Virtual File System interface ──────────────────────────────────
//
// Every "file" (device, pipe, real file) is represented by a vfs_file_t.
// It holds a set of function pointers (the operations) and a driver-specific
// context pointer.  An fd table entry is just a pointer to one of these.
//
// Rules for kernel code:
//   - vfs_read/vfs_write may BLOCK (they call sched_sleep internally if needed).
//   - vfs_read/vfs_write are called from process context ONLY, never from IRQ.
//   - vfs_close frees whatever the driver allocated for its context.
//   - NULL function pointers mean "not supported" → returns -1.

// lseek whence values (POSIX)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct vfs_file_t {
    // Read up to `len` bytes into `buf`.  Returns bytes read, 0 on EOF, -1 on error.
    int64_t (*read )(struct vfs_file_t* self, void*       buf, uint64_t len);
    // Write `len` bytes from `buf`.  Returns bytes written, -1 on error.
    int64_t (*write)(struct vfs_file_t* self, const void* buf, uint64_t len);
    // Close and free any driver state.  Must not be NULL.
    void    (*close)(struct vfs_file_t* self);
    // Seek to offset.  Returns new position, or -1 if not seekable.
    // May be NULL for non-seekable files (devices, pipes).
    int64_t (*seek )(struct vfs_file_t* self, int64_t offset, int whence);

    void*    ctx;    // driver-specific state (may be NULL for stateless drivers)
    uint32_t flags;  // VFS_FLAG_* (reserved for future use)
} vfs_file_t;

// ── Convenience wrappers ──────────────────────────────────────────────────
static inline int64_t vfs_read(vfs_file_t* f, void* buf, uint64_t len) {
    if (!f || !f->read) return -1;
    return f->read(f, buf, len);
}

static inline int64_t vfs_write(vfs_file_t* f, const void* buf, uint64_t len) {
    if (!f || !f->write) return -1;
    return f->write(f, buf, len);
}

static inline void vfs_close(vfs_file_t* f) {
    if (f && f->close) f->close(f);
}

// ── Shared VGA cursor state (defined in vfs.c) ────────────────────────────
extern uint32_t g_vga_row;
extern uint32_t g_vga_col;

// ── Current working directory (absolute path, "/" by default) ─────────────
extern char g_cwd[256];

// ── Built-in device constructors ─────────────────────────────────────────

// Returns a vfs_file_t backed by the VGA console (write-only).
// The returned pointer is a pointer to a static object — do NOT call
// vfs_close() on it (VGA is never freed).
vfs_file_t* vfs_vga_open(void);

// Returns a vfs_file_t backed by the PS/2 keyboard (read-only, blocking).
// Also static — do NOT call vfs_close() on it.
vfs_file_t* vfs_kbd_open(void);
