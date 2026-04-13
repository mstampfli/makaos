#pragma once
#include "common.h"

// ── PS/2 Keyboard driver ──────────────────────────────────────────────────

// Register real IRQ1 handler (replaces stub) and spawn driver kthread.
// Call from init_kthread after tasks are running.
void keyboard_init(void);
// Flush KBC hardware buffer and reset all software scancode/modifier state.
// Call after keyboard_init() before expecting clean input.
void keyboard_flush(void);

// Called from IRQ1 ASM stub — do not call directly.
void keyboard_irq_handler(void);

// LEGACY: Direct character polling — kept only so vfs.c /dev/kbd compiles.
//   keyboard_getchar() always returns 0.
//   keyboard_wait() sleeps forever (never returns).
//   NEW PATH: all input flows PS/2 IRQ → keyboard thread → input_emit()
//             → input_core handlers (tty_on_kbd_event, evdev_on_event).
//             Userland reads via /dev/tty0 (tty_open), /dev/input/event0
//             (evdev_open), or /dev/kbdraw (evdev_getraw).
char keyboard_getchar(void);
char keyboard_wait(void);
