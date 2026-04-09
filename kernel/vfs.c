#include "vfs.h"
#include "keyboard.h"
#include "mouse.h"
#include "hda.h"
#include "common.h"
#include "fb.h"

// ── Framebuffer console device ────────────────────────────────────────────

/* Legacy cursor globals — kept for compatibility with shell.c which reads them.
   They are kept in sync with g_fb_row / g_fb_col after every write. */
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

// ── Keyboard device ───────────────────────────────────────────────────────

static int64_t kbd_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    if (len == 0) return 0;
    char* dst = (char*)buf;
    uint64_t i = 0;
    // Block until first character.
    dst[i++] = keyboard_wait();
    // Drain any further buffered characters without blocking.
    while (i < len) {
        char c = keyboard_getchar();
        if (!c) break;
        dst[i++] = c;
    }
    return (int64_t)i;
}

static vfs_file_t s_kbd_file = {
    .read  = kbd_read,
    .write = NULL,         // read-only device
    .close = vga_noop_close,
    .ctx   = NULL,
    .flags = 0,
};

vfs_file_t* vfs_kbd_open(void) {
    return &s_kbd_file;
}

// ── Raw scancode device (/dev/kbdraw) ────────────────────────────────────
// Returns raw PS/2 scancodes including E0 prefixes and break codes.
// Reads are non-blocking: returns 0 bytes if the buffer is empty.

static int64_t kbdraw_read(vfs_file_t* self, void* buf, uint64_t len) {
    (void)self;
    if (!len) return 0;
    int n = keyboard_getraw((uint8_t*)buf, (int)len);
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
