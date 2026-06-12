// ── Multi-device evdev subsystem ─────────────────────────────────────────
//
// Each physical input device (keyboard, mouse, touchpad, …) is an
// input_device_t registered by its driver at init time.  Every device
// has its own /dev/input/event<N> node.  Userland libinput enumerates
// the nodes, probes capabilities via the EVIOCG* ioctls, then reads
// struct input_event streams from each fd.
//
// No multiplexed all-in-one node.  No MakaOS-specific wire format.
// Upstream libinput / libevdev / xf86-input-evdev work unmodified.

#include "evdev.h"
#include "input_core.h"
#include "kheap.h"
#include "sched.h"
#include "process.h"
#include "signal.h"
#include "tsc.h"
#include "common.h"
#include "rcu.h"
#include "cpu.h"
#include "errno.h"

// ── Ring sizing ──────────────────────────────────────────────────────────
// 64 events × 24 bytes = 1.5 KiB per open fd.  Plenty for a keyboard's
// max burst; mouse/touchpad allocate the same per client.
#define EVDEV_RING_SIZE  64

// ── evdev_client_t — per-open-fd event stream ────────────────────────────
typedef struct evdev_client {
    input_device_t*    dev;
    input_event_t      ring[EVDEV_RING_SIZE];
    uint32_t           head, tail;
    struct task_t*     reader;
    struct vfs_file_t* file;
    uint8_t            clock_id;      // 0 = REALTIME, 1 = MONOTONIC (default)
    uint8_t            grabbed;       // this fd currently holds EVIOCGRAB
    struct evdev_client* next;
} evdev_client_t;

// ── Device registry ──────────────────────────────────────────────────────
static input_device_t* s_devices;
static uint8_t         s_next_event_nr;

// ── Bitmap helpers ───────────────────────────────────────────────────────
static inline void bit_set64(uint64_t* b, uint32_t n) { b[n>>6] |= 1ULL << (n & 63); }
static inline int  bit_get64(const uint64_t* b, uint32_t n) { return (b[n>>6] >> (n & 63)) & 1ULL; }

// ── Ring buffer helpers ──────────────────────────────────────────────────
static inline int  ring_empty(evdev_client_t* c) { return c->head == c->tail; }
static inline int  ring_full (evdev_client_t* c) {
    return ((c->tail + 1) & (EVDEV_RING_SIZE - 1)) == c->head;
}
static inline void ring_push(evdev_client_t* c, const input_event_t* ev) {
    if (ring_full(c)) return;   // drop: reader isn't draining
    c->ring[c->tail] = *ev;
    c->tail = (c->tail + 1) & (EVDEV_RING_SIZE - 1);
}
static inline int ring_pop(evdev_client_t* c, input_event_t* out) {
    if (ring_empty(c)) return 0;
    *out = c->ring[c->head];
    c->head = (c->head + 1) & (EVDEV_RING_SIZE - 1);
    return 1;
}
static inline uint32_t ring_size_bytes(evdev_client_t* c) {
    uint32_t used = (c->tail - c->head) & (EVDEV_RING_SIZE - 1);
    return used * (uint32_t)sizeof(input_event_t);
}

// ── Raw scancode buffer for /dev/kbdraw (legacy, doom et al.) ────────────
// Unchanged from pre-refactor — lives parallel to evdev.
#define RAW_BUF_SIZE 256
static volatile uint8_t s_raw_buf[RAW_BUF_SIZE];
static volatile uint8_t s_raw_head = 0;
static volatile uint8_t s_raw_tail = 0;

static void raw_push(uint8_t sc) {
    uint8_t next = (s_raw_tail + 1) & (RAW_BUF_SIZE - 1);
    if (next == s_raw_head) return;
    s_raw_buf[s_raw_tail] = sc;
    s_raw_tail = next;
}

int evdev_getraw(uint8_t* buf, int max) {
    int n = 0;
    while (n < max && s_raw_head != s_raw_tail) {
        buf[n++] = s_raw_buf[s_raw_head];
        s_raw_head = (s_raw_head + 1) & (RAW_BUF_SIZE - 1);
    }
    return n;
}

