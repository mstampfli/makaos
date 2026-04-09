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
    // Poll: check readiness without blocking.  events is POLLIN or POLLOUT.
    // Returns 1 if ready, 0 if not.  May be NULL (means always ready).
    int     (*poll )(struct vfs_file_t* self, int events);

    void*    ctx;      // driver-specific state (may be NULL for stateless drivers)
    uint32_t flags;    // open flags (O_APPEND, O_NONBLOCK etc.)
    uint32_t refcount; // reference count; 0 = static object (never freed)
    char     path[256]; // absolute path for ext2 files (empty for devices/pipes)
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

// dup: increment refcount so two fds share one file description.
// Returns f (same pointer), or NULL if f is NULL.
static inline vfs_file_t* vfs_dup(vfs_file_t* f) {
    if (f && f->refcount > 0) f->refcount++;
    return f;
}

// close: decrement refcount; only call driver close when it reaches 0.
// Static objects (refcount==0) are never freed.
static inline void vfs_close(vfs_file_t* f) {
    if (!f) return;
    if (f->refcount == 0) return;           // static object
    if (--f->refcount > 0) return;          // still referenced
    if (f->close) f->close(f);             // last reference: free it
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

// Returns a vfs_file_t that delivers raw PS/2 scancodes (non-blocking).
// Includes E0 prefixes and break codes (make | 0x80).
vfs_file_t* vfs_kbdraw_open(void);

// Returns a vfs_file_t that delivers mouse_event_t structs (non-blocking).
// Read in multiples of sizeof(mouse_event_t) = 5 bytes.
vfs_file_t* vfs_mouse_open(void);

// Returns a write-only vfs_file_t for the AC97 PCM output stream (/dev/dsp).
// Write signed 16-bit stereo samples at AC97_SAMPLE_RATE Hz.
vfs_file_t* vfs_dsp_open(void);

// /dev/null  — reads return 0 (EOF), writes are silently consumed.
vfs_file_t* vfs_null_open(void);

// /dev/zero  — reads return zero bytes, writes are silently consumed.
vfs_file_t* vfs_zero_open(void);

// /dev/urandom — reads return pseudo-random bytes (TSC-seeded xorshift64).
vfs_file_t* vfs_urandom_open(void);
