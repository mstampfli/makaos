#include "common.h"
#include "keyboard.h"
#include "sched.h"
#include "timer.h"
#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "user_test_vmalloc.h"
#include "user_hello.h"
#include "fb.h"

/* ── Framebuffer drawing helpers (replaces VGA text mode) ───────────────── */
/* ATTR(bg,fg) packs two VGA color nibbles — we convert to RGB via fb_vga_color */
#define ATTR(bg,fg) (((uint8_t)(((bg)&0xF)<<4))|((uint8_t)((fg)&0xF)))

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

/* Character cell dimensions derived from current framebuffer */
static uint32_t fb_w(void) { return fb_cols(); }
static uint32_t fb_h(void) { return fb_rows(); }

static void vga_putc(uint32_t row, uint32_t col, char c, uint8_t attr) {
    if (row < fb_h() && col < fb_w())
        fb_putc_at(col, row, c, fb_vga_color(attr & 0xF), fb_vga_color((attr>>4)&0xF));
}

static void vga_puts(uint32_t row, uint32_t col, const char* s, uint8_t attr) {
    for (uint32_t i = 0; s[i] && col + i < fb_w(); i++)
        vga_putc(row, col + i, s[i], attr);
}

static void vga_fill(uint8_t attr) {
    uint32_t bg = fb_vga_color((attr >> 4) & 0xF);
    uint32_t fg = fb_vga_color(attr & 0xF);
    for (uint32_t r = 0; r < fb_h(); r++)
        for (uint32_t c = 0; c < fb_w(); c++)
            fb_putc_at(c, r, ' ', fg, bg);
}

static void vga_fill_region(uint32_t r0, uint32_t c0,
                             uint32_t r1, uint32_t c1, uint8_t attr) {
    for (uint32_t r = r0; r <= r1 && r < fb_h(); r++)
        for (uint32_t c = c0; c <= c1 && c < fb_w(); c++)
            vga_putc(r, c, ' ', attr);
}

static void vga_hline(uint32_t row, char ch, uint8_t attr) {
    for (uint32_t c = 0; c < fb_w(); c++)
        vga_putc(row, c, ch, attr);
}

