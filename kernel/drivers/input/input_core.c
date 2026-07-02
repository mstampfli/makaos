#include "input_core.h"
#include "common.h"
#include "smp.h"
#include "preempt.h"
#include "cpu.h"
#include "irq_wait.h"

// ── i8042 controller lock ─────────────────────────────────────────────────
// One PS/2 controller, two IRQ lines (IRQ1 keyboard, IRQ12 mouse), shared
// data/status ports 0x60/0x64.  Under SMP the IOAPIC can deliver the two
// IRQs to different CPUs concurrently — and the same IRQ can re-fire on a
// second CPU while the first is mid-handler.  Every status-read + data-read
// pair, and every byte-stream accumulator state touched by the ISRs, must
// be serialized by this one lock or bytes get stolen/garbled across CPUs
// and stream indices race (the mouse packet index escaping its bound was
// observed scribbling PS/2 packet bytes over .bss: g_mouse_waitq et al).
// Same design as Linux's i8042_lock.  ISRs run with local IRQs off, so a
// plain spin_lock (no irqsave) is sufficient: the only contention is
// cross-CPU.
spinlock_t g_i8042_lock = SPINLOCK_INIT;

// ── Shared i8042 ISR drain ────────────────────────────────────────────────
// ONE consume path for BOTH IRQ1 (keyboard) and IRQ12 (mouse), Linux-style.
// Each interrupt drains EVERY pending byte from the controller, routing
// each by the status AUX bit: keyboard bytes → sc ring, mouse bytes → the
// 3-byte packet accumulator.  This kills two whole failure classes the
// previous split handlers had:
//   - byte THEFT: the keyboard ISR consuming (discarding) a mouse byte
//     desyncs the packet stream → cursor teleporting;
//   - byte STRANDING: the keyboard ISR leaving an AUX byte but its IRQ12
//     edge already lost → the byte blocks the single output buffer forever
//     and BOTH devices go dead (observed: keyboard+mouse silent mid-boot).
// With the drain, whichever IRQ fires consumes everything pending and no
// byte can ever block the queue.  The loop is bounded as a guard against
// a wedged OBF bit; real FIFOs hold a handful of bytes.
void i8042_isr_drain(void) {
    uint8_t  pkts[8][3];
    uint32_t npkt = 0;
    uint8_t  got_kbd = 0;

    spin_lock(&g_i8042_lock);
    for (int i = 0; i < 32; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) break;               // output buffer empty
        uint8_t b = inb(0x60);
        if (st & (1u << 5)) {                  // AUX = mouse byte
            if (npkt < 8 && mouse_isr_byte(b, pkts[npkt]))
                npkt++;
        } else {
            keyboard_isr_byte(b);
            got_kbd = 1;
        }
    }
    spin_unlock(&g_i8042_lock);

    // Decode + wake OUTSIDE the controller lock (same rationale as the
    // old mouse handler: the wakeups take ring/runqueue locks).  The
    // preempt guard prevents a wake-side rcu unlock from context-
    // switching mid-ISR; direct depth-- so the guard itself can't
    // re-trigger the switch (pattern from mouse.c).
    if (npkt) {
        preempt_disable();
        for (uint32_t i = 0; i < npkt; i++)
            mouse_isr_packet(pkts[i]);
        preempt_enable_no_resched();
        irq_notify(12);
    }
    if (got_kbd)
        irq_notify(1);
}

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
