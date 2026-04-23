#pragma once
#include "common.h"

/* ── Kernel panic — DEBUGGING.md §3 ────────────────────────────────
 *
 * panic() never returns.  On call it:
 *   1. Disables interrupts on this CPU.
 *   2. IPIs every other CPU to halt (so multi-core state is frozen).
 *   3. Dumps the full context to serial, in order:
 *        - The panic message (what tripped it).
 *        - CPU state: all GP regs + RIP + RSP + RFLAGS + CR0/2/3/4 + CS/DS/SS.
 *        - Symbolized stack backtrace via the frame-pointer walker.
 *        - Current task: pid, comm, state, ksp.
 *        - Recent log ring tail.
 *        - Event trace ring tail (per-CPU, once trace.c is wired in).
 *   4. Hangs in an HLT loop.  Does NOT reboot.  Does NOT drop to a
 *      monitor.  The hang is deliberate: GDB can then attach and
 *      inspect (§3.3).
 *
 * Prefer panic() over raw halt loops anywhere a bug was detected —
 * never hide one behind `while (1) hlt`.
 */

__attribute__((noreturn, format(printf, 1, 2)))
void panic(const char* fmt, ...);

/* panic_from_exception — entry point used by the IDT exception handlers.
 * Takes the interrupt frame so the register dump reflects the faulting
 * context, not the panic() call site. */
struct interrupt_frame_t;
__attribute__((noreturn))
void panic_from_exception(const char* msg,
                           struct interrupt_frame_t* frame,
                           uint64_t error_code,
                           int has_ec);
