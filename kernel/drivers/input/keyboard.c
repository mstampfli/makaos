#include "keyboard.h"
#include "input_core.h"
#include "idt.h"
#include "ioapic.h"
#include "irq_wait.h"
#include "sched.h"
#include "process.h"

#define KB_DATA_PORT 0x60

// ── Swiss German (CH-DE) QWERTZ keymap ───────────────────────────────────
// Indexed by PS/2 set-1 base scancode (0x00–0x57).
// Each entry: { unshifted, shifted } ASCII char.  0 = no ASCII output.
// Non-ASCII chars (ü,ö,ä,ç,§,°) → 0 (no glyph in our framebuffer font).

typedef struct { char u; char s; } key_ascii_t;

static const key_ascii_t s_keymap[128] = {
//       unshifted  shifted
/* 00 */ { 0,       0      },
/* 01 */ { '\x1b',  '\x1b' },   // ESC
/* 02 */ { '1',     '+'    },
/* 03 */ { '2',     '"'    },
/* 04 */ { '3',     '*'    },
/* 05 */ { '4',     0      },   // 4 / ç (non-ASCII)
/* 06 */ { '5',     '%'    },
/* 07 */ { '6',     '&'    },
/* 08 */ { '7',     '/'    },
/* 09 */ { '8',     '('    },
/* 0A */ { '9',     ')'    },
/* 0B */ { '0',     '='    },
/* 0C */ { '\'',    '?'    },
/* 0D */ { '^',     '`'    },   // dead key — deliver as-is
/* 0E */ { '\b',    '\b'   },   // backspace
/* 0F */ { '\t',    '\t'   },   // tab
/* 10 */ { 'q',     'Q'    },
/* 11 */ { 'w',     'W'    },
/* 12 */ { 'e',     'E'    },
/* 13 */ { 'r',     'R'    },
/* 14 */ { 't',     'T'    },
/* 15 */ { 'z',     'Z'    },   // CH-DE: z on QWERTZ row
/* 16 */ { 'u',     'U'    },
/* 17 */ { 'i',     'I'    },
/* 18 */ { 'o',     'O'    },
/* 19 */ { 'p',     'P'    },
/* 1A */ { 0,       0      },   // ü / Ü
/* 1B */ { '!',     0      },   // ¨! / ¨ (non-ASCII shift)
/* 1C */ { '\n',    '\n'   },   // enter
/* 1D */ { 0,       0      },   // left ctrl — handled as modifier
/* 1E */ { 'a',     'A'    },
/* 1F */ { 's',     'S'    },
/* 20 */ { 'd',     'D'    },
/* 21 */ { 'f',     'F'    },
/* 22 */ { 'g',     'G'    },
/* 23 */ { 'h',     'H'    },
/* 24 */ { 'j',     'J'    },
/* 25 */ { 'k',     'K'    },
/* 26 */ { 'l',     'L'    },
/* 27 */ { 0,       0      },   // ö / Ö
/* 28 */ { 0,       0      },   // ä / Ä
/* 29 */ { 0,       0      },   // § / °
/* 2A */ { 0,       0      },   // left shift — modifier
/* 2B */ { '$',     '#'    },
/* 2C */ { 'y',     'Y'    },   // CH-DE: y on bottom row
/* 2D */ { 'x',     'X'    },
/* 2E */ { 'c',     'C'    },
/* 2F */ { 'v',     'V'    },
/* 30 */ { 'b',     'B'    },
/* 31 */ { 'n',     'N'    },
/* 32 */ { 'm',     'M'    },
/* 33 */ { ',',     ';'    },
/* 34 */ { '.',     ':'    },
/* 35 */ { '-',     '_'    },
/* 36 */ { 0,       0      },   // right shift — modifier
/* 37 */ { '*',     '*'    },
/* 38 */ { 0,       0      },   // left alt — modifier
/* 39 */ { ' ',     ' '    },   // space
/* 3A */ { 0,       0      },   // caps lock (not implemented)
/* 3B */ { 0,       0      },   // F1
/* 3C */ { 0,       0      },   // F2
/* 3D */ { 0,       0      },   // F3
/* 3E */ { 0,       0      },   // F4
/* 3F */ { 0,       0      },   // F5
/* 40 */ { 0,       0      },   // F6
/* 41 */ { 0,       0      },   // F7
/* 42 */ { 0,       0      },   // F8
/* 43 */ { 0,       0      },   // F9
/* 44 */ { 0,       0      },   // F10
/* 45 */ { 0,       0      },   // num lock
/* 46 */ { 0,       0      },   // scroll lock
/* 47 */ { '7',     0      },
/* 48 */ { '8',     0      },
/* 49 */ { '9',     0      },
/* 4A */ { '-',     0      },
/* 4B */ { '4',     0      },
/* 4C */ { '5',     0      },
/* 4D */ { '6',     0      },
/* 4E */ { '+',     0      },
/* 4F */ { '1',     0      },
/* 50 */ { '2',     0      },
/* 51 */ { '3',     0      },
/* 52 */ { '0',     0      },
/* 53 */ { '.',     0      },
/* 54 */ { 0,       0      },
/* 55 */ { 0,       0      },
/* 56 */ { '<',     '>'    },   // ISO extra key (left of Y)
/* 57 */ { 0,       0      },   // F11
};

