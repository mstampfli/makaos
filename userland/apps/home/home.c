// MakaOS home screen + snake — user space app
// Draws via SYS_FB_BLIT (same path as doom).
// Input via /dev/kbdraw (raw PS/2 set-1 scancodes, same as doom).

#include "libc.h"
#include "stdio.h"

// ── FB blit helpers ───────────────────────────────────────────────────────

typedef struct { uint32_t width, height, pitch, bpp; uint64_t phys; } fb_info_t;

static fb_info_t g_fb;
static uint32_t* g_buf = 0;   // RGBA pixel buffer (width × height)
static uint32_t  g_cols = 80;
static uint32_t  g_rows = 25;

// VGA 16-color palette (RGBA 0xRRGGBBAA)
static const uint32_t g_pal[16] = {
    0x000000FF, // 0 BLACK
    0x0000AAFF, // 1 BLUE
    0x00AA00FF, // 2 GREEN
    0x00AAAAFF, // 3 CYAN
    0xAA0000FF, // 4 RED
    0xAA00AAFF, // 5 MAGENTA
    0xAA5500FF, // 6 BROWN
    0xAAAAAAFF, // 7 LGRAY
    0x555555FF, // 8 DGRAY
    0x5555FFFF, // 9 LBLUE
    0x55FF55FF, // A LGREEN
    0x55FFFFFF, // B LCYAN
    0xFF5555FF, // C LRED
    0xFF55FFFF, // D LMAGENTA
    0xFFFF55FF, // E YELLOW
    0xFFFFFFFF, // F WHITE
};

#define BLACK   0x0
#define BLUE    0x1
#define GREEN   0x2
#define CYAN    0x3
#define RED     0x4
#define DGRAY   0x8
#define LBLUE   0x9
#define LGREEN  0xA
#define LCYAN   0xB
#define LRED    0xC
#define LGRAY   0x7
#define WHITE   0xF
#define YELLOW  0xE
#define ATTR(bg,fg) (((uint8_t)(((bg)&0xF)<<4))|((uint8_t)((fg)&0xF)))

// 8×16 font — just ASCII printables, stored as 16 bytes per glyph.
// We embed a minimal version directly so we don't depend on the kernel font.
// This is the standard VGA 8×16 ROM font subset (public domain).
#include "font8x16.h"

static void putc_at(uint32_t col, uint32_t row, char c, uint8_t attr) {
    if (col >= g_cols || row >= g_rows) return;
    uint32_t fg = g_pal[attr & 0xF];
    uint32_t bg = g_pal[(attr >> 4) & 0xF];
    const uint8_t* glyph = g_font8x16[(uint8_t)c];
    uint32_t px0 = row * 16 * g_fb.width + col * 8;
    for (uint32_t gy = 0; gy < 16; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < 8; gx++)
            g_buf[px0 + gy * g_fb.width + gx] = (bits >> (7 - gx)) & 1 ? fg : bg;
    }
}

static void fb_flush(void) {
    syscall4(SYS_FB_BLIT, (uint64_t)g_buf, g_fb.width, g_fb.height, 0);
}

static uint32_t slen(const char* s) { uint32_t n=0; while(s[n]) n++; return n; }

static void puts_at(uint32_t row, uint32_t col, const char* s, uint8_t attr) {
    for (uint32_t i = 0; s[i] && col+i < g_cols; i++)
        putc_at(col+i, row, s[i], attr);
}

static void fill(uint8_t attr) {
    for (uint32_t r = 0; r < g_rows; r++)
        for (uint32_t c = 0; c < g_cols; c++)
            putc_at(c, r, ' ', attr);
}

static void fill_region(uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1, uint8_t attr) {
    for (uint32_t r = r0; r <= r1 && r < g_rows; r++)
        for (uint32_t c = c0; c <= c1 && c < g_cols; c++)
            putc_at(c, r, ' ', attr);
}

static void hline(uint32_t row, char ch, uint8_t attr) {
    for (uint32_t c = 0; c < g_cols; c++) putc_at(c, row, ch, attr);
}

static void center(uint32_t row, const char* s, uint8_t attr) {
    uint32_t len = slen(s);
    uint32_t col = len < g_cols ? (g_cols - len) / 2 : 0;
    puts_at(row, col, s, attr);
}

