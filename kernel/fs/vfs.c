#include "vfs.h"
#include "keyboard.h"
#include "evdev.h"
#include "mouse.h"
#include "hda.h"
#include "common.h"
#include "fb.h"
#include "kheap.h"
#include "rcu.h"

// ── [LEGACY] Framebuffer console device (/dev/vga) ───────────────────────
// LEGACY: Direct write to framebuffer terminal, bypasses TTY entirely.
//   No read support, no line discipline, no OPOST/ONLCR translation.
//   NEW PATH: open /dev/tty0 (tty_open(0)) — writes go through tty_vfs_write
//   which applies OPOST/ONLCR (\n → \r\n) and calls write_char → fb_term_putc.
//   /dev/vga is kept for process.c fd 1/2 fallback and any kernel code that
//   writes directly to the console before tty_init() runs.

/* LEGACY cursor globals — kept for compatibility with shell.c which reads them.
   Synced with g_fb_row / g_fb_col after every vga_write.
   NEW PATH: shell should query cursor position via TTY ioctl (TIOCGWINSZ). */
uint32_t g_vga_row = 0;
uint32_t g_vga_col = 0;

// ── Current working directory ─────────────────────────────────────────────
char g_cwd[256] = "/";

// ── Helper: allocate and initialise a minimal vfs_file_t ─────────────────
// Sets waitq → &_waitq, secondary_waitq = NULL, refcount = 1, rights = 0.
// Caller fills in the function pointers and ctx.
static vfs_file_t* vfs_alloc_file(void) {
    vfs_file_t* f = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!f) return NULL;
    f->read            = NULL;
    f->write           = NULL;
    f->close           = NULL;
    f->seek            = NULL;
    f->poll            = NULL;
    f->ioctl           = NULL;
    f->ctx             = NULL;
    f->waitq           = &f->_waitq;
    wait_queue_init(f->waitq);
    f->secondary_waitq = NULL;
    f->flags           = 0;
    f->refcount        = 1;
    f->rights          = 0;
    f->path[0]         = '\0';
    return f;
}

// ── /dev/vga ─────────────────────────────────────────────────────────────

static int64_t vga_write(vfs_file_t* self, const void* buf, uint64_t len) {
    (void)self;
    const char* s = (const char*)buf;
    for (uint64_t i = 0; i < len; i++)
        fb_term_putc(s[i]);
    g_vga_row = g_fb_row;
    g_vga_col = g_fb_col;
    return (int64_t)len;
}

static void vga_close(vfs_file_t* self) { kfree(self); }

vfs_file_t* vfs_vga_open(void) {
    vfs_file_t* f = vfs_alloc_file();
    if (!f) return NULL;
    f->write = vga_write;
    f->close = vga_close;
    return f;
}

// ── [LEGACY] /dev/kbd raw keyboard device ────────────────────────────────
// LEGACY API path — use tty_open(0) or evdev_open() for new code.

static int64_t kbd_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    if (len == 0) return 0;
    char* dst = (char*)buf;
    uint64_t i = 0;
    dst[i++] = keyboard_wait();
    while (i < len) {
        char c = keyboard_getchar();
        if (!c) break;
        dst[i++] = c;
    }
    return (int64_t)i;
}

static void generic_close(vfs_file_t* self) { kfree(self); }

vfs_file_t* vfs_kbd_open(void) {
    vfs_file_t* f = vfs_alloc_file();
    if (!f) return NULL;
    f->read  = kbd_read;
    f->close = generic_close;
    return f;
}

// ── Raw scancode device (/dev/kbdraw) ────────────────────────────────────

static int64_t kbdraw_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    if (!len) return 0;
    int n = evdev_getraw((uint8_t*)buf, (int)len);
    return (int64_t)n;
}

vfs_file_t* vfs_kbdraw_open(void) {
    vfs_file_t* f = vfs_alloc_file();
    if (!f) return NULL;
    f->read  = kbdraw_read;
    f->close = generic_close;
    return f;
}

// ── Mouse device (/dev/mouse) ─────────────────────────────────────────────
// Returns packed mouse_event_t structs.
// poll() returns 1 when events are pending; epoll wakes via g_mouse_waitq
// which mouse_irq_handler fires after each decoded packet.

static int64_t mouse_dev_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    if (!len) return 0;
    int max = (int)(len / sizeof(mouse_event_t));
    if (max == 0) return 0;
    int n = mouse_read((mouse_event_t*)buf, max);
    return (int64_t)(n * (int)sizeof(mouse_event_t));
}

static int mouse_dev_poll(vfs_file_t* self, int events) {
    (void)self; (void)events;
    return mouse_has_events() ? 1 : 0;
}

vfs_file_t* vfs_mouse_open(void) {
    vfs_file_t* f = vfs_alloc_file();
    if (!f) return NULL;
    f->read            = mouse_dev_read;
    f->poll            = mouse_dev_poll;
    f->close           = generic_close;
    // Register on g_mouse_waitq so epoll/poll wakeups reach this fd.
    f->secondary_waitq = &g_mouse_waitq;
    return f;
}

// ── DSP device (/dev/dsp) ─────────────────────────────────────────────────

static int64_t dsp_write(vfs_file_t* self, const void* buf, uint64_t len) {
    (void)self;
    return (int64_t)hda_write(buf, (uint32_t)len);
}

vfs_file_t* vfs_dsp_open(void) {
    vfs_file_t* f = vfs_alloc_file();
    if (!f) return NULL;
    f->write = dsp_write;
    f->close = generic_close;
    return f;
}

// ── /dev/null ─────────────────────────────────────────────────────────────

static int64_t null_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self; (void)buf; (void)len;
    return 0;
}

static int64_t null_write(vfs_file_t* self, const void* buf, uint64_t len) {
    (void)self; (void)buf;
    return (int64_t)len;
}

vfs_file_t* vfs_null_open(void) {
    vfs_file_t* f = vfs_alloc_file();
    if (!f) return NULL;
    f->read  = null_read;
    f->write = null_write;
    f->close = generic_close;
    return f;
}

// ── /dev/zero ─────────────────────────────────────────────────────────────

static int64_t zero_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    __builtin_memset(buf, 0, len);
    return (int64_t)len;
}

vfs_file_t* vfs_zero_open(void) {
    vfs_file_t* f = vfs_alloc_file();
    if (!f) return NULL;
    f->read  = zero_read;
    f->write = null_write;
    f->close = generic_close;
    return f;
}

// ── /dev/urandom — routed through the kernel CSPRNG (Phase 9A) ─────
//
// Previously a TSC-seeded xorshift64 — NOT cryptographic.  Now reads
// from the multi-source entropy pool + ChaCha20 DRBG in
// kernel/crypto/random.c.  Mix sources include RDRAND (not trusted
// alone), TSC jitter on every IRQ, boot-DRAM noise, and periodic
// per-CPU fast-mix flushes.
#include "random.h"

static int64_t urandom_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    kcsprng_read(buf, len);
    return (int64_t)len;
}

vfs_file_t* vfs_urandom_open(void) {
    vfs_file_t* f = vfs_alloc_file();
    if (!f) return NULL;
    f->read  = urandom_read;
    f->write = null_write;
    f->close = generic_close;
    return f;
}
