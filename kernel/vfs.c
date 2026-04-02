#include "vfs.h"
#include "keyboard.h"
#include "common.h"

// ── VGA console device ────────────────────────────────────────────────────

#define VGA_COLS 80
#define VGA_ROWS 25

// g_vga is defined in syscall.c (backed by VGA_ADDR + HHDM_OFFSET).
extern volatile uint16_t* g_vga;

uint32_t g_vga_row = 0;
uint32_t g_vga_col = 0;

// ── Current working directory ─────────────────────────────────────────────
char g_cwd[256] = "/";

static void vga_putchar(char c) {
    if (c == '\n' || g_vga_col >= VGA_COLS) {
        g_vga_col = 0;
        g_vga_row++;
        if (g_vga_row >= VGA_ROWS) {
            // Scroll up one row.
            for (uint32_t r = 0; r < VGA_ROWS - 1; r++)
                for (uint32_t c2 = 0; c2 < VGA_COLS; c2++)
                    g_vga[r * VGA_COLS + c2] = g_vga[(r + 1) * VGA_COLS + c2];
            for (uint32_t c2 = 0; c2 < VGA_COLS; c2++)
                g_vga[(VGA_ROWS - 1) * VGA_COLS + c2] = (uint16_t)' ' | (0x07U << 8);
            g_vga_row = VGA_ROWS - 1;
        }
        if (c == '\n') return;
    }
    g_vga[g_vga_row * VGA_COLS + g_vga_col] = (uint16_t)(uint8_t)c | (0x07U << 8);
    g_vga_col++;
}

static int64_t vga_write(vfs_file_t* self, const void* buf, uint64_t len) {
    (void)self;
    const char* s = (const char*)buf;
    for (uint64_t i = 0; i < len; i++)
        vga_putchar(s[i]);
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
