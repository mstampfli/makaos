#pragma once
#include "common.h"

// ── PS/2 Keyboard driver ──────────────────────────────────────────────────

// Special key sentinels (non-printable, safe values below 0x05)
#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x03
#define KEY_RIGHT 0x04

// Register IRQ1 in the IDT and unmask it on the PIC.
void keyboard_init(void);

// Called from IRQ1 ASM stub — do not call directly.
void keyboard_irq_handler(void);

// Non-blocking: returns next character from the buffer, or 0 if empty.
char keyboard_getchar(void);

// Blocking: sleeps until a character is available, then returns it.
// This is what apps should use — hides all IRQ/sleep details.
char keyboard_wait(void);
