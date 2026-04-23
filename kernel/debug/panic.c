/* ── Kernel panic — DEBUGGING.md §3 implementation ───────────────────
 *
 * Entry points:
 *   panic(fmt, ...)                    — free-form diagnostic panic
 *   panic_from_exception(...)          — routed from IDT exception ISRs
 *
 * Both funnel through panic_common() which emits the canonical dump
 * order (§3.2) and hangs.  Reboot-on-panic is intentionally absent —
 * the hang leaves state intact for GDB (§3.3).
 */

#include "panic.h"
#include "log.h"
#include "kprintf.h"
#include "common.h"
#include "cpu.h"
#include "stackwalk.h"
#include "idt.h"
#include "lapic.h"
#include "trace.h"
#include "process.h"

/* Freestanding va_list. */
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)

/* Re-entrancy guard: a panic-inside-a-panic must not loop forever.
 * First arrival does the full dump; anyone else spins. */
/* Plain integer; accessed with __atomic_* primitives only.  Clang in
 * freestanding mode refuses __atomic_compare_exchange_n on _Atomic-
 * qualified integers. */
static uint32_t s_panic_owner = 0xFFFFFFFFu;  /* 0xFF = no owner */

static uint64_t read_cr0(void) { uint64_t v; __asm__ volatile ("mov %%cr0,%0":"=r"(v)); return v; }
static uint64_t read_cr2(void) { uint64_t v; __asm__ volatile ("mov %%cr2,%0":"=r"(v)); return v; }
static uint64_t read_cr3(void) { uint64_t v; __asm__ volatile ("mov %%cr3,%0":"=r"(v)); return v; }
static uint64_t read_cr4(void) { uint64_t v; __asm__ volatile ("mov %%cr4,%0":"=r"(v)); return v; }

static uint16_t read_cs(void)  { uint16_t v; __asm__ volatile ("mov %%cs,%0" :"=r"(v)); return v; }
static uint16_t read_ds(void)  { uint16_t v; __asm__ volatile ("mov %%ds,%0" :"=r"(v)); return v; }
static uint16_t read_ss(void)  { uint16_t v; __asm__ volatile ("mov %%ss,%0" :"=r"(v)); return v; }

/* Broadcast VEC_IPI_HALT so every other CPU jumps to ipi_halt_entry
 * (asm stub in irq_stubs.asm) and spins in cli/hlt.  No handshake —
 * we don't wait for them to acknowledge; the sends and their delivery
 * via LAPIC are fast enough that by the time we've finished the
 * serial dump the other CPUs are guaranteed to be frozen. */
static void halt_other_cpus(void) {
    uint32_t my_id  = 0;
    uint64_t self_gs;
    __asm__ volatile ("mov %%gs:0,%0" : "=r"(self_gs));
    if (self_gs) my_id = this_cpu()->id;

    for (unsigned i = 0; i < g_num_cpus; i++) {
        if (i == my_id) continue;
        lapic_send_ipi(g_cpus[i].apic_id, VEC_IPI_HALT);
    }
}

/* Dump the interrupt_frame_t into one nicely-formatted block.  When
 * frame is NULL (non-exception panic) we read the regs from our own
 * context. */
static void dump_cpu_state(const interrupt_frame_t* frame,
                            uint64_t error_code, int has_ec) {
    kprintf_atomic("=== CPU state ===\n");

    uint64_t rip, rsp, rflags;
    uint16_t cs, ss;
    if (frame) {
        rip = frame->ip; rsp = frame->sp; rflags = frame->flags;
        cs  = (uint16_t)frame->cs; ss = (uint16_t)frame->ss;
    } else {
        /* Not an exception: record our own frame.  RIP approximates
         * the panic() call site; RSP is our current stack. */
        __asm__ volatile ("lea 0(%%rip),%0" : "=r"(rip));
        __asm__ volatile ("mov %%rsp,%0" : "=r"(rsp));
        __asm__ volatile ("pushfq; pop %0" : "=r"(rflags));
        cs = read_cs(); ss = read_ss();
    }

    kprintf_atomic("  RIP=0x%016lx  RSP=0x%016lx  RFLAGS=0x%016lx\n",
                    rip, rsp, rflags);
    kprintf_atomic("  CR0=0x%016lx  CR2=0x%016lx\n", read_cr0(), read_cr2());
    kprintf_atomic("  CR3=0x%016lx  CR4=0x%016lx\n", read_cr3(), read_cr4());
    kprintf_atomic("  CS=0x%04x  DS=0x%04x  SS=0x%04x\n",
                    (unsigned)cs, (unsigned)read_ds(), (unsigned)ss);
    if (has_ec) {
        kprintf_atomic("  error_code=0x%016lx\n", error_code);
    }
}

