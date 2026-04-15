#include "fb.h"
#include "font.h"
#include "preempt.h"
#include "common.h"

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
    // If the background colour is all-zero bytes we can memset the whole
    // frame buffer in one shot — GCC lowers this to `rep stosb` or an SSE
    // store loop depending on target.
    if (g_fb_bg == 0) {
        __builtin_memset(row_ptr, 0, (uint64_t)rows * g_fb.pitch);
    } else {
        // Non-zero colour: fill one row with 4-byte writes, then memcpy it
        // to every subsequent row.  Turns an N×M inner loop into one loop
        // plus N memcpys.
        for (uint32_t x = 0; x < cols; x++) row_ptr[x] = g_fb_bg;
        uint8_t* first = (uint8_t*)row_ptr;
        uint8_t* cur   = first + g_fb.pitch;
        for (uint32_t y = 1; y < rows; y++) {
            __builtin_memcpy(cur, first, g_fb.pitch);
            cur += g_fb.pitch;
        }
    }
    g_fb_col = 0;
    g_fb_row = 0;
}

void fb_putc_at(uint32_t col, uint32_t row, char c, uint32_t fg, uint32_t bg) {
    const unsigned char* glyph = g_font8x16[(unsigned char)c];
    uint8_t* base = (uint8_t*)g_fb.base_virt
                    + row * 16 * g_fb.pitch
                    + col * 8 * 4;
    // Fully unrolled 8-wide glyph row to eliminate the inner branch and
    // let the compiler schedule the 8 stores as a single burst.  Each
    // character is 16 rows × 8 pixels = 128 pixel writes per glyph.
    for (uint32_t gy = 0; gy < 16; gy++) {
        uint32_t* px = (uint32_t*)(base + gy * g_fb.pitch);
        uint8_t bits = glyph[gy];
        px[0] = (bits & 0x80) ? fg : bg;
        px[1] = (bits & 0x40) ? fg : bg;
        px[2] = (bits & 0x20) ? fg : bg;
        px[3] = (bits & 0x10) ? fg : bg;
        px[4] = (bits & 0x08) ? fg : bg;
        px[5] = (bits & 0x04) ? fg : bg;
        px[6] = (bits & 0x02) ? fg : bg;
        px[7] = (bits & 0x01) ? fg : bg;
    }
}

void fb_term_scroll(void) {
    // TRACER: every 64th scroll — keeps the tracer alive while we dig
    // into whatever caused the char-by-char slowdown.  Remove once we
    // have the root cause.
    {
        static uint32_t s_scroll_cnt = 0;
        if ((s_scroll_cnt++ & 0x3F) == 0) {
            serial_puts_dbg("[trace] fb_scroll cnt=");
            serial_hex_dbg(s_scroll_cnt);
        }
    }
    uint32_t rows = fb_rows();
    // Copy rows 1..rows-1 up to rows 0..rows-2.  dst < src so a forward
    // memcpy is safe; no memmove needed.  GCC lowers this to `rep movsb`
    // (or an SSE copy loop depending on target) — 100× faster than the
    // byte-at-a-time loop this replaces.
    uint8_t* dst = (uint8_t*)g_fb.base_virt;
    uint8_t* src = dst + 16 * g_fb.pitch;
    uint64_t bytes = (uint64_t)(rows - 1) * 16 * g_fb.pitch;
    __builtin_memcpy(dst, src, bytes);

    // Clear last row.  Same trick as fb_clear: memset if bg==0,
    // otherwise build one row and memcpy-replicate downward.
    uint8_t* last = dst + (rows - 1) * 16 * g_fb.pitch;
    if (g_fb_bg == 0) {
        __builtin_memset(last, 0, 16ULL * g_fb.pitch);
    } else {
        uint32_t cols_px = g_fb.pitch / 4;
        uint32_t* first_row = (uint32_t*)last;
        for (uint32_t x = 0; x < cols_px; x++) first_row[x] = g_fb_bg;
        uint8_t* cur = last + g_fb.pitch;
        for (uint32_t y = 1; y < 16; y++) {
            __builtin_memcpy(cur, last, g_fb.pitch);
            cur += g_fb.pitch;
        }
    }
    g_fb_row = rows - 1;
}

// Internal: emit a single character assuming the caller already holds
// preempt_disable.  No locking of its own.  Factored out of fb_term_putc
// so fb_term_write can reuse it inside one big preempt-disabled section.
static inline void fb_term_putc_locked(char c) {
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

void fb_term_putc(char c) {
    // Serialize against preemption: two tasks writing to the console
    // concurrently would race on g_fb_row/col AND on framebuffer pixels
    // (especially during scroll, which touches the whole screen).
    // Holding preempt off across one character is cheap and guarantees
    // clean output.
    preempt_disable();
    fb_term_putc_locked(c);
    preempt_enable();
}

// Batched writer: take preempt_disable once for the whole user buffer
// instead of once per byte.  This is the hot path for tty0 writes from
// bash / ps / login, where `write(1, buf, len)` would otherwise fire
// `len` preempt_disable/enable cycles and `len` checks of
// reschedule_pending — on a long output line that's thousands of
// unnecessary scheduler round-trips.
void fb_term_write(const char* buf, uint64_t len) {
    if (!buf || !len) return;
    preempt_disable();
    for (uint64_t i = 0; i < len; i++)
        fb_term_putc_locked(buf[i]);
    preempt_enable();
}
