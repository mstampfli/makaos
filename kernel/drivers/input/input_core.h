#pragma once
#include "common.h"

// ── Input core — decoupled keyboard publish/subscribe bus ─────────────────
//
// Architecture:
//
//   PS/2 hardware
//       │
//   keyboard driver  (reads port, decodes scancodes, emits kbd_event_t)
//       │
//       ▼
//   input_core  ← input_register_handler() / input_unregister_handler()
//       │
//       ├─ tty ldisc handler  (ascii → N_TTY → read_buf, signals)
//       └─ evdev handler      (keycode → input_event ring per open-fd)
//                                   │
//                             /dev/input/event0   (userland: doom, etc.)
//
// Drivers call input_emit() with a fully-decoded kbd_event_t.
// Handlers call input_register_handler() once at init.
// The core knows nothing about TTYs, evdev, or any consumer.

// ── Modifier bitmask ──────────────────────────────────────────────────────
#define MOD_SHIFT   (1u << 0)
#define MOD_CTRL    (1u << 1)
#define MOD_ALT     (1u << 2)
#define MOD_ALTGR   (1u << 3)

// ── Keyboard event — fully decoded by the driver before emission ──────────
// Handlers receive this and pick whichever fields they need.
// No handler needs to re-parse raw scancodes.
typedef struct {
    uint8_t  scancode;    // raw PS/2 set-1 byte (for pass-through / logging)
    uint16_t keycode;     // Linux KEY_* code (evdev, keymaps)
    uint8_t  pressed;     // 1 = key down, 0 = key up
    uint8_t  modifiers;   // MOD_* bitmask at time of event
    uint8_t  ascii;       // layout-translated ASCII char, 0 if none
} kbd_event_t;

// ── Linux KEY codes (subset we care about) ────────────────────────────────
// PS/2 set-1 base scancodes map 1:1 to Linux keycodes for the AT-derived
// range — not a coincidence; Linux keycodes were derived from PS/2 set-1.
#define KEY_ESC        1
#define KEY_1          2
#define KEY_2          3
#define KEY_3          4
#define KEY_4          5
#define KEY_5          6
#define KEY_6          7
#define KEY_7          8
#define KEY_8          9
#define KEY_9          10
#define KEY_0          11
#define KEY_MINUS      12
#define KEY_EQUAL      13
#define KEY_BACKSPACE  14
#define KEY_TAB        15
#define KEY_Q          16
#define KEY_W          17
#define KEY_E          18
#define KEY_R          19
#define KEY_T          20
#define KEY_Y          21
#define KEY_U          22
#define KEY_I          23
#define KEY_O          24
#define KEY_P          25
#define KEY_LEFTBRACE  26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER      28
#define KEY_LEFTCTRL   29
#define KEY_A          30
#define KEY_S          31
#define KEY_D          32
#define KEY_F          33
#define KEY_G          34
#define KEY_H          35
#define KEY_J          36
#define KEY_K          37
#define KEY_L          38
#define KEY_SEMICOLON  39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE      41
#define KEY_LEFTSHIFT  42
#define KEY_BACKSLASH  43
#define KEY_Z          44
#define KEY_X          45
#define KEY_C          46
#define KEY_V          47
#define KEY_B          48
#define KEY_N          49
#define KEY_M          50
#define KEY_COMMA      51
#define KEY_DOT        52
#define KEY_SLASH      53
#define KEY_RIGHTSHIFT 54
#define KEY_KPASTERISK 55
#define KEY_LEFTALT    56
#define KEY_SPACE      57
#define KEY_CAPSLOCK   58
#define KEY_F1         59
#define KEY_F2         60
#define KEY_F3         61
#define KEY_F4         62
#define KEY_F5         63
#define KEY_F6         64
#define KEY_F7         65
#define KEY_F8         66
#define KEY_F9         67
#define KEY_F10        68
#define KEY_NUMLOCK    69
#define KEY_SCROLLLOCK 70
#define KEY_KP7        71
#define KEY_KP8        72
#define KEY_KP9        73
#define KEY_KPMINUS    74
#define KEY_KP4        75
#define KEY_KP5        76
#define KEY_KP6        77
#define KEY_KPPLUS     78
#define KEY_KP1        79
#define KEY_KP2        80
#define KEY_KP3        81
#define KEY_KP0        82
#define KEY_KPDOT      83
#define KEY_102ND      86   // ISO extra key (left of Z on European boards)
#define KEY_F11        87
#define KEY_F12        88
#define KEY_RIGHTCTRL  97
#define KEY_RIGHTALT   100
#define KEY_HOME       102
#define KEY_UP         103
#define KEY_PAGEUP     104
#define KEY_LEFT       105
#define KEY_RIGHT      106
#define KEY_END        107
#define KEY_DOWN       108
#define KEY_PAGEDOWN   109
#define KEY_INSERT     110
#define KEY_DELETE     111

// ── Handler ───────────────────────────────────────────────────────────────
// Statically allocated by the subsystem that wants keyboard events.
// Register once at init; the core keeps a pointer (no heap allocation).

// Handler flag bits.
//
// INPUT_HANDLER_CONSOLE marks the kernel console TTY's handler.  When a
// userland grabber is active (e.g. a GUI compositor opens /dev/input/event0
// to consume raw events), console handlers are skipped so VINTR/VSUSP/ECHO
// on tty0 doesn't fire from keystrokes the compositor owns.  Mirrors the
// Linux KD_GRAPHICS model where the console stops reacting to keyboard
// input while X/Wayland holds the device.
#define INPUT_HANDLER_CONSOLE  (1u << 0)

typedef struct input_handler_t {
    const char* name;                            // for debugging
    uint32_t    flags;                           // INPUT_HANDLER_*
    void (*event)(const kbd_event_t*, void*);    // called per event
    void* data;                                  // passed back to event()
    struct input_handler_t* next;                // internal list linkage
} input_handler_t;

// ── API ───────────────────────────────────────────────────────────────────

// Register/unregister a handler.  Call at subsystem init / teardown.
// Not interrupt-safe; call from process context only.
void input_register_handler(input_handler_t* h);
void input_unregister_handler(input_handler_t* h);

// Called by the keyboard driver (from the keyboard thread, process context).
// Iterates all registered handlers synchronously, honouring the grab
// counter (console handlers skipped while grabbed).
void input_emit(const kbd_event_t* ev);

// ── Keyboard grab ────────────────────────────────────────────────────────
// Bump the grab refcount when a userland process takes exclusive ownership
// of keyboard input (currently: any open fd on /dev/input/event0).  Drop it
// on close.  While non-zero, console-flagged handlers do not see events.
// Safe against concurrent open/close via __atomic_* on a uint32_t.
void input_kbd_grab(void);
void input_kbd_ungrab(void);
