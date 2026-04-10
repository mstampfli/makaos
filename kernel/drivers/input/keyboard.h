#pragma once
#include "common.h"

// ── PS/2 Keyboard driver ──────────────────────────────────────────────────

// Register IRQ1 in the IDT and unmask it on the PIC.
void keyboard_init(void);

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