// ── Device registration / capability setters ─────────────────────────────
input_device_t* input_device_register(const char* name,
                                       uint16_t bustype, uint16_t vendor,
                                       uint16_t product, uint16_t version) {
    input_device_t* d = kmalloc(sizeof(*d));
    if (!d) return NULL;
    __builtin_memset(d, 0, sizeof(*d));

    d->event_nr   = s_next_event_nr++;
    int i = 0;
    for (; i < 79 && name && name[i]; i++) d->name[i] = name[i];
    d->name[i] = '\0';
    d->id.bustype = bustype;
    d->id.vendor  = vendor;
    d->id.product = product;
    d->id.version = version;

    // EV_SYN is always implied — every device emits SYN_REPORT between frames.
    d->ev_bits |= (1ULL << EV_SYN);

    d->next   = s_devices;
    s_devices = d;
    extern void kprintf(const char*, ...);
    kprintf("[evdev] registered event%u: %s (bus=%x vendor=%04x product=%04x)\n",
            (unsigned)d->event_nr, d->name, d->id.bustype, d->id.vendor, d->id.product);
    return d;
}

void input_device_set_ev_bit(input_device_t* d, uint16_t type) {
    if (type < EV_CNT) d->ev_bits |= (1ULL << type);
}

void input_device_set_key_bit(input_device_t* d, uint16_t code) {
    if (code < KEY_CNT) { bit_set64(d->key_bits, code); d->ev_bits |= (1ULL << EV_KEY); }
}

void input_device_set_rel_bit(input_device_t* d, uint16_t code) {
    if (code < REL_CNT) { bit_set64(d->rel_bits, code); d->ev_bits |= (1ULL << EV_REL); }
}

void input_device_set_abs_bit(input_device_t* d, uint16_t code,
                               const input_absinfo_t* info) {
    if (code < ABS_CNT) {
        bit_set64(d->abs_bits, code);
        d->ev_bits |= (1ULL << EV_ABS);
        if (info) d->abs[code] = *info;
    }
}

// ── Monotonic clock helper: TSC → (sec, usec) ────────────────────────────
static void fill_timestamp(uint8_t clock_id, int64_t* out_sec, int64_t* out_usec) {
    uint64_t ns = tsc_read_ns();
    (void)clock_id;  // TODO: REALTIME offset; for now use MONOTONIC for both.
    *out_sec  = (int64_t)(ns / 1000000000ull);
    *out_usec = (int64_t)((ns % 1000000000ull) / 1000ull);
}

// ── input_device_emit — the single publish primitive ─────────────────────
void input_device_emit(input_device_t* d,
                        uint16_t type, uint16_t code, int32_t value) {
    if (!d) return;

    // Update cached key state so EVIOCGKEY reflects current presses.
    if (type == EV_KEY && code < KEY_CNT) {
        if (value) bit_set64(d->key_state, code);
        else       d->key_state[code >> 6] &= ~(1ULL << (code & 63));
    }

    input_event_t ev = { .type = type, .code = code, .value = value };

    // Fan out to every non-grabbed client, OR only to the grabber.
    evdev_client_t* grabber = NULL;
    if (d->grabbed) {
        for (evdev_client_t* c = d->clients; c; c = c->next)
            if (c->grabbed) { grabber = c; break; }
    }

    if (grabber) {
        fill_timestamp(grabber->clock_id, &ev.sec, &ev.usec);
        ring_push(grabber, &ev);
        if (grabber->reader) { sched_wake(grabber->reader); grabber->reader = NULL; }
        if (grabber->file)   wait_queue_wake_all(grabber->file->waitq);
    } else {
        for (evdev_client_t* c = d->clients; c; c = c->next) {
            fill_timestamp(c->clock_id, &ev.sec, &ev.usec);
            ring_push(c, &ev);
            if (c->reader) { sched_wake(c->reader); c->reader = NULL; }
            if (c->file)   wait_queue_wake_all(c->file->waitq);
        }
    }
}

// ── Keyboard bridge from input_core ──────────────────────────────────────
// The PS/2 keyboard driver currently emits kbd_event_t via input_emit().
// We register the keyboard as an input_device_t at init and translate
// every kbd_event_t into EV_KEY + EV_SYN on that device.  The bridge
// goes away once keyboard.c is migrated to call input_device_emit
// directly — kept now so the console TTY path keeps receiving events
// via input_core while evdev gets the same events in libinput format.

static input_device_t* s_keyboard_dev;

static void evdev_on_kbd_event(const kbd_event_t* kbd, void* data) {
    (void)data;
    raw_push(kbd->scancode);
    if (!s_keyboard_dev) return;
    input_device_emit(s_keyboard_dev, EV_KEY, kbd->keycode, kbd->pressed ? 1 : 0);
    input_device_emit(s_keyboard_dev, EV_SYN, SYN_REPORT, 0);
}

