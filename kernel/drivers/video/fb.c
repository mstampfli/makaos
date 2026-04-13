#include "fb.h"
#include "font.h"

fb_info_t g_fb = {0};

uint32_t g_fb_col = 0;
uint32_t g_fb_row = 0;
uint32_t g_fb_fg  = 0x00FFFFFF; /* white */
uint32_t g_fb_bg  = 0x00000000; /* black */

void fb_init(uint64_t fb_phys, uint32_t w, uint32_t h, uint32_t pitch) {
    g_fb.base_virt = fb_phys + HHDM_OFFSET;
    g_fb.width     = w;
    g_fb.height    = h;
    g_fb.pitch     = pitch;
    g_fb.bpp       = 32;
    g_fb_col = 0;
    g_fb_row = 0;
    g_fb_fg  = FB_WHITE;
    g_fb_bg  = FB_BLACK;
    fb_clear();
}

void fb_clear(void) {
    uint32_t rows = g_fb.height;
    uint32_t cols = g_fb.pitch / 4;
    uint32_t* row_ptr = (uint32_t*)g_fb.base_virt;
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < cols; x++)
            row_ptr[x] = g_fb_bg;
        row_ptr = (uint32_t*)((uint8_t*)row_ptr + g_fb.pitch);
    }
    g_fb_col = 0;
    g_fb_row = 0;
}

void fb_putc_at(uint32_t col, uint32_t row, char c, uint32_t fg, uint32_t bg) {
    const unsigned char* glyph = g_font8x16[(unsigned char)c];
    uint8_t* base = (uint8_t*)g_fb.base_virt
                    + row * 16 * g_fb.pitch
                    + col * 8 * 4;
    for (uint32_t gy = 0; gy < 16; gy++) {
        uint32_t* px = (uint32_t*)(base + gy * g_fb.pitch);
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < 8; gx++)
            px[gx] = (bits >> (7 - gx)) & 1 ? fg : bg;
    }
}

void fb_term_scroll(void) {
    uint32_t rows = fb_rows();
    /* Copy rows 1..rows-1 up to rows 0..rows-2 (pixel-level memmove) */
    uint8_t* dst = (uint8_t*)g_fb.base_virt;
    uint8_t* src = dst + 16 * g_fb.pitch;
    uint64_t bytes = (uint64_t)(rows - 1) * 16 * g_fb.pitch;
    /* Simple byte copy — no memmove dependency */
    for (uint64_t i = 0; i < bytes; i++)
        dst[i] = src[i];
    /* Clear last row */
    uint8_t* last = dst + (rows - 1) * 16 * g_fb.pitch;
    uint32_t cols_px = g_fb.pitch / 4;
    for (uint32_t y = 0; y < 16; y++) {
        uint32_t* px = (uint32_t*)(last + y * g_fb.pitch);
        for (uint32_t x = 0; x < cols_px; x++)
            px[x] = g_fb_bg;
    }
    g_fb_row = rows - 1;
}

void fb_term_putc(char c) {
    uint32_t cols = fb_cols();
    uint32_t rows = fb_rows();

    if (c == '\f') {  // form-feed: clear screen
        fb_clear();
        g_fb_row = 0;
        g_fb_col = 0;
        return;
    }
    if (c == '\r') {
        g_fb_col = 0;
        return;
    }
    if (c == '\n') {
        g_fb_col = 0;
        g_fb_row++;
        if (g_fb_row >= rows) fb_term_scroll();
        return;
    }
    if (c == '\b' || c == 127) {
        if (g_fb_col > 0) {
            g_fb_col--;
            fb_putc_at(g_fb_col, g_fb_row, ' ', g_fb_fg, g_fb_bg);
        }
        return;
    }
    if (g_fb_col >= cols) {
        g_fb_col = 0;
        g_fb_row++;
        if (g_fb_row >= rows) fb_term_scroll();
    }
    fb_putc_at(g_fb_col, g_fb_row, c, g_fb_fg, g_fb_bg);
    g_fb_col++;
}
