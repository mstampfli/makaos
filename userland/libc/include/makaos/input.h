#ifndef _MAKAOS_INPUT_H
#define _MAKAOS_INPUT_H 1

// ── MakaOS native input API ──────────────────────────────────────────
//
// Userland interface to the kernel input subsystem.  Deliberately
// NOT Linux-compatible at the byte level — MakaOS events are 16 bytes
// (uint64_t time_ns + u16 type + u16 code + s32 value) instead of
// Linux's 24-byte struct with split tv_sec/tv_usec fields.
//
// The kernel exposes one event stream per input device at
// /dev/input/event0.  Open the file, then read fixed-size events.
// Each read() returns 0 or more complete events (never partial).
// Non-blocking reads honour O_NONBLOCK like any other fd; poll/epoll
// work out of the box.
//
// Key codes (KEY_*) are chosen to match Linux's evdev numbering so
// keymap files interchange cleanly, but the struct layout does not.

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

// ── Event type ───────────────────────────────────────────────────────
#define MAKA_EV_SYN    0   // synchronisation marker (end-of-packet)
#define MAKA_EV_KEY    1   // key or button transition
#define MAKA_EV_REL    2   // relative pointer axis (future: mouse)
#define MAKA_EV_ABS    3   // absolute axis (future: touch/tablet)

// ── Modifier bitmask (mirrors kernel/drivers/input/input_core.h) ─────
#define MAKA_MOD_SHIFT  (1u << 0)
#define MAKA_MOD_CTRL   (1u << 1)
#define MAKA_MOD_ALT    (1u << 2)
#define MAKA_MOD_ALTGR  (1u << 3)

// ── Event record — 16 bytes, packed ──────────────────────────────────
// Matches exactly what the kernel emits on /dev/input/event0.
typedef struct maka_input_event {
    uint64_t time_ns;   // monotonic nanosecond timestamp
    uint16_t type;      // MAKA_EV_*
    uint16_t code;      // KEY_* for KEY events; axis for REL/ABS
    int32_t  value;     // 1=press, 0=release, 2=autorepeat
} maka_input_event_t;

// ── Linux-compatible keycodes (kept in sync with input_core.h) ───────
// Numbering matches Linux evdev so xkbcommon keymaps work unchanged.
#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
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
#define KEY_102ND      86
#define KEY_F11        87
#define KEY_F12        88
#define KEY_RIGHTCTRL  97
#define KEY_RIGHTALT  100
#define KEY_HOME      102
#define KEY_UP        103
#define KEY_PAGEUP    104
#define KEY_LEFT      105
#define KEY_RIGHT     106
#define KEY_END       107
#define KEY_DOWN      108
#define KEY_PAGEDOWN  109
#define KEY_INSERT    110
#define KEY_DELETE    111
#define KEY_MAX       255   // inclusive upper bound for key table sizing

// ── Mouse buttons (reuse KEY-space at BTN_* offsets per evdev) ───────
#define BTN_MOUSE      0x110
#define BTN_LEFT       0x110
#define BTN_RIGHT      0x111
#define BTN_MIDDLE     0x112

// ── Relative pointer axes (MAKA_EV_REL) ──────────────────────────────
#define REL_X          0
#define REL_Y          1
#define REL_WHEEL      8

// ── Absolute axes (MAKA_EV_ABS — future touch/tablet) ────────────────
#define ABS_X          0
#define ABS_Y          1
#define ABS_PRESSURE   24
#define ABS_MT_SLOT         47
#define ABS_MT_POSITION_X   53
#define ABS_MT_POSITION_Y   54
#define ABS_MT_TRACKING_ID  57

// ── Public API ───────────────────────────────────────────────────────

// Enumerate input devices.  Writes up to `max` device paths into
// `paths` (caller-owned, NUL-terminated).  Returns the number
// written, or -1 on error.
int maka_input_enumerate(char (*paths)[64], int max);

// Open an input device by path.  `O_NONBLOCK` may be OR'd with flags
// for non-blocking reads.  Returns an fd or -1 on error.
int maka_input_open(const char* path, int flags);

// Read one or more complete events.  Caller guarantees `buf` holds
// at least `n` events.  Returns event count or -1 on error.
// Partial events at the tail of the kernel ring are never returned.
int maka_input_read(int fd, maka_input_event_t* buf, int n);

// Close an input fd.  Identical to close(fd), exported for symmetry.
int maka_input_close(int fd);

#endif
