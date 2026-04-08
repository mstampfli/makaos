#pragma once
#include "common.h"

/* ── Framebuffer info ──────────────────────────────────────────────────────── */
typedef struct {
    uint64_t base_virt;  /* virtual address = fb_phys + HHDM_OFFSET */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint32_t bpp;        /* bits per pixel (32 for BGRX) */
} fb_info_t;

extern fb_info_t g_fb;

/* ── RGB color helpers (32-bit BGRX, little-endian: B G R X in memory) ──── */
#define FB_COLOR(r,g,b)  ((uint32_t)((uint32_t)(b) | ((uint32_t)(g)<<8) | ((uint32_t)(r)<<16)))
#define FB_BLACK    FB_COLOR(0x00,0x00,0x00)
#define FB_WHITE    FB_COLOR(0xFF,0xFF,0xFF)
#define FB_GRAY     FB_COLOR(0xAA,0xAA,0xAA)
#define FB_DGRAY    FB_COLOR(0x55,0x55,0x55)
#define FB_RED      FB_COLOR(0xFF,0x00,0x00)
#define FB_LRED     FB_COLOR(0xFF,0x55,0x55)
#define FB_GREEN    FB_COLOR(0x00,0xAA,0x00)
#define FB_LGREEN   FB_COLOR(0x55,0xFF,0x55)
#define FB_BLUE     FB_COLOR(0x00,0x00,0xAA)
#define FB_LBLUE    FB_COLOR(0x55,0x55,0xFF)
#define FB_CYAN     FB_COLOR(0x00,0xAA,0xAA)
#define FB_LCYAN    FB_COLOR(0x55,0xFF,0xFF)
#define FB_YELLOW   FB_COLOR(0xFF,0xFF,0x00)
#define FB_MAGENTA  FB_COLOR(0xAA,0x00,0xAA)

/* Map a VGA 4-bit color index to an RGB uint32_t */
static inline uint32_t fb_vga_color(uint8_t idx) {
    static const uint32_t pal[16] = {
        FB_BLACK,   FB_BLUE,    FB_GREEN,   FB_CYAN,
        FB_RED,     FB_MAGENTA, FB_COLOR(0xAA,0x55,0x00), FB_GRAY,
        FB_DGRAY,   FB_LBLUE,   FB_LGREEN,  FB_LCYAN,
        FB_LRED,    FB_COLOR(0xFF,0x55,0xFF), FB_YELLOW, FB_WHITE,
    };
    return pal[idx & 0xF];
}

/* ── Console cursor & colors ───────────────────────────────────────────────── */
extern uint32_t g_fb_col;
extern uint32_t g_fb_row;
extern uint32_t g_fb_fg;
extern uint32_t g_fb_bg;

/* Console dimensions in characters */
static inline uint32_t fb_cols(void) { return g_fb.width  / 8; }
static inline uint32_t fb_rows(void) { return g_fb.height / 16; }

/* ── Exclusive mode ─────────────────────────────────────────────────────────── */
// When set, fb_term_putc / fb_clear are no-ops so a fullscreen app owns the FB.
extern int g_fb_exclusive;

/* ── API ────────────────────────────────────────────────────────────────────── */
void fb_init(uint64_t fb_phys, uint32_t w, uint32_t h, uint32_t pitch);
void fb_clear(void);
void fb_putc_at(uint32_t col, uint32_t row, char c, uint32_t fg, uint32_t bg);
void fb_term_putc(char c);
void fb_term_scroll(void);

/* Set foreground/background from VGA attribute byte (low nibble=fg, high=bg) */
static inline void fb_set_attr(uint8_t attr) {
    g_fb_fg = fb_vga_color(attr & 0xF);
    g_fb_bg = fb_vga_color((attr >> 4) & 0xF);
}