// ── Mouse bridge ─────────────────────────────────────────────────────────
// PS/2 mouse driver hands us one decoded packet at a time (dx, dy, button
// mask).  We emit EV_REL deltas unconditionally and EV_KEY transitions
// for each button whose state changed since the last packet, terminated
// by a single SYN_REPORT per packet so libinput sees atomic updates.
//
// Previous-state tracking: packet cadence is ~100 Hz, all on one CPU; no
// lock needed.  If we ever get multi-packet SMP queueing we'd promote
// this to an atomic.

static input_device_t* s_mouse_dev;
static uint8_t         s_mouse_prev_btn;

void evdev_on_mouse_packet(int32_t dx, int32_t dy, uint8_t buttons) {
    if (!s_mouse_dev) return;

    if (dx) input_device_emit(s_mouse_dev, EV_REL, REL_X, dx);
    if (dy) input_device_emit(s_mouse_dev, EV_REL, REL_Y, dy);

    uint8_t changed = buttons ^ s_mouse_prev_btn;
    if (changed & 0x1) input_device_emit(s_mouse_dev, EV_KEY, BTN_LEFT,   (buttons & 0x1) ? 1 : 0);
    if (changed & 0x2) input_device_emit(s_mouse_dev, EV_KEY, BTN_MIDDLE, (buttons & 0x2) ? 1 : 0);
    if (changed & 0x4) input_device_emit(s_mouse_dev, EV_KEY, BTN_RIGHT,  (buttons & 0x4) ? 1 : 0);
    s_mouse_prev_btn = buttons;

    input_device_emit(s_mouse_dev, EV_SYN, SYN_REPORT, 0);
}

// ── VFS operations ───────────────────────────────────────────────────────

static int64_t evdev_vfs_read(vfs_file_t* self, void* buf, uint64_t len) {
    evdev_client_t* c = (evdev_client_t*)self->ctx;
    if (!c) return -EBADF;
    if (len < sizeof(input_event_t)) return -EINVAL;

    int nonblock = (self->flags & 0x800 /*O_NONBLOCK*/) != 0;
    while (ring_empty(c)) {
        if (nonblock) return -EAGAIN;
        c->reader = g_current;
        sched_sleep();
        if (signal_has_actionable(&g_current->sigstate))
            return -EINTR;
    }

    input_event_t* out = (input_event_t*)buf;
    uint64_t written = 0;
    uint64_t max_events = len / sizeof(input_event_t);
    while (written < max_events && ring_pop(c, &out[written])) written++;
    return (int64_t)(written * sizeof(input_event_t));
}

static int evdev_vfs_poll(vfs_file_t* self, int events) {
    evdev_client_t* c = (evdev_client_t*)self->ctx;
    if (!c) return 0;
    if (events & 1 /*POLLIN*/) return !ring_empty(c);
    return 0;
}

static void evdev_vfs_close(vfs_file_t* self) {
    evdev_client_t* c = (evdev_client_t*)self->ctx;
    if (c) {
        // Un-mute the console when the compositor releases the keyboard.
        if (c->dev == s_keyboard_dev) input_kbd_ungrab();
        // Drop grab if we hold it.
        if (c->grabbed && c->dev) {
            if (c->dev->grabbed > 0) c->dev->grabbed--;
            c->grabbed = 0;
        }
        if (c->dev) {
            evdev_client_t** pp = &c->dev->clients;
            while (*pp) {
                if (*pp == c) { *pp = c->next; break; }
                pp = &(*pp)->next;
            }
        }
        kfree(c);
    }
    kfree(self);
}

// ── ioctl — the full EVIOCG* / EVIOCS* surface libinput needs ────────────
// Linux _IOC encoding:
//   dir  = (req >> 30) & 0x3      (R=2, W=1, RW=3)
//   size = (req >> 16) & 0x3fff
//   type = (req >> 8)  & 0xff     ('E' = 0x45 for evdev)
//   nr   = req & 0xff
// Variable-size ioctls (names, bitmaps) match on nr-range and extract
// size from the request; fixed-size ioctls match on the full req.

#define IOC_TYPE(req)  (((req) >> 8)  & 0xffu)
#define IOC_NR(req)    ((req) & 0xffu)
#define IOC_SIZE(req)  (((req) >> 16) & 0x3fffu)