// ── Keyboard (raw PS/2 scancodes via /dev/kbdraw) ────────────────────────

#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x03
#define KEY_RIGHT 0x04

static int g_kbd_fd = -1;

static int sc_to_key(uint8_t sc, char* out) {
    if (sc & 0x80) return 0; // key release — ignore
    switch (sc) {
        case 0x48: *out = KEY_UP;    return 1;
        case 0x50: *out = KEY_DOWN;  return 1;
        case 0x4B: *out = KEY_LEFT;  return 1;
        case 0x4D: *out = KEY_RIGHT; return 1;
        case 0x1C: *out = '\n';      return 1; // Enter
        case 0x01: *out = 27;        return 1; // ESC
        case 0x39: *out = ' ';       return 1; // Space
        case 0x0E: *out = '\b';      return 1; // Backspace
    }
    // Letter keys a-z
    static const char row1[] = "qwertyuiop";
    static const char row2[] = "asdfghjkl";
    static const char row3[] = "zxcvbnm";
    if (sc >= 0x10 && sc <= 0x19) { *out = row1[sc-0x10]; return 1; }
    if (sc >= 0x1E && sc <= 0x26) { *out = row2[sc-0x1E]; return 1; }
    if (sc >= 0x2C && sc <= 0x32) { *out = row3[sc-0x2C]; return 1; }
    return 0;
}

// Non-blocking: returns 0 if no key
static char kbd_getchar(void) {
    uint8_t sc;
    int64_t n = read_nonblock(g_kbd_fd, &sc, 1);
    if (n <= 0) return 0;
    char k = 0;
    if (sc_to_key(sc, &k)) return k;
    return 0;
}

// Blocking
static char kbd_wait(void) {
    for (;;) {
        uint8_t sc;
        int64_t n = read(g_kbd_fd, &sc, 1);
        if (n <= 0) continue;
        char k = 0;
        if (sc_to_key(sc, &k)) return k;
    }
}

// ── Timing ────────────────────────────────────────────────────────────────

static void sleep_ms(uint64_t ms) {
    timespec_t req = { ms/1000, (ms%1000)*1000000ULL };
    nanosleep(&req, 0);
}

// ── Home menu ─────────────────────────────────────────────────────────────

#define MENU_ITEMS 2
static const char* s_labels[MENU_ITEMS] = {
    "  Play Snake  ",
    "    (more coming soon)    ",
};

static void draw_home(int sel) {
    fill(ATTR(BLACK, LGRAY));
    static const char* art[] = {
        "__  __       _          ___  ____",
        "|  \\/  | __ _| | _ __ _ / _ \\/ ___|",
        "| |\\/| |/ _` | |/ / _` | | | \\___ \\",
        "| |  | | (_| |   < (_| | |_| |___) |",
        "|_|  |_|\\__,_|_|\\_\\__,_|\\___/|____/",
    };
    for (uint32_t i = 0; i < 5; i++)
        center(1+i, art[i], ATTR(BLACK, LCYAN));
    hline(7, '=', ATTR(BLACK, LBLUE));
    center(9,  "Welcome to MakaOS  --  A 64-bit hobbyist kernel", ATTR(BLACK, WHITE));
    hline(11, '-', ATTR(BLACK, DGRAY));
    puts_at(13, 4, "Version  :  0.1.1",                        ATTR(BLACK, YELLOW));
    puts_at(14, 4, "Platform :  x86_64  |  UEFI  |  GOP FB",   ATTR(BLACK, YELLOW));
    hline(16, '-', ATTR(BLACK, DGRAY));
    center(18, "-- SELECT --", ATTR(BLACK, LGRAY));
    for (int i = 0; i < MENU_ITEMS; i++) {
        uint8_t is_sel = (i == sel);
        uint8_t attr = is_sel ? ATTR(LBLUE, WHITE) : ATTR(BLACK, LGRAY);
        uint32_t len = slen(s_labels[i]);
        uint32_t col = len < g_cols ? (g_cols - len) / 2 : 0;
        if (is_sel) putc_at(col-2, 20+(uint32_t)i, '>', ATTR(BLACK, YELLOW));
        puts_at(20+(uint32_t)i, col, s_labels[i], attr);
    }
    puts_at(24, 2, "UP/DOWN = navigate   ENTER = select", ATTR(BLACK, DGRAY));
    fb_flush();
}

