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

// ── input_emit ────────────────────────────────────────────────────────────
// Called from the keyboard thread (process context, not IRQ).
// Delivers the event to every registered handler in registration order.
// Handlers must not sleep or block.

void input_emit(const kbd_event_t* ev) {
    for (input_handler_t* h = s_handlers; h; h = h->next)
        h->event(ev, h->data);
}
