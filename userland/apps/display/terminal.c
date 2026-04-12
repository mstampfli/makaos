// MakaDisplay Terminal Emulator — runs bash in a window via PTY
//
// Architecture:
//   terminal.c (display client) ←→ PTY master fd ←→ PTY slave ←→ bash
//
// Keyboard events from compositor → translated to ASCII → write to PTY master
// PTY master read → ANSI parser → render glyphs into pixel buffer → commit

#include "libdisplay.h"
#include "../home/font8x16.h"

// ── Terminal dimensions ─────────────────────────────────────────────────
// Grid dimensions (cols/rows) and pixel buffer dimensions (win_w/win_h)
// are dynamic — reallocated on MD_SURFACE_CONFIGURE (window resize).

#define GLYPH_W      8
#define GLYPH_H      16
#define INIT_COLS    80
#define INIT_ROWS    25

static int      g_cols = INIT_COLS;
static int      g_rows = INIT_ROWS;
static uint32_t g_win_w = INIT_COLS * GLYPH_W;
static uint32_t g_win_h = INIT_ROWS * GLYPH_H;

// ── ANSI color palette (standard 8 + bright 8) ─────────────────────────

static const uint32_t g_ansi_colors[16] = {
    // BGRX format: B | (G<<8) | (R<<16)
    0x00000000,  // 0: black
    0x00000080,  // 1: red      → R=0x80
    0x00008000,  // 2: green    → G=0x80
    0x00008080,  // 3: yellow   → R=0x80 G=0x80
    0x00800000,  // 4: blue     → B=0x80
    0x00800080,  // 5: magenta  → B=0x80 R=0x80
    0x00808000,  // 6: cyan     → B=0x80 G=0x80
    0x00C0C0C0,  // 7: white    → light gray
    0x00808080,  // 8: bright black (dark gray)
    0x000000FF,  // 9: bright red
    0x0000FF00,  // 10: bright green
    0x0000FFFF,  // 11: bright yellow
    0x00FF0000,  // 12: bright blue
    0x00FF00FF,  // 13: bright magenta
    0x00FFFF00,  // 14: bright cyan
    0x00FFFFFF,  // 15: bright white
};

// ── Terminal cell ───────────────────────────────────────────────────────

typedef struct {
    char     ch;
    uint8_t  fg;    // color index (0-15)
    uint8_t  bg;    // color index (0-15)
    uint8_t  attrs; // bold, etc.
} term_cell_t;

// ── Terminal state ──────────────────────────────────────────────────────

// Dynamically allocated cell grid: g_cols * g_rows cells, row-major.
static term_cell_t* g_cells;
#define CELL(r, c) (g_cells[(r) * g_cols + (c)])
static int         g_cur_row;
static int         g_cur_col;
static uint8_t     g_cur_fg = 7;   // default: white on black
static uint8_t     g_cur_bg = 0;
static int         g_cur_visible = 1;
static int         g_running = 1;
static int         g_pty_master_fd = -1;

// ANSI escape sequence parser state
#define ESC_NONE     0
#define ESC_GOT_ESC  1
#define ESC_GOT_CSI  2
#define ESC_GOT_OSC  3

static int    g_esc_state;
static int    g_esc_params[16];
static int    g_esc_param_count;
static int    g_esc_private;  // '?' prefix

// Display server objects
static md_display_t*        g_dpy;
static md_client_surface_t* g_surf;
static md_client_buffer_t*  g_buf;
static uint32_t*            g_pixels;

// ── Dirty tracking ──────────────────────────────────────────────────────

static int g_dirty = 0;  // any cells changed since last commit?

// ── Rendering ───────────────────────────────────────────────────────────

static void render_cell(int row, int col) {
    if (row < 0 || row >= g_rows || col < 0 || col >= g_cols) return;

    term_cell_t* c = &CELL(row, col);
    uint32_t fg = g_ansi_colors[c->fg & 0xF];
    uint32_t bg = g_ansi_colors[c->bg & 0xF];

    int px = col * GLYPH_W;
    int py = row * GLYPH_H;

    unsigned char ch = (unsigned char)c->ch;
    const unsigned char* glyph = g_font8x16[ch];

    for (int y = 0; y < GLYPH_H; y++) {
        uint8_t bits = glyph[y];
        for (int x = 0; x < GLYPH_W; x++) {
            uint32_t color = (bits & (0x80 >> x)) ? fg : bg;
            g_pixels[(py + y) * g_win_w + (px + x)] = color;
        }
    }
}