// NR range layout (Linux evdev.h):
#define EVIOCGVERSION_NR   0x01
#define EVIOCGID_NR        0x02
#define EVIOCGREP_NR       0x03
#define EVIOCSREP_NR       0x03  // same NR, different dir
#define EVIOCGKEYCODE_NR   0x04
#define EVIOCSKEYCODE_NR   0x04
#define EVIOCGNAME_NR      0x06
#define EVIOCGPHYS_NR      0x07
#define EVIOCGUNIQ_NR      0x08
#define EVIOCGPROP_NR      0x09
#define EVIOCGKEY_NR       0x18
#define EVIOCGLED_NR       0x19
#define EVIOCGSND_NR       0x1a
#define EVIOCGSW_NR        0x1b
#define EVIOCGBIT_BASE     0x20  // NR = 0x20 + evtype
#define EVIOCGBIT_MAX      0x3f
#define EVIOCGABS_BASE     0x40  // NR = 0x40 + axis
#define EVIOCGABS_MAX      0x7f
#define EVIOCSABS_BASE     0xc0
#define EVIOCSFF_NR        0x80
#define EVIOCRMFF_NR       0x81
#define EVIOCGEFFECTS_NR   0x84
#define EVIOCGRAB_NR       0x90
#define EVIOCREVOKE_NR     0x91
#define EVIOCSCLOCKID_NR   0xa0
#define EVIOCSMASK_NR      0x93
#define EVIOCGMTSLOTS_NR   0x0a

// Standard POSIX FIONREAD = _IOR('T', 0x1b, int).  Our userland stubs
// use this value; we don't parse it through the _IOC macros.
#define FIONREAD_REQ       0x541b

static int copy_to_user_n(void* dst, const void* src, uint32_t n) {
    // copy_to_user is declared elsewhere; kernel linkage.
    extern int copy_to_user(void* dst, const void* src, uint64_t n);
    return copy_to_user(dst, src, n);
}

static int copy_from_user_n(void* dst, const void* src, uint32_t n) {
    extern int copy_from_user(void* dst, const void* src, uint64_t n);
    return copy_from_user(dst, src, n);
}

