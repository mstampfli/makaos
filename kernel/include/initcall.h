#pragma once
#include "common.h"

// ── Initcall DAG system ───────────────────────────────────────────────────
//
// Drivers register themselves with a name and explicit dependencies.
// At boot the init runner builds a dependency graph, topologically sorts it,
// and executes each initcall in the correct order.  Cycles cause a kernel
// panic with a clear diagnostic at boot, never a silent hang.
//
// Two execution levels:
//
//   INITCALL_LEVEL_EARLY  — runs in kmain, no scheduler, no heap, no sleep.
//                           Use for: IDT, PMM, VMM, LAPIC, IOAPIC, TSS.
//
//   INITCALL_LEVEL_SUBSYS — runs in init_kthread, scheduler live, can sleep.
//                           Use for: storage, TTY, input drivers, network.
//
// Usage:
//   static int my_driver_init(void) { ...; return 0; }
//
//   DEFINE_INITCALL(my_driver, INITCALL_LEVEL_SUBSYS,
//       .deps = INITCALL_DEPS("tty", "ahci"),
//   );
//
//   INITCALL_FN(my_driver) = my_driver_init;
//
// Or the short form for drivers with no deps:
//   DEFINE_INITCALL_NODEPS(my_driver, INITCALL_LEVEL_SUBSYS, my_driver_init);
//
// Return value: 0 = success, non-zero = failure (logged, boot continues
// unless INITCALL_FLAG_REQUIRED is set, in which case the kernel panics).
//
// Kernel modules use the exact same macros.  The ELF section mechanism
// ensures they are picked up automatically with no changes to main.c.

// ── Execution levels ──────────────────────────────────────────────────────
#define INITCALL_LEVEL_EARLY   0   // pre-scheduler, no sleeping
#define INITCALL_LEVEL_SUBSYS  1   // post-scheduler, may sleep

// ── Flags ─────────────────────────────────────────────────────────────────
#define INITCALL_FLAG_NONE      0
#define INITCALL_FLAG_REQUIRED  (1 << 0)  // panic on failure

// INITCALL_FLAG_PREEMPT_OFF: run the whole init function with preemption
// disabled.  Useful for short hardware bring-up sequences that must not
// be interleaved with scheduler activity — e.g. programming IOAPIC
// redirection entries, LAPIC config, MSR writes during CPU setup.
//
// RULES — a preempt-off initcall MUST NOT:
//   - call sched_sleep / sched_yield / irq_wait
//   - wait for an IRQ-driven completion
//   - allocate large objects (anything kmalloc might defer on)
//   - take any spinlock that can be held for more than a few cycles
//
// Violation is not silent: sched_sleep() panics loudly if called with
// preempt_depth > 0, so any accidental sleep under this flag is caught
// immediately on first execution.  See kernel/proc/sched.c.
#define INITCALL_FLAG_PREEMPT_OFF (1 << 1)

// ── Maximum dependencies per initcall ────────────────────────────────────
#define INITCALL_MAX_DEPS  8

// ── initcall_t — one registered initcall ─────────────────────────────────
typedef int (*initcall_fn_t)(void);

typedef struct {
    const char*    name;                         // unique driver name
    uint8_t        level;                        // INITCALL_LEVEL_*
    uint8_t        flags;                        // INITCALL_FLAG_*
    initcall_fn_t  fn;                           // init function
    const char*    deps[INITCALL_MAX_DEPS + 1];  // NULL-terminated dep list
} initcall_t;

// ── INITCALL_DEPS — build a NULL-terminated dependency list ──────────────
// Usage: .deps = INITCALL_DEPS("tty", "ahci")
// Up to INITCALL_MAX_DEPS entries.  Always NULL-terminated.
#define INITCALL_DEPS(...)  { __VA_ARGS__, NULL }

// ── DEFINE_INITCALL — register an initcall ───────────────────────────────
// Places an initcall_t pointer into the correct ELF section.
// The __used__ + KEEP() pair ensures the linker never discards it.
//
// Usage:
//   DEFINE_INITCALL(my_driver, INITCALL_LEVEL_SUBSYS,
//       .deps  = INITCALL_DEPS("tty", "pci"),
//       .flags = INITCALL_FLAG_REQUIRED,
//       .fn    = my_driver_init,
//   );

#define _INITCALL_SECTION(level) \
    ".initcall_" #level

// _INITCALL_STR forces macro expansion before stringification so that
// INITCALL_LEVEL_EARLY (which is 0) becomes the string "0", not the
// string "INITCALL_LEVEL_EARLY".
#define _INITCALL_STR2(x) #x
#define _INITCALL_STR(x)  _INITCALL_STR2(x)

#define DEFINE_INITCALL(id, _level, ...)                                    \
    static initcall_t __initcall_desc_##id = {                              \
        .name  = #id,                                                       \
        .level = (_level),                                                  \
        .flags = INITCALL_FLAG_NONE,                                        \
        .deps  = { NULL },                                                  \
        __VA_ARGS__                                                         \
    };                                                                      \
    static initcall_t* __initcall_ptr_##id                                  \
        __attribute__((__used__,                                            \
                       __section__(".initcall_" _INITCALL_STR(_level))))    \
        = &__initcall_desc_##id

// ── DEFINE_INITCALL_NODEPS — shorthand for dep-free initcalls ────────────
#define DEFINE_INITCALL_NODEPS(id, _level, _fn)                            \
    DEFINE_INITCALL(id, _level, .fn = (_fn))

// ── Runner API ────────────────────────────────────────────────────────────
// Called once per level by main.c / init_kthread.
void do_initcalls_early(void);   // level EARLY — called from kmain
void do_initcalls_subsys(void);  // level SUBSYS — called from init_kthread