// Draw cursor (inverted cell)
static void render_cursor(void) {
    if (!g_cur_visible) return;
    if (g_cur_row < 0 || g_cur_row >= g_rows) return;
    if (g_cur_col < 0 || g_cur_col >= g_cols) return;

    int px = g_cur_col * GLYPH_W;
    int py = g_cur_row * GLYPH_H;

    // Invert the bottom 2 scanlines for an underline cursor
    for (int y = GLYPH_H - 2; y < GLYPH_H; y++) {
        for (int x = 0; x < GLYPH_W; x++) {
            int idx = (py + y) * g_win_w + (px + x);
            g_pixels[idx] ^= 0x00FFFFFF;
        }
    }
}

static void render_all(void) {
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++)
            render_cell(r, c);
    render_cursor();
}

static void commit_display(void) {
    if (!g_dirty) return;
    render_all();
    md_surface_attach(g_surf, g_buf);
    md_surface_damage(g_surf, 0, 0, g_win_w, g_win_h);
    md_surface_commit(g_surf);
    g_dirty = 0;
}

// ── Terminal operations ─────────────────────────────────────────────────

static void term_scroll_up(void) {
    // Move rows 1..g_rows-1 up by one
    for (int r = 0; r < g_rows - 1; r++)
        for (int c = 0; c < g_cols; c++)
            CELL(r, c) = CELL(r + 1, c);

    // Clear last row
    for (int c = 0; c < g_cols; c++) {
        CELL(g_rows - 1, c).ch = ' ';
        CELL(g_rows - 1, c).fg = g_cur_fg;
        CELL(g_rows - 1, c).bg = g_cur_bg;
        CELL(g_rows - 1, c).attrs = 0;
    }
    g_dirty = 1;
}

static void term_newline(void) {
    g_cur_col = 0;
    g_cur_row++;
    if (g_cur_row >= g_rows) {
        g_cur_row = g_rows - 1;
        term_scroll_up();
    }
    g_dirty = 1;
}

static void term_putchar(char ch) {
    if (g_cur_col >= g_cols) {
        g_cur_col = 0;
        g_cur_row++;
        if (g_cur_row >= g_rows) {
            g_cur_row = g_rows - 1;
            term_scroll_up();
        }
    }
    CELL(g_cur_row, g_cur_col).ch = ch;
    CELL(g_cur_row, g_cur_col).fg = g_cur_fg;
    CELL(g_cur_row, g_cur_col).bg = g_cur_bg;
    CELL(g_cur_row, g_cur_col).attrs = 0;
    g_cur_col++;
    g_dirty = 1;
}

static void term_clear_row(int row, int from, int to) {
    if (row < 0 || row >= g_rows) return;
    if (from < 0) from = 0;
    if (to > g_cols) to = g_cols;
    for (int c = from; c < to; c++) {
        CELL(row, c).ch = ' ';
        CELL(row, c).fg = g_cur_fg;
        CELL(row, c).bg = g_cur_bg;
        CELL(row, c).attrs = 0;
    }
    g_dirty = 1;
}

static void term_clear_screen(void) {
    for (int r = 0; r < g_rows; r++)
        term_clear_row(r, 0, g_cols);
    g_dirty = 1;
}

// ── ANSI escape sequence handler ────────────────────────────────────────