static int64_t evdev_vfs_ioctl(vfs_file_t* self, uint64_t req, uint64_t arg) {
    evdev_client_t* c = (evdev_client_t*)self->ctx;
    if (!c || !c->dev) return -EBADF;
    input_device_t* d = c->dev;

    // FIONREAD: bytes available.
    if ((uint32_t)req == FIONREAD_REQ) {
        int n = (int)ring_size_bytes(c);
        if (copy_to_user_n((void*)arg, &n, sizeof(n)) != 0) return -EFAULT;
        return 0;
    }

    if (IOC_TYPE(req) != 'E') return -ENOTTY;
    uint32_t nr   = IOC_NR(req);
    uint32_t size = IOC_SIZE(req);

    switch (nr) {
    case EVIOCGVERSION_NR: {
        int32_t v = 0x010001;  // evdev ABI 1.0.1 (matches Linux)
        return copy_to_user_n((void*)arg, &v, sizeof(v)) == 0 ? 0 : -EFAULT;
    }
    case EVIOCGID_NR: {
        return copy_to_user_n((void*)arg, &d->id, sizeof(d->id)) == 0 ? 0 : -EFAULT;
    }
    case EVIOCGREP_NR: {
        // Keyboard auto-repeat: two u32s — delay_ms, period_ms.  libevdev
        // skips this ioctl on non-keyboard devices; when it's called it
        // expects success with valid numbers.  We don't actually repeat
        // in the kernel (userland/xkbcommon does it), but we still have
        // to return reasonable defaults.  Without this case, libevdev
        // treats the -EINVAL as "bogus device" and libinput drops the
        // keyboard — dwl then has no way to receive Alt+Shift+Q.
        //
        // Both GET (dir=R) and SET (dir=W) share the same nr=0x3; we
        // differentiate by the request direction bits.  GET returns
        // Linux's typical values (250 ms initial delay, 33 ms period).
        // SET is accepted and ignored — handing userland a no-op is
        // fine because our kernel isn't driving the repeat loop.
        uint32_t dir = (uint32_t)(req >> 30) & 0x3u;
        if (dir & 0x2u) {
            uint32_t rep[2] = { 250, 33 };
            return copy_to_user_n((void*)arg, rep, sizeof(rep)) == 0 ? 0 : -EFAULT;
        }
        /* dir & 1 => SET: accept + ignore */
        return 0;
    }
    case EVIOCGNAME_NR: {
        uint32_t n = 0; while (n < sizeof(d->name) && d->name[n]) n++;
        if (n + 1 < size) size = n + 1;  // include NUL
        if (copy_to_user_n((void*)arg, d->name, size) != 0) return -EFAULT;
        return (int64_t)size;
    }
    case EVIOCGPHYS_NR:
    case EVIOCGUNIQ_NR: {
        // Empty string for now — libinput tolerates it.
        uint8_t zero = 0;
        if (size && copy_to_user_n((void*)arg, &zero, 1) != 0) return -EFAULT;
        return 1;
    }
    case EVIOCGPROP_NR: {
        uint32_t bytes = sizeof(d->prop_bits);
        if (size < bytes) bytes = size;
        if (copy_to_user_n((void*)arg, d->prop_bits, bytes) != 0) return -EFAULT;
        return (int64_t)bytes;
    }
    case EVIOCGKEY_NR: {
        uint32_t bytes = sizeof(d->key_state);
        if (size < bytes) bytes = size;
        if (copy_to_user_n((void*)arg, d->key_state, bytes) != 0) return -EFAULT;
        return (int64_t)bytes;
    }
    case EVIOCGLED_NR:
    case EVIOCGSND_NR:
    case EVIOCGSW_NR: {
        uint8_t zero[16] = {0};
        uint32_t bytes = size < sizeof(zero) ? size : sizeof(zero);
        if (copy_to_user_n((void*)arg, zero, bytes) != 0) return -EFAULT;
        return (int64_t)bytes;
    }
    case EVIOCGRAB_NR: {
        // arg is int (0 = ungrab, nonzero = grab).  Our ioctl wrapper
        // receives arg as the raw syscall arg — not a pointer.
        int want = (int)arg;
        if (want) {
            if (d->grabbed && !c->grabbed) return -EBUSY;
            if (!c->grabbed) { d->grabbed++; c->grabbed = 1; }
        } else {
            if (c->grabbed) { d->grabbed--; c->grabbed = 0; }
        }
        return 0;
    }
    case EVIOCSCLOCKID_NR: {
        int id = 0;
        if (copy_from_user_n(&id, (void*)arg, sizeof(id)) != 0) return -EFAULT;
        c->clock_id = (id == CLOCK_REALTIME_ID) ? CLOCK_REALTIME_ID : CLOCK_MONOTONIC_ID;
        return 0;
    }
    }

    // EVIOCGBIT(type, len) — range 0x20..0x3f
    if (nr >= EVIOCGBIT_BASE && nr <= EVIOCGBIT_MAX) {
        uint32_t type = nr - EVIOCGBIT_BASE;
        const void* src = NULL;
        uint32_t src_size = 0;
        if (type == 0) { src = &d->ev_bits;  src_size = sizeof(d->ev_bits); }
        else if (type == EV_KEY) { src = d->key_bits; src_size = sizeof(d->key_bits); }
        else if (type == EV_REL) { src = d->rel_bits; src_size = sizeof(d->rel_bits); }
        else if (type == EV_ABS) { src = d->abs_bits; src_size = sizeof(d->abs_bits); }
        else if (type == EV_LED) { src = d->led_bits; src_size = sizeof(d->led_bits); }
        else if (type == EV_SW)  { src = d->sw_bits;  src_size = sizeof(d->sw_bits);  }
        else if (type == EV_MSC) { src = d->msc_bits; src_size = sizeof(d->msc_bits); }
        else return 0;  // empty bitmap
        uint32_t bytes = size < src_size ? size : src_size;
        if (bytes && copy_to_user_n((void*)arg, src, bytes) != 0) return -EFAULT;
        return (int64_t)bytes;
    }

    // EVIOCGABS(axis) — range 0x40..0x7f
    if (nr >= EVIOCGABS_BASE && nr <= EVIOCGABS_MAX) {
        uint32_t axis = nr - EVIOCGABS_BASE;
        if (axis >= ABS_CNT) return -EINVAL;
        if (copy_to_user_n((void*)arg, &d->abs[axis], sizeof(d->abs[axis])) != 0)
            return -EFAULT;
        return 0;
    }

    // EVIOCSABS(axis) — set absinfo, rarely used by libinput.  Accept.
    if (nr >= EVIOCSABS_BASE && nr < EVIOCSABS_BASE + ABS_CNT) {
        input_absinfo_t info;
        if (copy_from_user_n(&info, (void*)arg, sizeof(info)) != 0) return -EFAULT;
        d->abs[nr - EVIOCSABS_BASE] = info;
        return 0;
    }

    // EVIOCGMTSLOTS / EVIOCGEFFECTS / SFF / RMFF / GKEYCODE / revoke /
    // mask — return sensible defaults or -EINVAL.  libinput tolerates
    // absence of everything except the ones we implemented above.
    return -EINVAL;
}

