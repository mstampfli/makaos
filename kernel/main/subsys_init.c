#include "initcall.h"
#include "preempt.h"
#include "common.h"
#include "tty.h"
#include "evdev.h"
#include "ahci.h"
#include "ext2.h"
#include "keyboard.h"
#include "mouse.h"
#include "hda.h"
#include "net/net.h"
#include "fb.h"
#include "ioapic.h"

// ── INITCALL_LEVEL_EARLY registrations ───────────────────────────────────
// No sleeping.  Strict dependency order declared explicitly.

static int _tty_init(void)   { tty_init();        return 0; }
static int _evdev_init(void) { evdev_init();       return 0; }
static int _ahci_init(void)  { ahci_init();        return 0; }
static int _ext2_init(void)  { ext2_init(4096);    return 0; }

// tty: no kernel deps at this level (tty_init only needs the heap)
DEFINE_INITCALL(tty, INITCALL_LEVEL_EARLY, .fn = _tty_init);

// evdev: registers with input_core which is part of tty subsystem
DEFINE_INITCALL(evdev, INITCALL_LEVEL_EARLY,
    .fn   = _evdev_init,
    .deps = INITCALL_DEPS("tty"),
);

// ahci: needs PCI (already done in kmain before do_initcalls_early)
DEFINE_INITCALL(ahci, INITCALL_LEVEL_EARLY, .fn = _ahci_init);

// ext2: needs ahci to be initialised first
DEFINE_INITCALL(ext2, INITCALL_LEVEL_EARLY,
    .fn   = _ext2_init,
    .deps = INITCALL_DEPS("ahci"),
);

// ── INITCALL_LEVEL_SUBSYS registrations ──────────────────────────────────
// Process context — can sleep.

// input: keyboard + mouse init + full flush under preempt_disable.
// INITCALL_FLAG_PREEMPT_OFF so the DAG runner wraps fn with
// preempt_disable/enable — kbd/mouse threads can't be scheduled between
// their spawn and the flush.
static int _input_init(void) {
    // Mask IRQ1 at the IOAPIC for the window between installing the real
    // keyboard handler and draining the KBC + FIFO.  This prevents mouse
    // hardware ACKs (0xFA) — which arrive via the shared KBC and fire IRQ1 —
    // from being pushed into the keyboard FIFO as phantom scancodes.
    // The real keyboard handler is in place but delivery is blocked at the
    // hardware level; keyboard_flush() drains whatever is in the KBC/FIFO,
    // then we unmask so real user keypresses flow normally.
    uint32_t irq1_gsi = ioapic_isa_to_gsi(1);
    ioapic_mask(irq1_gsi);

    keyboard_init();    // install real IRQ1 handler, spawn kbd thread
    mouse_init();       // send mouse hw cmds — ACKs arrive at KBC but IRQ1 masked
    keyboard_flush();   // drain KBC hw buf + s_sc_fifo + irq_pending
    tty_flush_input(&g_ttys[0]);  // discard any phantom bytes in tty
    fb_clear();         // wipe UEFI boot artifacts

    ioapic_unmask(irq1_gsi);  // real keypresses flow from here
    return 0;
}

DEFINE_INITCALL(input, INITCALL_LEVEL_SUBSYS,
    .fn    = _input_init,
    .flags = INITCALL_FLAG_PREEMPT_OFF,
    .deps  = INITCALL_DEPS("tty", "evdev"),
);

// hda: Intel HDA audio — may sleep waiting for codec response
static int _hda_init(void) { hda_init(); return 0; }

DEFINE_INITCALL(hda, INITCALL_LEVEL_SUBSYS, .fn = _hda_init);

// net: virtio-net + IP stack — may sleep waiting for link negotiation
static int _net_init(void) { net_init(); return 0; }

DEFINE_INITCALL(net, INITCALL_LEVEL_SUBSYS, .fn = _net_init);
