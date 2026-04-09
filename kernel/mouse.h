#pragma once
#include "common.h"

// ── PS/2 Mouse driver ─────────────────────────────────────────────────────
//
// Exposes raw mouse motion and button events via a ring buffer.
// Userspace reads from /dev/mouse using the mouse_event_t struct below.
//
// Coordinate system: dx positive = right, dy positive = down (screen space).
// The PS/2 protocol reports dy positive = up; we invert it here so callers
// get a consistent screen-space coordinate system.
//
// Button mask bits in mouse_event_t.buttons:
//   MOUSE_BTN_LEFT   (bit 0)
//   MOUSE_BTN_RIGHT  (bit 1)
//   MOUSE_BTN_MIDDLE (bit 2)

#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

// One motion/button event.  Packed so userspace can cast a read buffer
// directly to an array of these.
typedef struct {
    int16_t  dx;       // signed horizontal delta (positive = right)
    int16_t  dy;       // signed vertical delta   (positive = down)
    uint8_t  buttons;  // button bitmask (MOUSE_BTN_*)
} __attribute__((packed)) mouse_event_t;

// Initialise the PS/2 mouse: enable aux port on the KBC, set stream mode,
// enable data reporting, register IRQ12, spawn the mouse driver thread.
void mouse_init(void);

// Called from IRQ12 ASM stub — do not call directly.
void mouse_irq_handler(void);

// Non-blocking: drain up to `max` events into `buf`.
// Returns the number of events written (0 if buffer is empty).
int mouse_read(mouse_event_t* buf, int max);
