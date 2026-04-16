#include "input_core.h"
#include "common.h"

// ── Handler list ──────────────────────────────────────────────────────────
// Singly-linked list of registered handlers.  Modified only at init time
// (process context, not IRQ), so no locking needed on a single-CPU kernel.

static input_handler_t* s_handlers = NULL;

void input_register_handler(input_handler_t* h) {
    if (!h || !h->event) return;
    h->next    = s_handlers;
    s_handlers = h;
}

void input_unregister_handler(input_handler_t* h) {
    if (!h) return;
    input_handler_t** pp = &s_handlers;
    while (*pp) {
        if (*pp == h) { *pp = h->next; h->next = NULL; return; }
        pp = &(*pp)->next;
    }
}

// ── Keyboard grab refcount ────────────────────────────────────────────────
// Number of userland processes currently grabbing the keyboard.  Incremented
// on /dev/input/event0 open, decremented on close.  Read lock-free on the
// hot emit path; updated rarely from process context.
static volatile uint32_t s_kbd_grab_count;

void input_kbd_grab(void) {
    __atomic_add_fetch(&s_kbd_grab_count, 1, __ATOMIC_RELEASE);
}

void input_kbd_ungrab(void) {
    __atomic_sub_fetch(&s_kbd_grab_count, 1, __ATOMIC_RELEASE);
}

static inline int input_kbd_is_grabbed(void) {
    return __atomic_load_n(&s_kbd_grab_count, __ATOMIC_ACQUIRE) != 0;
}

// ── input_emit ────────────────────────────────────────────────────────────
// Called from the keyboard thread (process context, not IRQ).
// Delivers the event to every registered handler in registration order.
// Handlers must not sleep or block.
//
// Grabbing: while s_kbd_grab_count > 0, handlers flagged
// INPUT_HANDLER_CONSOLE are skipped — the console TTY stops reacting so
// a graphical session gets exclusive keyboard ownership.  Non-console
// handlers (evdev itself, raw-scancode fanout, etc.) always receive
// events regardless of grab state.

void input_emit(const kbd_event_t* ev) {
    int grabbed = input_kbd_is_grabbed();
    for (input_handler_t* h = s_handlers; h; h = h->next) {
        if (grabbed && (h->flags & INPUT_HANDLER_CONSOLE)) continue;
        h->event(ev, h->data);
    }
}
