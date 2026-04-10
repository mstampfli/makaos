#include "vfs.h"
#include "keyboard.h"
#include "evdev.h"
#include "mouse.h"
#include "hda.h"
#include "common.h"
#include "fb.h"

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

static int64_t vga_write(vfs_file_t* self, const void* buf, uint64_t len) {
    (void)self;
    const char* s = (const char*)buf;
    for (uint64_t i = 0; i < len; i++)
        fb_term_putc(s[i]);
    g_vga_row = g_fb_row;
    g_vga_col = g_fb_col;
    return (int64_t)len;
}

static void vga_noop_close(vfs_file_t* self) { (void)self; }

static vfs_file_t s_vga_file = {
    .read  = NULL,         // write-only device
    .write = vga_write,
    .close = vga_noop_close,
    .ctx   = NULL,
    .flags = 0,
};

vfs_file_t* vfs_vga_open(void) {
    return &s_vga_file;
}

// ── [LEGACY] /dev/kbd raw keyboard device ────────────────────────────────
// LEGACY: Direct keyboard polling via keyboard_wait()/keyboard_getchar().
//   keyboard_wait() is now a stub that sleeps forever (returns nothing).
//   keyboard_getchar() is now a stub that always returns 0.
//
// NEW PATH: Open /dev/tty or /dev/tty0 instead.
//   Input flows: PS/2 IRQ → keyboard thread → input_emit() → tty_on_kbd_event()
//   → N_TTY line discipline → tty_vfs_read() → userland read(fd, ...).
//   For raw evdev events use /dev/input/event0 (input_event_t structs).
//   For raw PS/2 scancodes use /dev/kbdraw (evdev_getraw()).
//
// This device and vfs_kbd_open() are kept only so that process.c's
// fallback fd_table init compiles.  Nothing should open /dev/kbd in
// new code.  Remove once process.c is updated to always use tty_open(0).

static int64_t kbd_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    if (len == 0) return 0;
    char* dst = (char*)buf;
    uint64_t i = 0;
    // LEGACY: keyboard_wait() is now a dead stub — blocks forever.
    dst[i++] = keyboard_wait();
    while (i < len) {
        char c = keyboard_getchar(); // LEGACY: always returns 0
        if (!c) break;
        dst[i++] = c;
    }
    return (int64_t)i;
}

static vfs_file_t s_kbd_file = {
    .read  = kbd_read,
    .write = NULL,
    .close = vga_noop_close,
    .ctx   = NULL,
    .flags = 0,
};

// LEGACY — see block comment above.  Use tty_open(0) for new code.
vfs_file_t* vfs_kbd_open(void) {
    return &s_kbd_file;
}

// ── Raw scancode device (/dev/kbdraw) ────────────────────────────────────
// Backed by evdev's raw scancode buffer.  Non-blocking: returns 0 if empty.

static int64_t kbdraw_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    if (!len) return 0;
    int n = evdev_getraw((uint8_t*)buf, (int)len);
    return (int64_t)n;
}

static vfs_file_t s_kbdraw_file = {
    .read  = kbdraw_read,
    .write = NULL,
    .close = vga_noop_close,
    .ctx   = NULL,
    .flags = 0,
};

vfs_file_t* vfs_kbdraw_open(void) {
    return &s_kbdraw_file;
}

// ── Mouse device (/dev/mouse) ─────────────────────────────────────────────
// Returns packed mouse_event_t structs.  Non-blocking: returns 0 bytes if
// no events are pending.  Callers should read in multiples of sizeof(mouse_event_t).

static int64_t mouse_dev_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    if (!len) return 0;
    int max = (int)(len / sizeof(mouse_event_t));
    if (max == 0) return 0;
    int n = mouse_read((mouse_event_t*)buf, max);
    return (int64_t)(n * (int)sizeof(mouse_event_t));
}

static vfs_file_t s_mouse_file = {
    .read  = mouse_dev_read,
    .write = NULL,
    .close = vga_noop_close,
    .ctx   = NULL,
    .flags = 0,
};

vfs_file_t* vfs_mouse_open(void) {
    return &s_mouse_file;
}

// ── DSP device (/dev/dsp) ─────────────────────────────────────────────────
// Write-only.  Accepts signed 16-bit stereo PCM at HDA_SAMPLE_RATE Hz.
// Blocks (IRQ-driven) when the HDA DMA ring is full.

static int64_t dsp_write(vfs_file_t* self, const void* buf, uint64_t len) {
    (void)self;
    return (int64_t)hda_write(buf, (uint32_t)len);
}

static vfs_file_t s_dsp_file = {
    .read  = NULL,
    .write = dsp_write,
    .close = vga_noop_close,
    .ctx   = NULL,
    .flags = 0,
};

vfs_file_t* vfs_dsp_open(void) {
    return &s_dsp_file;
}

// ── /dev/null device ──────────────────────────────────────────────────────
// Reads return 0 (EOF), writes consume silently.

static int64_t null_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self; (void)buf; (void)len;
    return 0; // EOF
}

static int64_t null_write(vfs_file_t* self, const void* buf, uint64_t len) {
    (void)self; (void)buf;
    return (int64_t)len; // swallow all
}

static vfs_file_t s_null_file = {
    .read  = null_read,
    .write = null_write,
    .close = vga_noop_close,
    .ctx   = NULL,
    .flags = 0,
};

vfs_file_t* vfs_null_open(void) {
    return &s_null_file;
}

// ── /dev/zero device ─────────────────────────────────────────────────────
// Reads return zeroed bytes; writes are consumed silently.

static int64_t zero_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    uint8_t* p = (uint8_t*)buf;
    for (uint64_t i = 0; i < len; i++) p[i] = 0;
    return (int64_t)len;
}

static vfs_file_t s_zero_file = {
    .read  = zero_read,
    .write = null_write, // reuse: consume silently
    .close = vga_noop_close,
    .ctx   = NULL,
    .flags = 0,
};

vfs_file_t* vfs_zero_open(void) {
    return &s_zero_file;
}

// ── /dev/urandom device ───────────────────────────────────────────────────
// Returns TSC-seeded pseudo-random bytes.

static uint64_t s_urandom_state = 0x123456789abcdef0ULL;

static int64_t urandom_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    uint8_t* p = (uint8_t*)buf;
    for (uint64_t i = 0; i < len; i++) {
        // xorshift64
        s_urandom_state ^= s_urandom_state << 13;
        s_urandom_state ^= s_urandom_state >> 7;
        s_urandom_state ^= s_urandom_state << 17;
        p[i] = (uint8_t)s_urandom_state;
    }
    return (int64_t)len;
}

static vfs_file_t s_urandom_file = {
    .read  = urandom_read,
    .write = null_write, // consume silently
    .close = vga_noop_close,
    .ctx   = NULL,
    .flags = 0,
};

vfs_file_t* vfs_urandom_open(void) {
    // Seed with TSC on first call.
    if (s_urandom_state == 0x123456789abcdef0ULL) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        s_urandom_state ^= ((uint64_t)hi << 32) | lo;
    }
    return &s_urandom_file;
}
