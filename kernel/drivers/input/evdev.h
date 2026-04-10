#pragma once
#include "common.h"
#include "input_core.h"
#include "vfs.h"

// ── evdev — per-open-fd keyboard event streams ────────────────────────────
//
// Mirrors Linux's evdev in structure:
//
//   input_core calls evdev_handler.event() for every kbd_event_t.
//   Each open("/dev/input/event0") creates an independent evdev_client
//   with its own ring buffer.  Events flow to ALL open clients simultaneously.
//   Clients read input_event structs:
//
//     struct input_event {
//         uint64_t time_ns;   // monotonic timestamp (TSC-based)
//         uint16_t type;      // EV_KEY=1, EV_SYN=0
//         uint16_t code;      // KEY_* for EV_KEY
//         int32_t  value;     // 1=press, 0=release, 2=repeat
//     };
//
// Compatible with Linux's input_event layout (same field order, same codes).
// Userland can use standard evdev-aware libraries in the future.

// ── input_event ───────────────────────────────────────────────────────────
#define EV_SYN   0   // synchronisation marker
#define EV_KEY   1   // key press/release/repeat

typedef struct {
    uint64_t time_ns;   // monotonic ns timestamp
    uint16_t type;      // EV_*
    uint16_t code;      // KEY_* (for EV_KEY)
    int32_t  value;     // 1=press, 0=release, 2=autorepeat
} input_event_t;

// ── API ───────────────────────────────────────────────────────────────────

// Register the evdev handler with input_core.  Call once at boot.
void evdev_init(void);

// Open /dev/input/event0 — returns a vfs_file_t per caller.
// Each caller gets its own independent event stream.
vfs_file_t* evdev_open(void);

// Non-blocking read of raw PS/2 scancodes (for /dev/kbdraw compat).
// Returns number of bytes written to buf (0 if empty).
int evdev_getraw(uint8_t* buf, int max);