// ── Open /dev/input/event<N> ─────────────────────────────────────────────
vfs_file_t* evdev_open_device(uint32_t event_nr) {
    input_device_t* d = NULL;
    for (input_device_t* it = s_devices; it; it = it->next)
        if (it->event_nr == event_nr) { d = it; break; }
    if (!d) return NULL;

    evdev_client_t* c = kmalloc(sizeof(*c));
    if (!c) return NULL;
    __builtin_memset(c, 0, sizeof(*c));
    c->dev      = d;
    c->clock_id = CLOCK_MONOTONIC_ID;

    vfs_file_t* f = kmalloc(sizeof(*f));
    if (!f) { kfree(c); return NULL; }
    __builtin_memset(f, 0, sizeof(*f));
    f->read   = evdev_vfs_read;
    f->write  = NULL;
    f->close  = evdev_vfs_close;
    f->seek   = NULL;
    f->poll   = evdev_vfs_poll;
    f->ioctl  = evdev_vfs_ioctl;
    f->ctx    = c;
    f->waitq  = &f->_waitq;
    wait_queue_init(f->waitq);
    f->secondary_waitq = NULL;
    f->flags    = 0;
    f->refcount = 1;
    f->rights   = 0;
    f->path[0]  = '\0';
    // Linux input device major = 13, minor = 64 + event_nr.  Matches
    // what our libudev advertises via s_devices[] and what libinput's
    // fstat-then-udev_device_new_from_devnum cross-check expects.
    f->rdev    = (13u << 8) | ((64u + event_nr) & 0xff);
    c->file = f;

    // Push into device's client list.
    c->next    = d->clients;
    d->clients = c;

    // A userland reader of the keyboard means a compositor has taken
    // over input.  Mute the kernel console TTY for the lifetime of this
    // fd (refcounted) so keystrokes go to the compositor, not the shell
    // behind it.  (No VT switching on MakaOS — this is the equivalent.)
    if (d == s_keyboard_dev) input_kbd_grab();

    return f;
}

vfs_file_t* evdev_open(void) { return evdev_open_device(0); }

// ── init: register keyboard device + the input_core bridge ───────────────
static input_handler_t s_evdev_handler = {
    .name  = "evdev",
    .event = evdev_on_kbd_event,
    .data  = NULL,
    .flags = 0,
    .next  = NULL,
};

void evdev_init(void) {
    // Keyboard device — event0, bus_i8042, generic vendor.
    s_keyboard_dev = input_device_register("MakaOS PS/2 Keyboard",
                                            BUS_I8042, 0x0001, 0x0001, 0x0100);
    if (s_keyboard_dev) {
        input_device_set_ev_bit(s_keyboard_dev, EV_KEY);
        input_device_set_ev_bit(s_keyboard_dev, EV_REP);
        // Declare every standard keyboard key as capable.  libinput
        // uses this bitmap to classify the device (needs KEY_A..KEY_Z,
        // KEY_1..KEY_0 etc. to tag it as "keyboard").
        for (uint16_t k = 1; k < 0x100; k++) input_device_set_key_bit(s_keyboard_dev, k);
    }

    // Mouse device — event1.  Declares REL_X/REL_Y + BTN_LEFT/RIGHT/MIDDLE
    // so libinput recognises it as a standard mouse.
    s_mouse_dev = input_device_register("MakaOS PS/2 Mouse",
                                         BUS_I8042, 0x0002, 0x0001, 0x0100);
    if (s_mouse_dev) {
        input_device_set_rel_bit(s_mouse_dev, REL_X);
        input_device_set_rel_bit(s_mouse_dev, REL_Y);
        input_device_set_key_bit(s_mouse_dev, BTN_LEFT);
        input_device_set_key_bit(s_mouse_dev, BTN_RIGHT);
        input_device_set_key_bit(s_mouse_dev, BTN_MIDDLE);
    }

    input_register_handler(&s_evdev_handler);
}
