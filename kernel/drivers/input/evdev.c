#include "evdev.h"
#include "input_core.h"
#include "kheap.h"
#include "sched.h"
#include "process.h"
#include "tsc.h"
#include "common.h"

// ── Per-client event ring buffer ──────────────────────────────────────────
// Each open("/dev/input/event0") allocates one of these.
// Events are written by the keyboard thread (via evdev_on_event),
// read by the client process (via evdev_vfs_read).
//
// Power-of-2 size so the modulo compiles to AND.
// 64 events = 64 * 16 = 1 KiB per open fd — generous for a keyboard.

#define EVDEV_RING_SIZE  64   // must be power of 2

typedef struct evdev_client_t {
    input_event_t    ring[EVDEV_RING_SIZE];
    uint32_t         head;           // reader advances this
    uint32_t         tail;           // writer advances this
    struct task_t*   reader;         // task sleeping in read(), or NULL
    struct vfs_file_t* file;         // back-pointer for poll wakeups
    struct evdev_client_t* next;     // intrusive list — all open clients
} evdev_client_t;

// ── Global client list ────────────────────────────────────────────────────
// All open evdev fds.  The keyboard thread iterates this to fan out events.
// Modified only in process context (open/close), not in IRQ.
static evdev_client_t* s_clients = NULL;

// ── Ring buffer helpers ───────────────────────────────────────────────────
static inline int  ring_empty(evdev_client_t* c) { return c->head == c->tail; }
static inline int  ring_full(evdev_client_t* c) {
    return ((c->tail + 1) & (EVDEV_RING_SIZE - 1)) == c->head;
}
static inline void ring_push(evdev_client_t* c, const input_event_t* ev) {
    if (ring_full(c)) return;   // drop: client isn't draining fast enough
    c->ring[c->tail] = *ev;
    c->tail = (c->tail + 1) & (EVDEV_RING_SIZE - 1);
}
static inline int ring_pop(evdev_client_t* c, input_event_t* out) {
    if (ring_empty(c)) return 0;
    *out = c->ring[c->head];
    c->head = (c->head + 1) & (EVDEV_RING_SIZE - 1);
    return 1;
}

// ── Raw scancode buffer (for /dev/kbdraw compat) ──────────────────────────
// Stores the raw PS/2 bytes from kbd_event_t.scancode so doom and other
// apps that expect raw scancodes can still use /dev/kbdraw without change.
// This lives in evdev because evdev owns the raw-input-to-userland pipeline.

#define RAW_BUF_SIZE 256   // power of 2
static volatile uint8_t s_raw_buf[RAW_BUF_SIZE];
static volatile uint8_t s_raw_head = 0;
static volatile uint8_t s_raw_tail = 0;

static void raw_push(uint8_t sc) {
    uint8_t next = (s_raw_tail + 1) & (RAW_BUF_SIZE - 1);
    if (next == s_raw_head) return;   // full — drop
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

// ── evdev_on_event — input_core handler callback ──────────────────────────
// Called from keyboard thread (process context) for every kbd_event_t.
// Fans out to all open clients.  Must not sleep.

static void evdev_on_event(const kbd_event_t* kbd, void* data) {
    (void)data;

    // Always push raw scancode for /dev/kbdraw consumers.
    raw_push(kbd->scancode);

    if (!s_clients) return;

    // Build a Linux-compatible input_event.
    input_event_t ev = {
        .time_ns = tsc_read_ns(),
        .type    = EV_KEY,
        .code    = kbd->keycode,
        .value   = kbd->pressed ? 1 : 0,
    };

    // Fan out to every open /dev/input/event0 client.
    for (evdev_client_t* c = s_clients; c; c = c->next) {
        ring_push(c, &ev);
        // Also push EV_SYN so clients can detect event boundaries.
        input_event_t syn = { .time_ns = ev.time_ns, .type = EV_SYN, .code = 0, .value = 0 };
        ring_push(c, &syn);
        // Wake any sleeping reader (blocking read or poll).
        if (c->reader) {
            sched_wake(c->reader);
            c->reader = NULL;
        }
        if (c->file) wait_queue_wake_all(c->file->waitq);
    }
}

// ── VFS operations ────────────────────────────────────────────────────────

static int64_t evdev_vfs_read(vfs_file_t* self, void* buf, uint64_t len) {
    evdev_client_t* c = (evdev_client_t*)self->ctx;
    if (!c) return -1;

    // Must read in whole input_event_t units.
    if (len < sizeof(input_event_t)) return -1;

    // Block until at least one event is available.
    while (ring_empty(c)) {
        c->reader = g_current;
        sched_sleep();
        if (g_current->sigstate.head != g_current->sigstate.tail)
            return -4; // -EINTR
    }

    // Drain whole events up to len bytes.
    input_event_t* out = (input_event_t*)buf;
    uint64_t count = 0;
    uint64_t max_events = len / sizeof(input_event_t);
    while (count < max_events) {
        if (!ring_pop(c, &out[count])) break;
        count++;
    }
    return (int64_t)(count * sizeof(input_event_t));
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
        // Remove from global client list.
        evdev_client_t** pp = &s_clients;
        while (*pp) {
            if (*pp == c) { *pp = c->next; break; }
            pp = &(*pp)->next;
        }
        kfree(c);
    }
    kfree(self);
}

// ── evdev_open ────────────────────────────────────────────────────────────
vfs_file_t* evdev_open(void) {
    evdev_client_t* c = kmalloc(sizeof(evdev_client_t));
    if (!c) return NULL;
    c->head   = 0;
    c->tail   = 0;
    c->reader = NULL;
    c->file   = NULL;
    // Prepend to client list.
    c->next   = s_clients;
    s_clients = c;

    vfs_file_t* f = kmalloc(sizeof(vfs_file_t));
    if (!f) {
        s_clients = c->next;
        kfree(c);
        return NULL;
    }
    f->read     = evdev_vfs_read;
    f->write    = NULL;
    f->close    = evdev_vfs_close;
    f->seek     = NULL;
    f->poll           = evdev_vfs_poll;
    f->ioctl          = NULL;
    f->ctx            = c;
    f->waitq           = &f->_waitq; wait_queue_init(f->waitq);
    f->secondary_waitq = NULL;
    f->flags          = 0;
    f->refcount    = 1;
    f->rights      = 0;
    f->path[0]     = '\0';

    c->file = f;  // back-pointer for poll wakeups

    return f;
}

// ── evdev_init ────────────────────────────────────────────────────────────
// Register the evdev handler with input_core.

static input_handler_t s_evdev_handler = {
    .name  = "evdev",
    .event = evdev_on_event,
    .data  = NULL,
    .next  = NULL,
};

void evdev_init(void) {
    input_register_handler(&s_evdev_handler);
}
