#include "keyboard.h"
#include "idt.h"
#include "pic.h"
#include "sched.h"
#include "process.h"

#define KB_DATA_PORT 0x60

// ── Scancode Set 1 → ASCII lookup table ───────────────────────────────────
static const char s_scancode_ascii[88] = {
//  0     1     2     3     4     5     6     7     8     9     A     B     C     D     E     F
    0,    0,   '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t', // 0x00
   'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  'o',  'p',  '[',  ']',  '\n',  0,   'a',  's',  // 0x10
   'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  '\'',  '`',  0,   '\\', 'z',  'x',  'c',  'v',  // 0x20
   'b',  'n',  'm',  ',',  '.',  '/',   0,   '*',   0,   ' ',   0,    0,    0,    0,    0,    0,    // 0x30
    0,    0,    0,    0,    0,    0,    0,   '7',  '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  // 0x40
   '2',  '3',  '0',  '.',                                                                           // 0x50-0x53
};

// ── Extended scancodes ────────────────────────────────────────────────────
static char extended_key(uint8_t sc) {
    switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        default:   return 0;
    }
}

// ── Ring buffer ───────────────────────────────────────────────────────────
#define KBUF_SIZE 64
static char      s_buf[KBUF_SIZE];
static uint32_t  s_head = 0;
static uint32_t  s_tail = 0;
static uint8_t   s_extended = 0;

// Single waiter: the one process sleeping on keyboard_wait().
static task_t* s_waiter = NULL;

static void buf_push(char c) {
    uint32_t next = (s_tail + 1) % KBUF_SIZE;
    if (next == s_head) return;  // full — drop
    s_buf[s_tail] = c;
    s_tail = next;
    // Wake whichever process is blocked in keyboard_wait().
    if (s_waiter) {
        sched_wake(s_waiter);
        s_waiter = NULL;
    }
}

// ── Public API ────────────────────────────────────────────────────────────

char keyboard_getchar(void) {
    if (s_head == s_tail) return 0;
    char c = s_buf[s_head];
    s_head = (s_head + 1) % KBUF_SIZE;
    return c;
}

char keyboard_wait(void) {
    char c;
    while (!(c = keyboard_getchar())) {
        s_waiter = g_current;
        sched_sleep();
        s_waiter = NULL;
    }
    return c;
}

// ── IRQ1 handler ─────────────────────────────────────────────────────────
extern void irq1_entry(void);

void keyboard_irq_handler(void) {
    uint8_t sc = inb(KB_DATA_PORT);

    if (sc == 0xE0) { s_extended = 1; return; }

    if (s_extended) {
        s_extended = 0;
        if (!(sc & 0x80)) {
            char c = extended_key(sc);
            if (c) buf_push(c);
        }
        return;
    }

    if (sc & 0x80) return;

    if (sc < 88) {
        char c = s_scancode_ascii[sc];
        if (c) buf_push(c);
    }
}

void keyboard_init(void) {
    idt_irq_register(0x21, (uint64_t)irq1_entry);
    pic_unmask(1);
}