// ── PS/2 set-1 base scancode → Linux KEY_* ───────────────────────────────
// For extended (E0-prefixed) scancodes, the base byte maps differently;
// those are handled in ext_to_keycode() below.
// Indices 0x00–0x58 cover the AT keyboard set (plus F12 at 0x58).
static const uint16_t s_sc_to_key[128] = {
    [0x01] = KEY_ESC,       [0x02] = KEY_1,         [0x03] = KEY_2,
    [0x04] = KEY_3,         [0x05] = KEY_4,         [0x06] = KEY_5,
    [0x07] = KEY_6,         [0x08] = KEY_7,         [0x09] = KEY_8,
    [0x0A] = KEY_9,         [0x0B] = KEY_0,         [0x0E] = KEY_BACKSPACE,
    [0x0F] = KEY_TAB,       [0x10] = KEY_Q,         [0x11] = KEY_W,
    [0x12] = KEY_E,         [0x13] = KEY_R,         [0x14] = KEY_T,
    [0x15] = KEY_Y,         [0x16] = KEY_U,         [0x17] = KEY_I,
    [0x18] = KEY_O,         [0x19] = KEY_P,         [0x1C] = KEY_ENTER,
    [0x1D] = KEY_LEFTCTRL,  [0x1E] = KEY_A,         [0x1F] = KEY_S,
    [0x20] = KEY_D,         [0x21] = KEY_F,         [0x22] = KEY_G,
    [0x23] = KEY_H,         [0x24] = KEY_J,         [0x25] = KEY_K,
    [0x26] = KEY_L,         [0x2A] = KEY_LEFTSHIFT,
    [0x2C] = KEY_Z,         [0x2D] = KEY_X,         [0x2E] = KEY_C,
    [0x2F] = KEY_V,         [0x30] = KEY_B,         [0x31] = KEY_N,
    [0x32] = KEY_M,         [0x33] = KEY_COMMA,     [0x34] = KEY_DOT,
    [0x35] = KEY_SLASH,     [0x36] = KEY_RIGHTSHIFT,[0x37] = KEY_KPASTERISK,
    [0x38] = KEY_LEFTALT,   [0x39] = KEY_SPACE,     [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1,        [0x3C] = KEY_F2,        [0x3D] = KEY_F3,
    [0x3E] = KEY_F4,        [0x3F] = KEY_F5,        [0x40] = KEY_F6,
    [0x41] = KEY_F7,        [0x42] = KEY_F8,        [0x43] = KEY_F9,
    [0x44] = KEY_F10,       [0x45] = KEY_NUMLOCK,   [0x46] = KEY_SCROLLLOCK,
    [0x47] = KEY_KP7,       [0x48] = KEY_KP8,       [0x49] = KEY_KP9,
    [0x4A] = KEY_KPMINUS,   [0x4B] = KEY_KP4,       [0x4C] = KEY_KP5,
    [0x4D] = KEY_KP6,       [0x4E] = KEY_KPPLUS,    [0x4F] = KEY_KP1,
    [0x50] = KEY_KP2,       [0x51] = KEY_KP3,       [0x52] = KEY_KP0,
    [0x53] = KEY_KPDOT,     [0x56] = KEY_102ND,     [0x57] = KEY_F11,
    [0x58] = KEY_F12,
};

static uint16_t ext_to_keycode(uint8_t sc) {
    switch (sc) {
        case 0x1D: return KEY_RIGHTCTRL;
        case 0x38: return KEY_RIGHTALT;
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UP;
        case 0x49: return KEY_PAGEUP;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x4F: return KEY_END;
        case 0x50: return KEY_DOWN;
        case 0x51: return KEY_PAGEDOWN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        // Super/logo + menu keys — wayland compositors bind Mod4 to
        // these (sway's default $mod).  Without the mapping the kernel
        // dropped E0-5B entirely and no Mod4 binding could ever fire.
        case 0x5B: return KEY_LEFTMETA;
        case 0x5C: return KEY_RIGHTMETA;
        case 0x5D: return KEY_COMPOSE;
        default:   return 0;
    }
}

// ── Modifier state ────────────────────────────────────────────────────────
static uint8_t s_shift  = 0;
static uint8_t s_ctrl   = 0;
static uint8_t s_altgr  = 0;  // right alt = AltGr on CH-DE

// ── IRQ→thread staging FIFO ───────────────────────────────────────────────
// Minimal 16-byte ring: ISR drops one byte here, keyboard thread drains it.
#define SC_FIFO_SIZE 16
static volatile uint8_t s_sc_fifo[SC_FIFO_SIZE];
static volatile uint8_t s_sc_head = 0;
static volatile uint8_t s_sc_tail = 0;

static void sc_push(uint8_t sc) {
    uint8_t next = (s_sc_tail + 1) & (SC_FIFO_SIZE - 1);
    if (next == s_sc_head) return;
    s_sc_fifo[s_sc_tail] = sc;
    s_sc_tail = next;
}

static uint8_t sc_pop(void) {
    uint8_t sc = s_sc_fifo[s_sc_head];
    s_sc_head = (s_sc_head + 1) & (SC_FIFO_SIZE - 1);
    return sc;
}

// ── IRQ1 handler — interrupt context, minimal work ────────────────────────
extern void irq1_entry(void);

void keyboard_irq_handler(void) {
    // Discard bytes that originated from the aux (mouse) port — the KBC
    // shares port 0x60 between keyboard and mouse; bit 5 of the status
    // register distinguishes them.  Without this check, mouse ACKs during
    // mouse_init would be stolen from the polling path, causing a hang.
    if (inb(0x64) & (1 << 5)) { inb(KB_DATA_PORT); return; }
    uint8_t sc = inb(KB_DATA_PORT);
    sc_push(sc);
    irq_notify(1);
}

// ── Keyboard thread — process context, full decoding + emit ──────────────
static uint8_t s_extended = 0;

static void keyboard_thread_fn(void) {
    for (;;) {
        irq_wait(1);

        while (s_sc_head != s_sc_tail) {
            uint8_t sc = sc_pop();

            // ── E0 prefix — next byte is an extended scancode ─────────────
            if (sc == 0xE0) { s_extended = 1; continue; }

            // ── Decode scancode → (keycode, pressed, modifiers, ascii) ─────
            uint8_t  pressed  = !(sc & 0x80);
            uint8_t  base     = sc & 0x7F;
            uint16_t keycode;

            if (s_extended) {
                s_extended = 0;
                keycode = ext_to_keycode(base);
            } else {
                keycode = (base < 128) ? s_sc_to_key[base] : 0;
            }

            // ── Update modifier state ─────────────────────────────────────
            switch (keycode) {
                case KEY_LEFTSHIFT:  case KEY_RIGHTSHIFT: s_shift = pressed; break;
                case KEY_LEFTCTRL:   case KEY_RIGHTCTRL:  s_ctrl  = pressed; break;
                case KEY_RIGHTALT:                         s_altgr = pressed; break;
                default: break;
            }

            // ── Build modifier bitmask ────────────────────────────────────
            uint8_t mods = 0;
            if (s_shift) mods |= MOD_SHIFT;
            if (s_ctrl)  mods |= MOD_CTRL;
            if (s_altgr) mods |= MOD_ALTGR;

            // ── Translate to ASCII ────────────────────────────────────────
            // Extended keys: no direct ASCII (handlers inject ANSI sequences).
            // Base keys: use keymap, apply ctrl folding.
            uint8_t ascii = 0;
            if (!s_extended && base < 128 && pressed) {
                char c = s_shift ? s_keymap[base].s : s_keymap[base].u;
                if (c) {
                    if (s_ctrl && c >= 'a' && c <= 'z')
                        ascii = (uint8_t)(c - 'a' + 1);   // ^A–^Z
                    else if (s_ctrl && c >= 'A' && c <= 'Z')
                        ascii = (uint8_t)(c - 'A' + 1);
                    else
                        ascii = (uint8_t)c;
                }
            }

            // ── Emit to input_core ────────────────────────────────────────
            kbd_event_t ev = {
                .scancode  = sc,
                .keycode   = keycode,
                .pressed   = pressed,
                .modifiers = mods,
                .ascii     = ascii,
            };
            input_emit(&ev);
        }
    }
}

// ── LEGACY stubs ─────────────────────────────────────────────────────────
// keyboard_getchar() and keyboard_wait() exist only to satisfy the linker
// for vfs.c's /dev/kbd device (vfs_kbd_open).  They do NOT deliver input.
//   keyboard_getchar() → always returns 0 (no character)
//   keyboard_wait()    → sleeps forever (never returns)
// NEW PATH: input flows keyboard_thread_fn → input_emit() → input_core
//   handlers.  Userland gets input via:
//     /dev/tty0          (tty_open)      — canonical/raw TTY with line discipline
//     /dev/input/event0  (evdev_open)    — structured input_event_t per-fd streams
//     /dev/kbdraw        (vfs_kbdraw)    — raw PS/2 scancodes via evdev_getraw()
char keyboard_getchar(void) { return 0; }
char keyboard_wait(void)    { for (;;) sched_sleep(); }

void keyboard_flush(void) {
    // Drain KBC hardware buffer and reset all software state.
    // Call after keyboard_init() but before any user input is expected.
    while (inb(0x64) & 0x01) inb(0x60);   // drain KBC hardware FIFO
    s_sc_head = s_sc_tail = 0;             // discard any queued scancodes
    s_extended = 0;                        // reset extended prefix state
    s_shift = s_ctrl = s_altgr = 0;       // reset all modifier state
    irq_drain(1);                          // clear any pending phantom IRQ1 counts
}

void keyboard_init(void) {
    // Install handler + spawn thread but do NOT unmask IRQ1 yet.
    // IRQ1 shares the KBC data register with the mouse — if IRQ1 fires
    // while mouse_init is polling for ACK bytes, keyboard_irq_handler
    // consumes those bytes even with the AUX discard, starving mouse_read_byte.
    // _input_init calls keyboard_flush() + ioapic_unmask(IRQ1) after mouse_init.
    idt_irq_register(0x21, (uint64_t)irq1_entry);
    task_t* t = task_create_kthread(keyboard_thread_fn, pid_alloc());
    if (t) sched_add(t);
}
