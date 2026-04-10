#include "keyboard.h"
#include "idt.h"
#include "irq_wait.h"
#include "sched.h"
#include "process.h"
#include "tty.h"

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

// ── Grab state ───────────────────────────────────────────────────────────
// When grabbed, translated chars are suppressed; only raw bytes flow.
static volatile uint8_t s_grabbed = 0;

void keyboard_grab(void)   { s_grabbed = 1; }
void keyboard_ungrab(void) { s_grabbed = 0; }

// ── Modifier state ────────────────────────────────────────────────────────
static uint8_t s_shift = 0;
static uint8_t s_ctrl  = 0;

// ── Extended scancodes → ANSI escape sequences ────────────────────────────
// Inject as ESC [ x so the TTY passes them through to userland correctly.
// In ICANON mode these are buffered as regular chars (no special meaning).
// In raw mode userland can parse them as arrow keys.
static void extended_key_inject(uint8_t sc, tty_t* tty) {
    const char* seq = NULL;
    switch (sc) {
        case 0x48: seq = "\x1b[A"; break;  // up
        case 0x50: seq = "\x1b[B"; break;  // down
        case 0x4B: seq = "\x1b[D"; break;  // left
        case 0x4D: seq = "\x1b[C"; break;  // right
        case 0x47: seq = "\x1b[H"; break;  // home
        case 0x4F: seq = "\x1b[F"; break;  // end
        case 0x49: seq = "\x1b[5~"; break; // page up
        case 0x51: seq = "\x1b[6~"; break; // page down
        case 0x53: seq = "\x1b[3~"; break; // delete
        default:   return;
    }
    for (int i = 0; seq[i]; i++)
        tty_input_char(tty, seq[i]);
}

// ── Public API (legacy — backed by TTY; kept for /dev/kbd in vfs.c) ───────
// Translated chars now flow through the TTY line discipline; these stubs
// exist so vfs.c's kbd_read compiles.  Use /dev/tty instead.

char keyboard_getchar(void) { return 0; }

char keyboard_wait(void) {
    // Block via tty — just sleep until the tty has a char.
    for (;;) {
        char c = keyboard_getchar();
        if (c) return c;
        sched_sleep();
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

// ── Raw scancode ring buffer (for /dev/kbdraw) ───────────────────────────
// Stores raw bytes before any translation (including break codes / E0 prefix).
// Used by doom for key press/release events.
#define RAW_BUF_SIZE 256
static volatile uint8_t s_raw_buf[RAW_BUF_SIZE];
static volatile uint8_t s_raw_head = 0;
static volatile uint8_t s_raw_tail = 0;

static void raw_push(uint8_t sc) {
    uint8_t next = (s_raw_tail + 1) & (RAW_BUF_SIZE - 1);
    if (next == s_raw_head) return; // full — drop
    s_raw_buf[s_raw_tail] = sc;
    s_raw_tail = next;
}

// Non-blocking read of raw scancodes.  Returns bytes read (0 if empty).
int keyboard_getraw(uint8_t* buf, int max) {
    int n = 0;
    while (n < max && s_raw_head != s_raw_tail) {
        buf[n++] = s_raw_buf[s_raw_head];
        s_raw_head = (s_raw_head + 1) & (RAW_BUF_SIZE - 1);
    }
    return n;
}

// ── IRQ1 handler — runs in interrupt context, minimal work ───────────────
extern void irq1_entry(void);

void keyboard_irq_handler(void) {
    uint8_t sc = inb(KB_DATA_PORT);
    sc_push(sc);    // single FIFO — keyboard thread fans out from here
    irq_notify(1);
}

// ── Keyboard thread — runs in process context, does all processing ────────
static uint8_t s_extended = 0;

static void keyboard_thread_fn(void) {
    for (;;) {
        irq_wait(1);
        while (s_sc_head != s_sc_tail) {
            uint8_t sc = sc_pop();

            // ── Fan out to raw consumers (/dev/kbdraw) ────────────────────
            // Every byte before translation goes to the raw ring buffer.
            // This is the single source — ISR no longer fans out.
            raw_push(sc);

            if (sc == 0xE0) { s_extended = 1; continue; }

            if (s_extended) {
                s_extended = 0;
                if (sc == 0x1D) { s_ctrl = 1; continue; }
                if (sc == 0x9D) { s_ctrl = 0; continue; }
                if (!s_grabbed && !(sc & 0x80))
                    extended_key_inject(sc, &g_ttys[0]);
                continue;
            }

            if (sc == 0x2A || sc == 0x36) { s_shift = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { s_shift = 0; continue; }
            if (sc == 0x1D) { s_ctrl = 1; continue; }
            if (sc == 0x9D) { s_ctrl = 0; continue; }
            if (sc & 0x80) continue;

            if (!s_grabbed && sc < 88) {
                char c = s_shift ? s_shifted[sc] : s_unshifted[sc];
                if (c) {
                    char out;
                    if (s_ctrl && c >= 'a' && c <= 'z')
                        out = (char)(c - 'a' + 1);
                    else if (s_ctrl && c >= 'A' && c <= 'Z')
                        out = (char)(c - 'A' + 1);
                    else
                        out = c;
                    // Route through TTY line discipline.
                    tty_input_char(&g_ttys[0], out);
                }
            }
        }
    }
}

void keyboard_init(void) {
    // Vector 0x21 is already programmed into the IOAPIC by ioapic_init().
    // We just register the IDT entry; the IOAPIC does the unmasking.
    idt_irq_register(0x21, (uint64_t)irq1_entry);

    task_t* t = task_create_kthread(keyboard_thread_fn, pid_alloc());
    if (t) sched_add(t);
}
