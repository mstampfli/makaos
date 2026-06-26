#pragma once
#include "common.h"
#include "input_core.h"
#include "vfs.h"

// ── evdev — Linux-compatible per-device input subsystem ──────────────────
//
// One input_device_t per physical device (keyboard, mouse, touchpad, …).
// Each device gets its own /dev/input/event<N> node.  Userland libinput
// is the consumer: it enumerates these nodes, issues EVIOCG* ioctls to
// probe capabilities, then reads struct input_event from each fd.
//
//   PS/2 / USB / virtio-input driver
//          │
//    input_device_emit(dev, type, code, value)      ← publishes event
//          │
//     fan-out to all evdev_client_t subscribers of `dev`
//          │
//     /dev/input/eventN  →  userspace libinput
//
// Wire format: Linux `struct input_event` — byte-exact compat so
// libinput, libevdev, xf86-input-evdev, etc. work unmodified.

// ── input_event wire struct ───────────────────────────────────────────────
// Linux x86_64 layout: `struct timeval` (16B: s64 tv_sec + s64 tv_usec) +
// u16 type + u16 code + s32 value = 24 B.  We emit exactly that.
typedef struct {
    int64_t  sec;        // CLOCK_MONOTONIC seconds (or REALTIME per EVIOCSCLOCKID)
    int64_t  usec;       // microseconds
    uint16_t type;       // EV_*
    uint16_t code;       // KEY_* / REL_* / ABS_* / SYN_*
    int32_t  value;
} input_event_t;

// ── Event types (Linux linux/input-event-codes.h, subset we expose) ──────
#define EV_SYN        0x00
#define EV_KEY        0x01
#define EV_REL        0x02
#define EV_ABS        0x03
#define EV_MSC        0x04
#define EV_SW         0x05
#define EV_LED        0x11
#define EV_REP        0x14
#define EV_MAX        0x1f
#define EV_CNT        (EV_MAX + 1)

// SYN codes
#define SYN_REPORT    0
#define SYN_DROPPED   3

// Key/button codes share one 16-bit space.  KEY_MAX matches Linux.
#define KEY_MAX       0x2ff
#define KEY_CNT       (KEY_MAX + 1)

// Rel axes (mouse)
#define REL_X         0
#define REL_Y         1
#define REL_HWHEEL    6
#define REL_WHEEL     8
#define REL_MAX       0x0f
#define REL_CNT       (REL_MAX + 1)

// Abs axes (touch / tablet)
#define ABS_X               0
#define ABS_Y               1
#define ABS_PRESSURE        0x18
#define ABS_MT_SLOT         0x2f
#define ABS_MT_POSITION_X   0x35
#define ABS_MT_POSITION_Y   0x36
#define ABS_MT_TRACKING_ID  0x39
#define ABS_MAX             0x3f
#define ABS_CNT             (ABS_MAX + 1)

// Mouse buttons
#define BTN_LEFT      0x110
#define BTN_RIGHT     0x111
#define BTN_MIDDLE    0x112
#define BTN_SIDE      0x113
#define BTN_EXTRA     0x114
#define BTN_TOUCH     0x14a
#define BTN_TOOL_FINGER  0x145
#define BTN_TOOL_PEN     0x140

// Bus types (EVIOCGID.bustype)
#define BUS_PCI       0x01
#define BUS_USB       0x03
#define BUS_I8042     0x11
#define BUS_VIRTUAL   0x06

// Input-prop bits (dense bitmap, INPUT_PROP_MAX from Linux)
#define INPUT_PROP_POINTER         0x00
#define INPUT_PROP_DIRECT          0x01
#define INPUT_PROP_BUTTONPAD       0x02
#define INPUT_PROP_MAX             0x1f
#define INPUT_PROP_CNT             (INPUT_PROP_MAX + 1)

// POSIX clock IDs used by EVIOCSCLOCKID.
#define CLOCK_REALTIME_ID   0
#define CLOCK_MONOTONIC_ID  1

// ── Per-axis absolute info (EVIOCGABS) ───────────────────────────────────
typedef struct {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
} input_absinfo_t;

// ── Device identity (EVIOCGID) ───────────────────────────────────────────
typedef struct {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
} input_id_t;

// ── input_device_t — one per physical device ─────────────────────────────
// Capabilities are a 2-level bitmap: ev_bits says which EV_* types are
// supported; key_bits / rel_bits / abs_bits / ... enumerate codes within
// types.  Matches Linux's struct input_dev bitmap layout so EVIOCGBIT
// returns the bytes libinput / libevdev expect.
#define BITMAP_WORDS(max)  (((max) + 63) / 64)

typedef struct input_device {
    uint8_t          event_nr;
    char             name[80];
    input_id_t       id;
    uint32_t         prop_bits[BITMAP_WORDS(INPUT_PROP_CNT)];
    uint64_t         ev_bits;
    uint64_t         key_bits[BITMAP_WORDS(KEY_CNT)];
    uint64_t         rel_bits[BITMAP_WORDS(REL_CNT)];
    uint64_t         abs_bits[BITMAP_WORDS(ABS_CNT)];
    uint64_t         led_bits[BITMAP_WORDS(0x10)];
    uint64_t         msc_bits[BITMAP_WORDS(0x08)];
    uint64_t         sw_bits [BITMAP_WORDS(0x10)];
    uint64_t         key_state[BITMAP_WORDS(KEY_CNT)];
    input_absinfo_t  abs[ABS_CNT];

    struct evdev_client* clients;
    int              grabbed;           // EVIOCGRAB holder count
    spinlock_t       lock;              // serialises the client list lifetime +
                                        // each client's ring head/tail.  IRQ-safe
                                        // (spin_lock_irqsave): the mouse producer
                                        // runs input_device_emit in IRQ context.

    struct input_device* next;
} input_device_t;

// ── Driver-facing API ─────────────────────────────────────────────────────
void evdev_init(void);

input_device_t* input_device_register(const char* name,
                                       uint16_t bustype, uint16_t vendor,
                                       uint16_t product, uint16_t version);

void input_device_set_ev_bit  (input_device_t* dev, uint16_t type);
void input_device_set_key_bit (input_device_t* dev, uint16_t code);
void input_device_set_rel_bit (input_device_t* dev, uint16_t code);
void input_device_set_abs_bit (input_device_t* dev, uint16_t code,
                                const input_absinfo_t* info);

void input_device_emit(input_device_t* dev,
                        uint16_t type, uint16_t code, int32_t value);

// ── Mouse bridge ─────────────────────────────────────────────────────────
// Called from the PS/2 mouse driver after it decodes one packet.  Emits
// the motion + button-state deltas as Linux-compatible evdev events on
// the registered mouse device.  `buttons` is the current button mask
// (bit 0 = left, 1 = middle, 2 = right) — the bridge tracks previous
// state internally and emits EV_KEY only on transitions.
void evdev_on_mouse_packet(int32_t dx, int32_t dy, uint8_t buttons);

// ── Legacy helpers kept for /dev/kbdraw (doom et al.) ────────────────────
int evdev_getraw(uint8_t* buf, int max);

// ── VFS open path for /dev/input/event<N> ────────────────────────────────
vfs_file_t* evdev_open_device(uint32_t event_nr);

// Back-compat alias — opens device 0 (keyboard).  New callers should use
// evdev_open_device.
vfs_file_t* evdev_open(void);
