#pragma once
#include "common.h"
#include "rights.h"

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

    // Ioctl: device-specific control.  May be NULL (means not supported).
    int64_t (*ioctl)(struct vfs_file_t* self, uint64_t request, uint64_t arg);

    void*    ctx;      // driver-specific state (may be NULL for stateless drivers)
    void*    poll_waiter; // task_t* — task sleeping in poll/select on this fd
    uint32_t flags;    // open flags (O_APPEND, O_NONBLOCK etc.)
    uint32_t refcount; // reference count; 0 = static object (never freed)
    uint32_t rights;   // RIGHT_* bitmask; checked before every operation
    char     path[256]; // absolute path for ext2 files (empty for devices/pipes)
} vfs_file_t;

// ── Convenience wrappers ──────────────────────────────────────────────────
// NOTE: vfs_read/vfs_write do NOT enforce fd rights — they are called from
// kernel-internal paths (ksec reader thread, pipe internals, device drivers)
// that should not be subject to per-fd rights checks.
// Rights enforcement is done in sys_read/sys_write syscall handlers only,
// where the caller is a userland process operating on an fd from its fd table.
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

// LEGACY: /dev/vga — direct framebuffer write, bypasses TTY.
//   Write-only, no line discipline, no OPOST/ONLCR translation.
//   Static object — do NOT call vfs_close() on it.
//   NEW PATH: use tty_open(0) for /dev/tty0 — output goes through
//   tty_vfs_write (OPOST/ONLCR) → write_char → fb_term_putc.
vfs_file_t* vfs_vga_open(void);

// LEGACY: /dev/kbd — direct keyboard polling, no line discipline.
//   keyboard_wait()/keyboard_getchar() are now dead stubs.
//   NEW PATH: use tty_open(0) for /dev/tty0 (N_TTY line discipline),
//             evdev_open() for /dev/input/event0 (structured events),
//             or vfs_kbdraw_open() for raw PS/2 scancodes.
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