static int home_menu(void) {
    int sel = 0;
    draw_home(sel);
    for (;;) {
        char c = kbd_wait();
        if (c == KEY_UP   && sel > 0)              { sel--; draw_home(sel); }
        if (c == KEY_DOWN && sel < MENU_ITEMS-1)   { sel++; draw_home(sel); }
        if (c == '\n') return sel;
    }
}

// ── Snake ─────────────────────────────────────────────────────────────────

#define PLAY_R0  1
#define PLAY_C0  1
#define PLAY_R1  22
#define PLAY_C1  78
#define PLAY_H   (PLAY_R1 - PLAY_R0 + 1)
#define PLAY_W   (PLAY_C1 - PLAY_C0 + 1)
#define MAX_SNAKE (PLAY_H * PLAY_W)

typedef struct { int16_t r, c; } pos_t;
static pos_t    s_body[MAX_SNAKE];
static int32_t  s_head, s_len;
static int16_t  s_dr, s_dc;
static pos_t    s_food;
static uint32_t s_score;
static uint32_t s_rand = 12345;

static uint32_t rand_next(void) {
    s_rand = s_rand * 1664525u + 1013904223u;
    return s_rand;
}

static void draw_cell(pos_t p, char ch, uint8_t attr) {
    putc_at((uint32_t)(p.c + PLAY_C0), (uint32_t)(p.r + PLAY_R0), ch, attr);
}

static void draw_snake_border(void) {
    uint8_t ba = ATTR(DGRAY, LGRAY);
    for (uint32_t c = 0; c < g_cols; c++) {
        putc_at(c, 0, '-', ba);
        putc_at(c, PLAY_R1+1, '-', ba);
    }
    for (uint32_t r = 1; r <= (uint32_t)PLAY_R1; r++) {
        putc_at(0, r, '|', ba);
        putc_at(g_cols-1, r, '|', ba);
    }
    putc_at(0, 0, '+', ba); putc_at(g_cols-1, 0, '+', ba);
    putc_at(0, PLAY_R1+1, '+', ba); putc_at(g_cols-1, PLAY_R1+1, '+', ba);
}

static void draw_score(void) {
    fill_region(24, 0, 24, g_cols-1, ATTR(BLACK, LGRAY));
    puts_at(24, 2, "SCORE:", ATTR(BLACK, YELLOW));
    char buf[12]; buf[11] = '\0';
    uint32_t v = s_score; int32_t i = 10;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v && i > 0);
    puts_at(24, 9, &buf[i], ATTR(BLACK, YELLOW));
    puts_at(24, 30, "WASD/Arrows=move  ESC=menu", ATTR(BLACK, DGRAY));
}

static void place_food(void) {
    for (;;) {
        int16_t r = (int16_t)(rand_next() % PLAY_H);
        int16_t c = (int16_t)(rand_next() % PLAY_W);
        uint8_t ok = 1;
        for (int32_t i = 0; i < s_len; i++) {
            int32_t idx = (s_head - i + MAX_SNAKE) % MAX_SNAKE;
            if (s_body[idx].r == r && s_body[idx].c == c) { ok = 0; break; }
        }
        if (ok) { s_food.r = r; s_food.c = c; return; }
    }
}

static void snake_setup(void) {
    fill(ATTR(BLACK, BLACK));
    draw_snake_border();
    s_head = 2; s_len = 3; s_dr = 0; s_dc = 1; s_score = 0;
    s_body[0].r = 10; s_body[0].c = 10;
    s_body[1].r = 10; s_body[1].c = 11;
    s_body[2].r = 10; s_body[2].c = 12;
    draw_cell(s_body[0], 'o', ATTR(BLACK, LGREEN));
    draw_cell(s_body[1], 'o', ATTR(BLACK, LGREEN));
    draw_cell(s_body[2], 'O', ATTR(BLACK, LGREEN));
    place_food();
    draw_cell(s_food, '*', ATTR(BLACK, RED));
    draw_score();
    // seed rand with clock
    s_rand ^= (uint32_t)(clock_ns() & 0xFFFFFFFF);
    fb_flush();
}