static void csi_dispatch(char final) {
    int p0 = (g_esc_param_count > 0) ? g_esc_params[0] : 0;
    int p1 = (g_esc_param_count > 1) ? g_esc_params[1] : 0;

    switch (final) {
    case 'A': // Cursor Up
        g_cur_row -= (p0 ? p0 : 1);
        if (g_cur_row < 0) g_cur_row = 0;
        g_dirty = 1;
        break;
    case 'B': // Cursor Down
        g_cur_row += (p0 ? p0 : 1);
        if (g_cur_row >= g_rows) g_cur_row = g_rows - 1;
        g_dirty = 1;
        break;
    case 'C': // Cursor Forward (right)
        g_cur_col += (p0 ? p0 : 1);
        if (g_cur_col >= g_cols) g_cur_col = g_cols - 1;
        g_dirty = 1;
        break;
    case 'D': // Cursor Back (left)
        g_cur_col -= (p0 ? p0 : 1);
        if (g_cur_col < 0) g_cur_col = 0;
        g_dirty = 1;
        break;
    case 'H': // Cursor Position (row;col, 1-based)
    case 'f':
        g_cur_row = (p0 ? p0 - 1 : 0);
        g_cur_col = (p1 ? p1 - 1 : 0);
        if (g_cur_row >= g_rows) g_cur_row = g_rows - 1;
        if (g_cur_col >= g_cols) g_cur_col = g_cols - 1;
        if (g_cur_row < 0) g_cur_row = 0;
        if (g_cur_col < 0) g_cur_col = 0;
        g_dirty = 1;
        break;
    case 'J': // Erase in Display
        if (p0 == 0) {
            // Clear from cursor to end of screen
            term_clear_row(g_cur_row, g_cur_col, g_cols);
            for (int r = g_cur_row + 1; r < g_rows; r++)
                term_clear_row(r, 0, g_cols);
        } else if (p0 == 1) {
            // Clear from start to cursor
            for (int r = 0; r < g_cur_row; r++)
                term_clear_row(r, 0, g_cols);
            term_clear_row(g_cur_row, 0, g_cur_col + 1);
        } else if (p0 == 2) {
            // Clear entire screen
            term_clear_screen();
        }
        break;
    case 'K': // Erase in Line
        if (p0 == 0) {
            term_clear_row(g_cur_row, g_cur_col, g_cols);
        } else if (p0 == 1) {
            term_clear_row(g_cur_row, 0, g_cur_col + 1);
        } else if (p0 == 2) {
            term_clear_row(g_cur_row, 0, g_cols);
        }
        break;
    case 'L': { // Insert Lines
        int n = p0 ? p0 : 1;
        for (int i = 0; i < n && g_cur_row + n < g_rows; i++) {
            for (int r = g_rows - 1; r > g_cur_row; r--)
                for (int c = 0; c < g_cols; c++)
                    CELL(r, c) = CELL(r - 1, c);
            term_clear_row(g_cur_row, 0, g_cols);
        }
        break;
    }
    case 'M': { // Delete Lines
        int n = p0 ? p0 : 1;
        for (int i = 0; i < n; i++) {
            for (int r = g_cur_row; r < g_rows - 1; r++)
                for (int c = 0; c < g_cols; c++)
                    CELL(r, c) = CELL(r + 1, c);
            term_clear_row(g_rows - 1, 0, g_cols);
        }
        break;
    }
    case 'P': { // Delete Characters
        int n = p0 ? p0 : 1;
        for (int c = g_cur_col; c < g_cols - n; c++)
            CELL(g_cur_row, c) = CELL(g_cur_row, c + n);
        term_clear_row(g_cur_row, g_cols - n, g_cols);
        break;
    }
    case '@': { // Insert Characters
        int n = p0 ? p0 : 1;
        for (int c = g_cols - 1; c >= g_cur_col + n; c--)
            CELL(g_cur_row, c) = CELL(g_cur_row, c - n);
        term_clear_row(g_cur_row, g_cur_col, g_cur_col + n);
        break;
    }
    case 'm': // SGR — Select Graphic Rendition
        if (g_esc_param_count == 0) {
            // ESC[m = reset
            g_cur_fg = 7;
            g_cur_bg = 0;
        }
        for (int i = 0; i < g_esc_param_count; i++) {
            int p = g_esc_params[i];
            if (p == 0) {
                g_cur_fg = 7; g_cur_bg = 0;
            } else if (p == 1) {
                // Bold: use bright colors
                if (g_cur_fg < 8) g_cur_fg += 8;
            } else if (p == 22) {
                // Normal intensity
                if (g_cur_fg >= 8) g_cur_fg -= 8;
            } else if (p >= 30 && p <= 37) {
                g_cur_fg = (uint8_t)(p - 30);
            } else if (p >= 40 && p <= 47) {
                g_cur_bg = (uint8_t)(p - 40);
            } else if (p == 39) {
                g_cur_fg = 7;  // default fg
            } else if (p == 49) {
                g_cur_bg = 0;  // default bg
            } else if (p >= 90 && p <= 97) {
                g_cur_fg = (uint8_t)(p - 90 + 8);  // bright fg
            } else if (p >= 100 && p <= 107) {
                g_cur_bg = (uint8_t)(p - 100 + 8);  // bright bg
            } else if (p == 7) {
                // Reverse video
                uint8_t tmp = g_cur_fg;
                g_cur_fg = g_cur_bg;
                g_cur_bg = tmp;
            }
        }
        break;
    case 'n': // Device Status Report
        if (p0 == 6) {
            // Report cursor position: ESC [ row ; col R
            char resp[32];
            int len = 0;
            resp[len++] = '\x1b';
            resp[len++] = '[';
            // row (1-based)
            int r = g_cur_row + 1;
            if (r >= 10) resp[len++] = '0' + r / 10;
            resp[len++] = '0' + r % 10;
            resp[len++] = ';';
            // col (1-based)
            int cc = g_cur_col + 1;
            if (cc >= 10) resp[len++] = '0' + cc / 10;
            resp[len++] = '0' + cc % 10;
            resp[len++] = 'R';
            if (g_pty_master_fd >= 0)
                write(g_pty_master_fd, resp, len);
        }
        break;
    case 'h': // Set Mode
        if (g_esc_private && p0 == 25) g_cur_visible = 1; // show cursor
        break;
    case 'l': // Reset Mode
        if (g_esc_private && p0 == 25) g_cur_visible = 0; // hide cursor
        break;
    case 'd': // Vertical Line Position Absolute
        g_cur_row = (p0 ? p0 - 1 : 0);
        if (g_cur_row >= g_rows) g_cur_row = g_rows - 1;
        g_dirty = 1;
        break;
    case 'G': // Cursor Horizontal Absolute
        g_cur_col = (p0 ? p0 - 1 : 0);
        if (g_cur_col >= g_cols) g_cur_col = g_cols - 1;
        g_dirty = 1;
        break;
    case 'S': { // Scroll Up
        int n = p0 ? p0 : 1;
        for (int i = 0; i < n; i++) term_scroll_up();
        break;
    }
    case 'T': { // Scroll Down
        int n = p0 ? p0 : 1;
        for (int i = 0; i < n; i++) {
            for (int r = g_rows - 1; r > 0; r--)
                for (int c = 0; c < g_cols; c++)
                    CELL(r, c) = CELL(r - 1, c);
            term_clear_row(0, 0, g_cols);
        }
        break;
    }
    case 'X': { // Erase Characters
        int n = p0 ? p0 : 1;
        for (int i = 0; i < n && g_cur_col + i < g_cols; i++) {
            CELL(g_cur_row, g_cur_col + i).ch = ' ';
            CELL(g_cur_row, g_cur_col + i).fg = g_cur_fg;
            CELL(g_cur_row, g_cur_col + i).bg = g_cur_bg;
        }
        g_dirty = 1;
        break;
    }
    }
}

