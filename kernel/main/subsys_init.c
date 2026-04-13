#include "initcall.h"
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

// ── INITCALL_EARLY ────────────────────────────────────────────────────────
// Runs in kmain — no sleeping allowed.
static int _early_tty(void)   { tty_init();        return 0; }
static int _early_evdev(void) { evdev_init();       return 0; }
static int _early_ahci(void)  { ahci_init();        return 0; }
static int _early_ext2(void)  { ext2_init(4096);    return 0; }

INITCALL_EARLY(_early_tty,   1);
INITCALL_EARLY(_early_evdev, 2);
INITCALL_EARLY(_early_ahci,  3);
INITCALL_EARLY(_early_ext2,  4);

// ── INITCALL_SUBSYS ───────────────────────────────────────────────────────
// Runs in init_kthread — process context, can sleep.
//
// Priority order:
//   0 — keyboard + mouse: spawn threads first so they go straight to
//       irq_wait() and sleep before any real IRQ arrives
//   1 — drain KBC hardware buffer + flush tty + clear fb.
//       By the time this runs the kbd/mouse threads are already sleeping
//       in irq_wait — no race between drain and thread startup.
//   2 — audio + network (may sleep on hardware)

static int _subsys_input(void) {
    // cli: prevent timer preemption so keyboard/mouse threads can't be
    // scheduled between init and flush — no phantom can sneak through.
    __asm__ volatile("cli");
    keyboard_init();
    mouse_init();
    keyboard_flush();
    tty_flush_input(&g_ttys[0]);
    fb_clear();
    __asm__ volatile("sti");
    return 0;
}
INITCALL_SUBSYS(_subsys_input, 0);

static int _subsys_hda(void) { hda_init(); return 0; }
static int _subsys_net(void) { net_init(); return 0; }

INITCALL_SUBSYS(_subsys_hda, 1);
INITCALL_SUBSYS(_subsys_net, 1);
