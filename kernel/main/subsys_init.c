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

// input: keyboard + mouse init + full flush, all under preempt_disable so
// the spawned threads can't be scheduled before the flush completes.
// Declared with INITCALL_FLAG_PREEMPT_OFF — the DAG runner calls
// preempt_disable()/preempt_enable() around the fn automatically.
static int _input_init(void) {
    keyboard_init();    // install real IDT handler, spawn kbd thread
    mouse_init();       // install real IDT handler, spawn mouse thread
    keyboard_flush();   // drain KBC hw buf + s_sc_fifo + modifier state
    tty_flush_input(&g_ttys[0]);  // discard any phantom bytes in tty
    fb_clear();         // wipe UEFI boot artifacts
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