static int snake_tick(void) {
    char c;
    int esc = 0;
    while ((c = kbd_getchar())) {
        if (c == 27 || c == 'q' || c == 'Q') esc = 1;
        if ((c == 'w' || c == 'W' || c == KEY_UP)    && s_dr != 1)  { s_dr=-1; s_dc= 0; }
        if ((c == 's' || c == 'S' || c == KEY_DOWN)  && s_dr != -1) { s_dr= 1; s_dc= 0; }
        if ((c == 'a' || c == 'A' || c == KEY_LEFT)  && s_dc != 1)  { s_dr= 0; s_dc=-1; }
        if ((c == 'd' || c == 'D' || c == KEY_RIGHT) && s_dc != -1) { s_dr= 0; s_dc= 1; }
    }
    if (esc) return -1;

    pos_t cur = s_body[s_head];
    pos_t next;
    next.r = (int16_t)(cur.r + s_dr);
    next.c = (int16_t)(cur.c + s_dc);
    if (next.r < 0 || next.r >= PLAY_H || next.c < 0 || next.c >= PLAY_W) return 0;

    uint8_t ate = (next.r == s_food.r && next.c == s_food.c);
    int32_t check = ate ? s_len : s_len - 1;
    for (int32_t i = 0; i < check; i++) {
        int32_t idx = (s_head - i + MAX_SNAKE) % MAX_SNAKE;
        if (s_body[idx].r == next.r && s_body[idx].c == next.c) return 0;
    }
    if (!ate) {
        int32_t tail = (s_head - s_len + 1 + MAX_SNAKE) % MAX_SNAKE;
        draw_cell(s_body[tail], ' ', ATTR(BLACK, BLACK));
    }
    draw_cell(cur, 'o', ATTR(BLACK, LGREEN));
    s_head = (s_head + 1) % MAX_SNAKE;
    s_body[s_head] = next;
    draw_cell(next, 'O', ATTR(BLACK, LGREEN));
    if (ate) {
        s_len++; s_score += 10;
        s_rand ^= (uint32_t)(clock_ns() & 0xFFFFFFFF);
        place_food();
        draw_cell(s_food, '*', ATTR(BLACK, RED));
        draw_score();
    }
    fb_flush();
    return 1;
}

static int run_snake(void) {
    snake_setup();
    for (;;) {
        sleep_ms(80); // ~12 ticks/sec
        int r = snake_tick();
        if (r == -1) return 1;
        if (r ==  0) return 0;
    }
}

static void show_game_over(void) {
    fill_region(10, 22, 14, 57, ATTR(RED, WHITE));
    center(11, "  GAME OVER  ", ATTR(RED, WHITE));
    puts_at(12, 24, "  Final score: ", ATTR(RED, WHITE));
    char buf[12]; buf[11] = '\0';
    uint32_t v = s_score; int32_t i = 10;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v && i > 0);
    puts_at(12, 39, &buf[i], ATTR(RED, YELLOW));
    center(13, "  Press any key...  ", ATTR(RED, WHITE));
    fb_flush();
    // drain then wait
    while (kbd_getchar());
    kbd_wait();
}

// ── main ─────────────────────────────────────────────────────────────────

int main(void) {
    // Get framebuffer info
    syscall1(SYS_FB_INFO, (uint64_t)&g_fb);
    g_cols = g_fb.width  / 8;
    g_rows = g_fb.height / 16;

    // Allocate pixel buffer
    g_buf = (uint32_t*)malloc(g_fb.width * g_fb.height * 4);
    if (!g_buf) exit(1);

    // Open keyboard
    g_kbd_fd = open("/dev/kbdraw", 0, 0);
    if (g_kbd_fd < 0) exit(1);

    for (;;) {
        int choice = home_menu();
        if (choice == 0) {
            for (;;) {
                int back = run_snake();
                if (back) break;
                show_game_over();
                fill_region(15, 22, 15, 57, ATTR(BLACK, LGRAY));
                center(15, "  R = restart   ESC = menu  ", ATTR(BLACK, YELLOW));
                fb_flush();
                for (;;) {
                    char c = kbd_wait();
                    if (c == 'r' || c == 'R') break;
                    if (c == 27 || c == '\n') goto menu;
                }
            }
        }
        menu:;
    }
}