// ── Process one byte of output from the PTY ─────────────────────────────

static void term_process_char(char c) {
    switch (g_esc_state) {
    case ESC_NONE:
        if (c == '\x1b') {
            g_esc_state = ESC_GOT_ESC;
            g_esc_param_count = 0;
            g_esc_private = 0;
            for (int i = 0; i < 16; i++) g_esc_params[i] = 0;
        } else if (c == '\n') {
            term_newline();
        } else if (c == '\r') {
            g_cur_col = 0;
            g_dirty = 1;
        } else if (c == '\b' || c == 127) {
            if (g_cur_col > 0) g_cur_col--;
            g_dirty = 1;
        } else if (c == '\t') {
            int next = (g_cur_col + 8) & ~7;
            if (next > g_cols) next = g_cols;
            while (g_cur_col < next) term_putchar(' ');
        } else if (c == '\a') {
            // Bell — ignore
        } else if (c == '\f') {
            // Form feed — clear screen
            term_clear_screen();
            g_cur_row = 0;
            g_cur_col = 0;
        } else if ((unsigned char)c >= 0x20) {
            term_putchar(c);
        }
        break;

    case ESC_GOT_ESC:
        if (c == '[') {
            g_esc_state = ESC_GOT_CSI;
        } else if (c == ']') {
            g_esc_state = ESC_GOT_OSC;
        } else if (c == '(') {
            // Character set designation — ignore next byte
            g_esc_state = ESC_NONE;  // simplified: skip
        } else if (c == 'c') {
            // Full reset
            term_clear_screen();
            g_cur_row = 0; g_cur_col = 0;
            g_cur_fg = 7; g_cur_bg = 0;
            g_esc_state = ESC_NONE;
        } else if (c == 'M') {
            // Reverse Index — move cursor up, scroll if at top
            if (g_cur_row > 0) {
                g_cur_row--;
            } else {
                // Scroll down
                for (int r = g_rows - 1; r > 0; r--)
                    for (int cc = 0; cc < g_cols; cc++)
                        CELL(r, cc) = CELL(r - 1, cc);
                term_clear_row(0, 0, g_cols);
            }
            g_dirty = 1;
            g_esc_state = ESC_NONE;
        } else {
            g_esc_state = ESC_NONE;
        }
        break;

    case ESC_GOT_CSI:
        if (c == '?') {
            g_esc_private = 1;
        } else if (c >= '0' && c <= '9') {
            if (g_esc_param_count == 0) g_esc_param_count = 1;
            g_esc_params[g_esc_param_count - 1] =
                g_esc_params[g_esc_param_count - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (g_esc_param_count < 16) g_esc_param_count++;
        } else if (c >= 0x40 && c <= 0x7E) {
            // Final byte — dispatch
            csi_dispatch(c);
            g_esc_state = ESC_NONE;
        } else {
            // Unknown intermediate — abort
            g_esc_state = ESC_NONE;
        }
        break;

    case ESC_GOT_OSC:
        // OSC sequence: eat everything until BEL or ST (ESC \)
        if (c == '\a' || c == '\x1b') {
            g_esc_state = ESC_NONE;
        }
        break;
    }
}

// ── Keycode → ASCII translation ─────────────────────────────────────────
// Translates Linux-style keycodes to characters/escape sequences and
// writes them to the PTY master fd.

// US QWERTY keymap (unshifted and shifted)
static const char g_keymap_lower[128] = {
    [0] = 0,
    [1] = '\x1b',  // ESC
    [2] = '1', [3] = '2', [4] = '3', [5] = '4', [6] = '5',
    [7] = '6', [8] = '7', [9] = '8', [10] = '9', [11] = '0',
    [12] = '-', [13] = '=',
    [14] = '\b',    // Backspace
    [15] = '\t',    // Tab
    [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't',
    [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [26] = '[', [27] = ']',
    [28] = '\n',    // Enter
    [29] = 0,       // Left Ctrl
    [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
    [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l',
    [39] = ';', [40] = '\'', [41] = '`',
    [42] = 0,       // Left Shift
    [43] = '\\',
    [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
    [49] = 'n', [50] = 'm',
    [51] = ',', [52] = '.', [53] = '/',
    [54] = 0,       // Right Shift
    [55] = '*',     // KP *
    [56] = 0,       // Left Alt
    [57] = ' ',     // Space
};

static const char g_keymap_upper[128] = {
    [0] = 0,
    [1] = '\x1b',
    [2] = '!', [3] = '@', [4] = '#', [5] = '$', [6] = '%',
    [7] = '^', [8] = '&', [9] = '*', [10] = '(', [11] = ')',
    [12] = '_', [13] = '+',
    [14] = '\b', [15] = '\t',
    [16] = 'Q', [17] = 'W', [18] = 'E', [19] = 'R', [20] = 'T',
    [21] = 'Y', [22] = 'U', [23] = 'I', [24] = 'O', [25] = 'P',
    [26] = '{', [27] = '}',
    [28] = '\n', [29] = 0,
    [30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F', [34] = 'G',
    [35] = 'H', [36] = 'J', [37] = 'K', [38] = 'L',
    [39] = ':', [40] = '"', [41] = '~',
    [42] = 0,
    [43] = '|',
    [44] = 'Z', [45] = 'X', [46] = 'C', [47] = 'V', [48] = 'B',
    [49] = 'N', [50] = 'M',
    [51] = '<', [52] = '>', [53] = '?',
    [54] = 0, [55] = '*', [56] = 0, [57] = ' ',
};

static void on_key(md_client_surface_t* surf, uint32_t keycode,
                   uint32_t modifiers, int pressed) {
    (void)surf;
    if (!pressed) return;  // only handle key press
    if (g_pty_master_fd < 0) return;

    int shift = modifiers & 1;  // MD_MOD_SHIFT
    int ctrl  = modifiers & 2;  // MD_MOD_CTRL

    // Arrow keys → ANSI escape sequences
    const char* seq = 0;
    switch (keycode) {
    case 103: seq = "\x1b[A"; break;  // KEY_UP
    case 108: seq = "\x1b[B"; break;  // KEY_DOWN
    case 106: seq = "\x1b[C"; break;  // KEY_RIGHT
    case 105: seq = "\x1b[D"; break;  // KEY_LEFT
    case 102: seq = "\x1b[H"; break;  // KEY_HOME
    case 107: seq = "\x1b[F"; break;  // KEY_END
    case 104: seq = "\x1b[5~"; break; // KEY_PAGEUP
    case 109: seq = "\x1b[6~"; break; // KEY_PAGEDOWN
    case 110: seq = "\x1b[2~"; break; // KEY_INSERT
    case 111: seq = "\x1b[3~"; break; // KEY_DELETE
    }
    if (seq) {
        int len = 0;
        while (seq[len]) len++;
        write(g_pty_master_fd, seq, len);
        return;
    }

    if (keycode >= 128) return;

    char ch = shift ? g_keymap_upper[keycode] : g_keymap_lower[keycode];
    if (!ch) return;

    // Ctrl+letter → control character (^A=1, ^C=3, etc.)
    if (ctrl && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 1;
    if (ctrl && ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 1;

    write(g_pty_master_fd, &ch, 1);
}

// Allocate / reallocate the cell grid for the current g_cols/g_rows.
// If old grid exists, preserves the overlapping top-left region.
static void alloc_cells(int old_cols, int old_rows) {
    term_cell_t* old = g_cells;
    size_t bytes = (size_t)g_cols * (size_t)g_rows * sizeof(term_cell_t);
    g_cells = (term_cell_t*)malloc(bytes);
    if (!g_cells) { g_running = 0; return; }
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            term_cell_t* dst = &g_cells[r * g_cols + c];
            if (old && r < old_rows && c < old_cols) {
                *dst = old[r * old_cols + c];
            } else {
                dst->ch = ' ';
                dst->fg = g_cur_fg;
                dst->bg = g_cur_bg;
                dst->attrs = 0;
            }
        }
    }
    if (old) free(old);
}

// Render callback invoked by md_surface_resize_commit with the freshly
// allocated buffer. This is where the terminal reacts to the new window
// size: reflow the grid, clamp the cursor, and repaint the whole surface
// into the new buffer. Because the helper sequences allocate→render→
// attach→commit→destroy, the old buffer is still attached on the server
// while we paint the new one, so there is no flicker or race.
typedef struct {
    int old_cols;
    int old_rows;
} resize_ctx_t;

static void resize_render(md_client_buffer_t* new_buf, void* userdata) {
    resize_ctx_t* ctx = (resize_ctx_t*)userdata;
    g_win_w  = md_buffer_width(new_buf);
    g_win_h  = md_buffer_height(new_buf);
    g_pixels = md_buffer_data(new_buf);

    g_cols = (int)(g_win_w / GLYPH_W);
    g_rows = (int)(g_win_h / GLYPH_H);
    if (g_cols < 1) g_cols = 1;
    if (g_rows < 1) g_rows = 1;

    alloc_cells(ctx->old_cols, ctx->old_rows);

    if (g_cur_row >= g_rows) g_cur_row = g_rows - 1;
    if (g_cur_col >= g_cols) g_cur_col = g_cols - 1;

    for (uint32_t i = 0; i < g_win_w * g_win_h; i++) g_pixels[i] = 0;
    render_all();
    g_dirty = 0;
}

static void on_configure(md_client_surface_t* surf, uint32_t width,
                         uint32_t height, uint32_t states) {
    (void)surf; (void)states;
    if (width == 0 || height == 0) return;
    if (width == g_win_w && height == g_win_h) return;

    resize_ctx_t ctx = { g_cols, g_rows };
    if (md_surface_resize_commit(g_surf, &g_buf, width, height,
                                 resize_render, &ctx) < 0) {
        return;  // allocation failed — keep old buffer
    }

    // Tell the child about the new grid so it reflows its prompt/layout.
    if (g_pty_master_fd >= 0) {
        winsize_t ws;
        ws.ws_row = g_rows;
        ws.ws_col = g_cols;
        ws.ws_xpixel = g_win_w;
        ws.ws_ypixel = g_win_h;
        ioctl(g_pty_master_fd, 0x5414, (void*)&ws);  // TIOCSWINSZ
    }
}

static void on_close(md_client_surface_t* surf) {
    (void)surf;
    // Match doom: die immediately. The kernel tears down every fd we own,
    // which closes the pty master (SIGHUPs bash) and the display socket
    // (compositor sees POLLHUP → client_disconnect → surface gone). Running
    // the graceful path from inside the dispatch callback was leaving the
    // surface on screen in cases where the post-loop teardown got stuck
    // behind a blocking pty/shm close.
    _exit(0);
}

// ── Helpers ─────────────────────────────────────────────────────────────

static void print(const char* s) {
    uint32_t len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

// ── Main ────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    print("makaterm: starting terminal emulator\n");

    // ── Open PTY ────────────────────────────────────────────────────────
    int pty_fds[2];
    if (openpty(pty_fds) < 0) {
        print("makaterm: openpty failed\n");
        return 1;
    }
    g_pty_master_fd = pty_fds[0];
    int slave_fd = pty_fds[1];

    print("makaterm: PTY opened\n");

    // ── Fork bash on PTY slave ──────────────────────────────────────────
    int child = fork();
    if (child < 0) {
        print("makaterm: fork failed\n");
        return 1;
    }

    if (child == 0) {
        // Child: set up PTY slave as stdin/stdout/stderr
        close(g_pty_master_fd);
        setsid();
        dup2(slave_fd, 0);
        dup2(slave_fd, 1);
        dup2(slave_fd, 2);
        if (slave_fd > 2) close(slave_fd);
        ioctl(0, TIOCSCTTY, (void*)0);

        winsize_t ws;
        ws.ws_row = g_rows;
        ws.ws_col = g_cols;
        ws.ws_xpixel = g_win_w;
        ws.ws_ypixel = g_win_h;
        ioctl(0, 0x5414, (void*)&ws);  // TIOCSWINSZ

        const char* bash_argv[] = { "bash", "--norc", "--noprofile", 0 };
        const char* bash_envp[] = { "TERM=xterm", "HOME=/", "PATH=/bin", 0 };
        execve("/bin/bash", bash_argv, bash_envp);

        write(1, "EXEC-FAIL\r\n", 11);
        _exit(1);
    }

    // Parent: close slave fd (child has it)
    close(slave_fd);

    print("makaterm: bash forked\n");

    // ── Connect to display server ───────────────────────────────────────
    g_dpy = md_display_connect();
    if (!g_dpy) {
        print("makaterm: failed to connect to display server\n");
        return 1;
    }

    g_surf = md_surface_create(g_dpy);
    if (!g_surf) {
        print("makaterm: failed to create surface\n");
        return 1;
    }

    md_surface_set_title(g_surf, "Terminal");
    md_surface_on_key(g_surf, on_key);
    md_surface_on_close(g_surf, on_close);
    md_surface_on_configure(g_surf, on_configure);

    print("makaterm: surface created, on_key set\n");

    g_buf = md_buffer_create(g_dpy, g_win_w, g_win_h);
    if (!g_buf) {
        print("makaterm: failed to create buffer\n");
        return 1;
    }
    g_pixels = md_buffer_data(g_buf);

    // Allocate the initial cell grid, then clear it.
    alloc_cells(0, 0);
    term_clear_screen();

    // Initial render
    g_dirty = 1;
    commit_display();

    print("makaterm: terminal window created\n");

    // ── Main event loop ─────────────────────────────────────────────────
    // Poll on both: display server socket + PTY master fd

    int dpy_fd = md_display_fd(g_dpy);

    while (g_running) {
        pollfd_t fds[2];
        fds[0].fd = dpy_fd;
        fds[0].events = POLLIN;
        fds[1].fd = g_pty_master_fd;
        fds[1].events = POLLIN;

        poll(fds, 2, g_dirty ? 16 : 100);

        // Handle display server events (keyboard input from compositor)
        if (fds[0].revents & POLLIN) {
            int r = md_display_dispatch(g_dpy);
            if (r < 0) break;
        }

        // Handle PTY master output (bash wrote something)
        if (fds[1].revents & POLLIN) {
            char pty_buf[1024];
            int n = (int)read(g_pty_master_fd, pty_buf, sizeof(pty_buf));
            if (n <= 0) {
                // Bash exited
                g_running = 0;
                break;
            }
            for (int i = 0; i < n; i++) {
                term_process_char(pty_buf[i]);
            }
        }

        // Commit any dirty state to display
        commit_display();
    }

    // Clean up
    close(g_pty_master_fd);
    md_buffer_destroy(g_buf);
    md_surface_destroy(g_surf);
    md_display_disconnect(g_dpy);

    return 0;
}
