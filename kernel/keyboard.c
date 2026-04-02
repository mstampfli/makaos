#include "keyboard.h"
#include "idt.h"
#include "pic.h"
#include "irq_wait.h"
#include "sched.h"
#include "process.h"
#include "kheap.h"

#define KB_DATA_PORT 0x60

// ── Swiss German (CH-DE) QWERTZ scancode tables ───────────────────────────
static const char s_unshifted[88] = {
//  0     1     2     3     4     5     6     7     8     9     A     B     C     D     E     F
    0,   '\x1b','1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0', '\'',  '^', '\b', '\t', // 0x00
   'q',  'w',  'e',  'r',  't',  'z',  'u',  'i',  'o',  'p',   0,    0,  '\n',   0,  'a',  's',  // 0x10
   'd',  'f',  'g',  'h',  'j',  'k',  'l',   0,    0,    0,    0,   '$',  'y',  'x',  'c',  'v',  // 0x20
   'b',  'n',  'm',  ',',  '.',  '-',   0,   '*',   0,   ' ',   0,    0,    0,    0,    0,    0,    // 0x30
    0,    0,    0,    0,    0,    0,    0,   '7',  '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  // 0x40
   '2',  '3',  '0',  '.',                                                                           // 0x50-0x53
};

static const char s_shifted[88] = {
//  0     1     2     3     4     5     6     7     8     9     A     B     C     D     E     F
    0,   '\x1b','!',  '"',  '*',  'c',  '%',  '&',  '/',  '(',  ')',  '=',  '?',  '`', '\b', '\t', // 0x00
   'Q',  'W',  'E',  'R',  'T',  'Z',  'U',  'I',  'O',  'P',   0,    0,  '\n',   0,  'A',  'S',  // 0x10
   'D',  'F',  'G',  'H',  'J',  'K',  'L',   0,    0,    0,    0,  '#',  'Y',  'X',  'C',  'V',  // 0x20
   'B',  'N',  'M',  ';',  ':',  '_',   0,   '*',   0,   ' ',   0,    0,    0,    0,    0,    0,   // 0x30
    0,    0,    0,    0,    0,    0,    0,   '7',  '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  // 0x40
   '2',  '3',  '0',  '.',                                                                           // 0x50-0x53
};

// ── Modifier state ────────────────────────────────────────────────────────
static uint8_t s_shift = 0;
static uint8_t s_ctrl  = 0;

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

// Single waiter: the one process sleeping on keyboard_wait().
static task_t* s_waiter = NULL;

static void buf_push(char c) {
    uint32_t next = (s_tail + 1) % KBUF_SIZE;
    if (next == s_head) return;  // full — drop
    s_buf[s_tail] = c;
    s_tail = next;
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
    for (;;) {
        __asm__ volatile("cli");
        char c = keyboard_getchar();
        if (c) {
            __asm__ volatile("sti");
            return c;
        }
        s_waiter = g_current;
        sched_sleep();  /* do_switch re-enables interrupts */
        s_waiter = NULL;
    }
}

// ── Raw scancode FIFO (ISR → thread) ─────────────────────────────────────
// ISR pushes one byte per IRQ; thread pops and processes.
// Power-of-2 size so the modulo compiles to AND.
// If full, the byte is dropped (PS/2 hardware also has a 16-byte buffer,
// so this only happens under extreme load).
#define SC_FIFO_SIZE 16
static volatile uint8_t s_sc_fifo[SC_FIFO_SIZE];
static volatile uint8_t s_sc_head = 0;  // thread reads here
static volatile uint8_t s_sc_tail = 0;  // ISR writes here

static void sc_push(uint8_t sc) {
    uint8_t next = (s_sc_tail + 1) & (SC_FIFO_SIZE - 1);
    if (next == s_sc_head) return;  // full — drop
    s_sc_fifo[s_sc_tail] = sc;
    s_sc_tail = next;
}

static uint8_t sc_pop(void) {
    uint8_t sc = s_sc_fifo[s_sc_head];
    s_sc_head = (s_sc_head + 1) & (SC_FIFO_SIZE - 1);
    return sc;
}

// ── IRQ1 handler — runs in interrupt context, minimal work ───────────────
extern void irq1_entry(void);

void keyboard_irq_handler(void) {
    sc_push(inb(KB_DATA_PORT));
    irq_notify(1);
}

// ── Keyboard thread — runs in process context, does all processing ────────
static uint8_t s_extended = 0;

static void keyboard_thread_fn(void) {
    for (;;) {
        irq_wait(1);
        while (s_sc_head != s_sc_tail) {
            uint8_t sc = sc_pop();

            if (sc == 0xE0) { s_extended = 1; continue; }

            if (s_extended) {
                s_extended = 0;
                if (sc == 0x1D) { s_ctrl = 1; continue; }
                if (sc == 0x9D) { s_ctrl = 0; continue; }
                if (!(sc & 0x80)) {
                    char c = extended_key(sc);
                    if (c) buf_push(c);
                }
                continue;
            }

            if (sc == 0x2A || sc == 0x36) { s_shift = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { s_shift = 0; continue; }
            if (sc == 0x1D) { s_ctrl = 1; continue; }
            if (sc == 0x9D) { s_ctrl = 0; continue; }
            if (sc & 0x80) continue;

            if (sc < 88) {
                char c = s_shift ? s_shifted[sc] : s_unshifted[sc];
                if (c) {
                    if (s_ctrl && c >= 'a' && c <= 'z')
                        buf_push((char)(c - 'a' + 1));
                    else if (s_ctrl && c >= 'A' && c <= 'Z')
                        buf_push((char)(c - 'A' + 1));
                    else
                        buf_push(c);
                }
            }
        }
    }
}

void keyboard_init(void) {
    idt_irq_register(0x21, (uint64_t)irq1_entry);
    pic_unmask(1);

    task_t* t = task_create_kthread(keyboard_thread_fn, pid_alloc());
    if (t) sched_add(t);
}