static uint32_t kstrlen(const char* s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

static void vga_center(uint32_t row, const char* s, uint8_t attr) {
    uint32_t len = kstrlen(s);
    uint32_t col = len < fb_w() ? (fb_w() - len) / 2 : 0;
    vga_puts(row, col, s, attr);
}

/* ── delay ───────────────────────────────────────────────────────────────── */
extern volatile uint32_t g_irq_count;

static void delay_ticks(uint32_t ticks) {
    uint32_t start = g_irq_count;
    while (g_irq_count - start < ticks)
        sched_yield();
}

static char wait_key(void) {
    return keyboard_wait();
}

/* ═══════════════════════════════════════════════════════════════════════════
   HOME SCREEN
   ═══════════════════════════════════════════════════════════════════════════ */

#define MENU_ITEMS 2
static const char* s_menu_labels[MENU_ITEMS] = {
    "  Play Snake  ",
    "    (more coming soon)    ",
};

static void draw_home(int sel) {
    vga_fill(ATTR(BLACK, LGRAY));

    static const char* art[] = {
        "__  __       _          ___  ____",
        "|  \\/  | __ _| | _ __ _ / _ \\/ ___|",
        "| |\\/| |/ _` | |/ / _` | | | \\___ \\",
        "| |  | | (_| |   < (_| | |_| |___) |",
        "|_|  |_|\\__,_|_|\\_\\__,_|\\___/|____/",
    };
    for (uint32_t i = 0; i < 5; i++)
        vga_center(1 + i, art[i], ATTR(BLACK, LCYAN));

    vga_hline(7, '=', ATTR(BLACK, LBLUE));
    vga_center(9,  "Welcome to MakaOS  --  A 64-bit hobbyist kernel", ATTR(BLACK, WHITE));
    vga_hline(11, '-', ATTR(BLACK, DGRAY));
    vga_puts(13, 4, "Version  :  0.1.1",                         ATTR(BLACK, YELLOW));
    vga_puts(14, 4, "Platform :  x86_64  |  BIOS  |  VGA text",  ATTR(BLACK, YELLOW));
    vga_hline(16, '-', ATTR(BLACK, DGRAY));
    vga_center(18, "-- SELECT --", ATTR(BLACK, LGRAY));

    for (int i = 0; i < MENU_ITEMS; i++) {
        uint8_t is_sel = (i == sel);
        uint8_t attr  = is_sel ? ATTR(LBLUE, WHITE) : ATTR(BLACK, LGRAY);
        uint32_t len   = kstrlen(s_menu_labels[i]);
        uint32_t col   = len < fb_w() ? (fb_w() - len) / 2 : 0;
        if (is_sel) vga_putc(20 + (uint32_t)i, col - 2, '>', ATTR(BLACK, YELLOW));
        vga_puts(20 + (uint32_t)i, col, s_menu_labels[i], attr);
    }

    vga_puts(24, 2, "UP/DOWN = navigate   ENTER = select", ATTR(BLACK, DGRAY));
}

static int home_menu(void) {
    int sel = 0;
    draw_home(sel);

    for (;;) {
        char c = keyboard_wait();
        if (c == KEY_UP)   { if (sel > 0)              { sel--; draw_home(sel); } }
        if (c == KEY_DOWN) { if (sel < MENU_ITEMS - 1) { sel++; draw_home(sel); } }
        if (c == '\n')     return sel;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   SNAKE
   ═══════════════════════════════════════════════════════════════════════════ */

#define PLAY_R0  1
#define PLAY_C0  1
#define PLAY_R1  22
#define PLAY_C1  78
#define PLAY_H   (PLAY_R1 - PLAY_R0 + 1)
#define PLAY_W   (PLAY_C1 - PLAY_C0 + 1)
#define MAX_SNAKE (PLAY_H * PLAY_W)

typedef struct { int16_t r, c; } pos_t;

static pos_t   s_body[MAX_SNAKE];
static int32_t s_head;
static int32_t s_len;
static int16_t s_dr, s_dc;
static pos_t   s_food;
static uint32_t s_score;

static uint32_t s_rand = 12345;
static uint32_t rand_next(void) {
    s_rand = s_rand * 1664525u + 1013904223u;
    return s_rand;
}

static void draw_snake_border(void) {
    uint8_t ba = ATTR(DGRAY, LGRAY);
    for (uint32_t c = 0; c < fb_w(); c++) {
        vga_putc(0,         c, '-', ba);
        vga_putc(PLAY_R1+1, c, '-', ba);
    }
    for (uint32_t r = 1; r <= PLAY_R1; r++) {
        vga_putc(r, 0,        '|', ba);
        vga_putc(r, fb_w()-1, '|', ba);
    }
    vga_putc(0, 0,               '+', ba);
    vga_putc(0, fb_w()-1,        '+', ba);
    vga_putc(PLAY_R1+1, 0,      '+', ba);
    vga_putc(PLAY_R1+1, fb_w()-1,'+', ba);
}

static void draw_cell(pos_t p, char ch, uint8_t attr) {
    vga_putc((uint32_t)(p.r + PLAY_R0), (uint32_t)(p.c + PLAY_C0), ch, attr);
}

static void draw_score_bar(void) {
    vga_fill_region(24, 0, 24, fb_w()-1, ATTR(BLACK, LGRAY));
    vga_puts(24, 2, "SCORE:", ATTR(BLACK, YELLOW));
    char buf[12]; buf[11] = '\0';
    uint32_t v = s_score; int32_t i = 10;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v && i > 0);
    vga_puts(24, 9, &buf[i], ATTR(BLACK, YELLOW));
    vga_puts(24, 30, "WASD/Arrows=move  ESC=menu", ATTR(BLACK, DGRAY));
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
    vga_fill(ATTR(BLACK, BLACK));
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
    draw_score_bar();
    s_rand ^= g_irq_count;
}

static int snake_tick(void) {
    char c;
    int esc = 0;
    while ((c = keyboard_getchar())) {
        if (c == 27 || c == 'q' || c == 'Q') { esc = 1; }
        if ((c == 'w' || c == 'W' || c == KEY_UP)    && s_dr != 1)  { s_dr=-1; s_dc= 0; }
        if ((c == 's' || c == 'S' || c == KEY_DOWN)  && s_dr != -1) { s_dr= 1; s_dc= 0; }
        if ((c == 'a' || c == 'A' || c == KEY_LEFT)  && s_dc != 1)  { s_dr= 0; s_dc=-1; }
        if ((c == 'd' || c == 'D' || c == KEY_RIGHT) && s_dc != -1) { s_dr= 0; s_dc= 1; }
    }
    if (esc) return -1;

    pos_t cur  = s_body[s_head];
    pos_t next;
    next.r = (int16_t)(cur.r + s_dr);
    next.c = (int16_t)(cur.c + s_dc);

    if (next.r < 0 || next.r >= PLAY_H || next.c < 0 || next.c >= PLAY_W)
        return 0;

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
        s_len++;
        s_score += 10;
        s_rand ^= g_irq_count;
        place_food();
        draw_cell(s_food, '*', ATTR(BLACK, RED));
        draw_score_bar();
    }
    return 1;
}

static int run_snake(void) {
    snake_setup();
    for (;;) {
        delay_ticks(8);
        int r = snake_tick();
        if (r == -1) return 1;
        if (r ==  0) return 0;
    }
}

static void show_game_over(void) {
    vga_fill_region(10, 22, 14, 57, ATTR(RED, WHITE));
    vga_center(11, "  GAME OVER  ",        ATTR(RED, WHITE));
    vga_puts(12, 24, "  Final score: ",     ATTR(RED, WHITE));
    char buf[12]; buf[11] = '\0';
    uint32_t v = s_score; int32_t i = 10;
    do { buf[--i] = '0' + (char)(v % 10); v /= 10; } while (v && i > 0);
    vga_puts(12, 39, &buf[i], ATTR(RED, YELLOW));
    vga_center(13, "  Press any key...  ", ATTR(RED, WHITE));
    while (keyboard_getchar());
    wait_key();
}

/* ═══════════════════════════════════════════════════════════════════════════
   PROCESS ENTRY POINTS
   ═══════════════════════════════════════════════════════════════════════════ */
void home_fn(void) {
    for (;;) {
        int choice = home_menu();

        if (choice == 0) {
            for (;;) {
                int back = run_snake();
                if (back) break;
                show_game_over();
                vga_fill_region(15, 22, 15, 57, ATTR(BLACK, LGRAY));
                vga_center(15, "  R = restart   ESC = menu  ", ATTR(BLACK, YELLOW));
                for (;;) {
                    char c = wait_key();
                    if (c == 'r' || c == 'R') break;
                    if (c == 27 || c == '\n') goto menu;
                }
            }
        }
        menu:;
    }
}

void snake_fn(void) {
    for (;;) sched_yield();
}

/* ── Ring-3 vmalloc test launcher ───────────────────────────────────────── */
static void fs_puts(uint32_t row, const char* s, uint8_t attr) {
    vga_puts(row, 0, s, attr);
}

void test_vmalloc_fn(void) {
    fs_puts(0, "[TEST] launching ring-3 vmalloc test...", ATTR(BLACK, YELLOW));

    // Copy the embedded binary into physical pages so task_create_user can map it.
    uint32_t bin_size  = g_user_test_vmalloc_size;
    uint32_t pages     = (bin_size + PAGE_SIZE - 1) / PAGE_SIZE;

    // Allocate contiguous physical pages (order = ceil_log2(pages)).
    uint8_t order = 0;
    while ((1u << order) < pages) order++;
    phys_addr_t code_phys = pmm_buddy_alloc(order);
    if (code_phys == PMM_INVALID_ADDR) {
        fs_puts(0, "[TEST] FAIL: out of memory for user binary", ATTR(RED, WHITE));
        for (;;) sched_yield();
    }

    // Copy binary into the physical frames via HHDM.
    uint8_t* dst = (uint8_t*)(code_phys + HHDM_OFFSET);
    const uint8_t* src = g_user_test_vmalloc;
    for (uint32_t i = 0; i < bin_size; i++) dst[i] = src[i];
    // Zero any padding in the last page.
    for (uint32_t i = bin_size; i < (uint32_t)(pages * PAGE_SIZE); i++) dst[i] = 0;

    // Create and schedule the user process.
    task_t* ut = task_create_user(code_phys, pages, pid_alloc());
    if (!ut) {
        fs_puts(0, "[TEST] FAIL: task_create_user returned NULL", ATTR(RED, WHITE));
        for (;;) sched_yield();
    }
    // Store the pid so we can detect completion without touching freed memory.
    uint32_t ut_pid = ut->pid;
    sched_add(ut);

    // Wait until the task is no longer g_current and no longer in the queue.
    // We detect completion by watching g_current: once the user task has run
    // and exited, the scheduler will destroy it and never schedule it again.
    // We use a tick counter as a timeout (100 ticks ≈ 1 second at 100Hz).
    extern volatile uint32_t g_irq_count;
    uint32_t start = g_irq_count;
    // Yield repeatedly; the user task will print its own results via sys_write.
    // We just wait a generous amount of time for it to complete.
    while (g_irq_count - start < 500)
        sched_yield();

    (void)ut_pid;
    fs_puts(0, "[TEST] done waiting — see rows above for pass/fail", ATTR(BLACK, LGREEN));
    for (;;) sched_yield();
}

/* ── Embedded hello binary launcher ─────────────────────────────────────── */
// Launched via the shell "hello" command.
// Uses the same raw-binary + task_create_user path as the vmalloc test —
// completely bypasses ext2 and ELF loading so we can isolate issues.
void hello_embedded_fn(void) {
    uint32_t bin_size = g_user_hello_size;
    uint32_t pages    = (bin_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint8_t order = 0;
    while ((1u << order) < pages) order++;
    phys_addr_t code_phys = pmm_buddy_alloc(order);
    if (code_phys == PMM_INVALID_ADDR) {
        for (;;) sched_yield();
    }

    uint8_t* dst = (uint8_t*)(code_phys + HHDM_OFFSET);
    const uint8_t* src = g_user_hello;
    for (uint32_t i = 0; i < bin_size; i++) dst[i] = src[i];
    for (uint32_t i = bin_size; i < pages * PAGE_SIZE; i++) dst[i] = 0;

    task_t* t = task_create_user(code_phys, pages, pid_alloc());
    if (!t) { for (;;) sched_yield(); }

    uint32_t child_pid = t->pid;
    sched_add(t);
    sched_wait_pid(child_pid);
}

/* ── FAT32/ATA self-test (disabled — FAT32 uses ATA which is excluded) ─── */
void fs_init_fn(void) {
    for (;;) sched_yield();
}
