#pragma once

// ── Initcall system ───────────────────────────────────────────────────────
//
// Two execution levels, each with 8 priority slots:
//
//   INITCALL_EARLY(fn, priority)
//     Runs in kmain — no scheduler, no heap, interrupts off.
//     Use for: IDT, PMM, VMM, LAPIC, IOAPIC, TSS, syscall table.
//     Priority 0 runs first, 7 runs last within the level.
//
//   INITCALL_SUBSYS(fn, priority)
//     Runs in init_kthread — scheduler live, heap available, can sleep.
//     Use for: drivers (AHCI, HDA, net), TTY, ext2, keyboard, login spawn.
//     Priority 0 runs first, 7 runs last within the level.
//
// Usage:
//   static int my_driver_init(void) { ...; return 0; }
//   INITCALL_SUBSYS(my_driver_init, 3);
//
// Return value: 0 = success, non-zero = failure (logged, boot continues).
//
// Kernel modules use the exact same macros — they register into the same
// ELF sections and do_initcalls() picks them up automatically.

typedef int (*initcall_fn_t)(void);

// Place a pointer to `fn` into the correct ELF section.
// The __used__ attribute prevents the compiler from optimising away the
// pointer even if fn is static.  The section name encodes level + priority.
#define _INITCALL(fn, level, prio) \
    static initcall_fn_t __initcall_##fn##_##level##prio \
        __attribute__((__used__, __section__(".initcall_" #level #prio))) = (fn)

// Trailing semicolon is included — use without one: INITCALL_EARLY(fn, 3)
#define INITCALL_EARLY(fn, prio)  _INITCALL(fn, early,  prio)
#define INITCALL_SUBSYS(fn, prio) _INITCALL(fn, subsys, prio)

// ── Convenience aliases matching Linux naming convention ─────────────────
// pure hardware / arch init
#define early_initcall(fn)     INITCALL_EARLY(fn,  0)
// memory / paging
#define mm_initcall(fn)        INITCALL_EARLY(fn,  1)
// interrupt / timer infrastructure
#define irq_initcall(fn)       INITCALL_EARLY(fn,  2)
// bus / PCI scan
#define bus_initcall(fn)       INITCALL_EARLY(fn,  3)
// storage drivers
#define fs_initcall(fn)        INITCALL_SUBSYS(fn, 0)
// tty / input
#define tty_initcall(fn)       INITCALL_SUBSYS(fn, 1)
// network
#define net_initcall(fn)       INITCALL_SUBSYS(fn, 2)
// sound
#define sound_initcall(fn)     INITCALL_SUBSYS(fn, 3)
// everything else
#define device_initcall(fn)    INITCALL_SUBSYS(fn, 4)
// late / userspace-facing setup
#define late_initcall(fn)      INITCALL_SUBSYS(fn, 7)

// ── Runner — called once per level by main.c ─────────────────────────────
void do_initcalls_early(void);   // called from kmain
void do_initcalls_subsys(void);  // called from init_kthread