static const char* task_state_str(uint32_t st) {
    /* Mirrors task_state_t in kernel/proc/process.h.  Strings short
     * so they don't wrap in the panic dump. */
    switch ((task_state_t)st) {
    case TASK_READY:    return "READY";
    case TASK_RUNNING:  return "RUNNING";
    case TASK_DEAD:     return "DEAD";
    case TASK_SLEEPING: return "SLEEPING";
    case TASK_ZOMBIE:   return "ZOMBIE";
    }
    return "?";
}

static void dump_current_task(void) {
    kprintf_atomic("=== current task ===\n");
    uint64_t self_gs;
    __asm__ volatile ("mov %%gs:0,%0" : "=r"(self_gs));
    if (!self_gs) {
        kprintf_atomic("  (no GS setup — pre-SMP or BSP init phase)\n");
        return;
    }
    task_t* t = current_task();
    if (!t) {
        kprintf_atomic("  (idle — no current task)\n");
        return;
    }
    kprintf_atomic("  pid=%u  comm=\"%s\"  state=%s  kstack_top=0x%016lx\n",
                    t->pid, t->comm,
                    task_state_str((uint32_t)t->state),
                    (uint64_t)t->kstack_top);
}

/* Hex-dump the top 16 qwords of the stack pointed at by rsp.  Only
 * called when the RSP is canonical and the walker succeeded. */
static void dump_stack_head(uint64_t rsp) {
    uint64_t top = rsp >> 47;
    if (top != 0 && top != 0x1FFFFull) return;
    kprintf_atomic("=== stack head (@RSP) ===\n");
    uint64_t* p = (uint64_t*)rsp;
    for (int i = 0; i < 16; i++) {
        kprintf_atomic("  [rsp+0x%02x] = 0x%016lx\n",
                        (unsigned)(i * 8), p[i]);
    }
}

/* Single path shared by both entry points.  `frame` is NULL for the
 * free-form panic case; `msg` is always the final user-facing one-
 * liner. */
static __attribute__((noreturn))
void panic_common(const char* msg, const interrupt_frame_t* frame,
                   uint64_t error_code, int has_ec) {
    /* Re-entry guard — first caller wins the dump. */
    uint32_t expected = 0xFFFFFFFFu;
    uint32_t my_id    = 0;
    uint64_t self_gs;
    __asm__ volatile ("mov %%gs:0,%0" : "=r"(self_gs));
    if (self_gs) my_id = this_cpu()->id;
    if (!__atomic_compare_exchange_n(&s_panic_owner, &expected, my_id,
                                      0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        /* Someone else is already panicking.  Halt quietly. */
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }

    __asm__ volatile ("cli");
    halt_other_cpus();

    /* Banner first so the log scraper can find the panic line.
     * kprintf_atomic takes the serial lock once per call, so the
     * whole header is printed without interleaving even if another
     * CPU hadn't halted yet. */
    kprintf_atomic("\n");
    kprintf_atomic("################ KERNEL PANIC ################\n");
    kprintf_atomic("  %s\n", msg);
    kprintf_atomic("##############################################\n");

    /* CPU state (§3.2 item 2). */
    dump_cpu_state(frame, error_code, has_ec);

    /* Stack backtrace (§3.2 item 3). */
    uint64_t rbp, rip;
    if (frame) {
        /* Exception: use the frame's RIP as frame-0.  rbp we can't
         * see directly from interrupt_frame_t (only ip/cs/flags/sp/
         * ss are saved).  Approximate by reading our own rbp — the
         * backtrace will start in the panic path, then climb into
         * the exception handler chain and from there into the
         * faulting code via the saved rbp chain. */
        rbp = read_rbp();
        rip = frame->ip;
    } else {
        rbp = read_rbp();
        __asm__ volatile ("lea 0(%%rip),%0" : "=r"(rip));
    }
    stackwalk_print_from(rbp, rip);

    /* Current task (§3.2 item 4). */
    dump_current_task();

    /* Stack head dump — short raw view in case the walker gave up. */
    if (frame) dump_stack_head(frame->sp);

    /* Log ring tail (§3.2 item 6). */
    klog_ring_dump();

    /* Event ring tail (§3.2 item 7). */
    trace_ring_dump();

    kprintf_atomic("################ end of panic dump — halting ################\n");

    /* Hang.  No reboot. */
    for (;;) __asm__ volatile ("hlt");
}

void panic(const char* fmt, ...) {
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    panic_common(msg, (const interrupt_frame_t*)0, 0, 0);
}

void panic_from_exception(const char* msg,
                           struct interrupt_frame_t* frame,
                           uint64_t error_code,
                           int has_ec) {
    panic_common(msg, frame, error_code, has_ec);
}
