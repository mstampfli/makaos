#pragma once
#include "common.h"
#include "smp.h"
#include "tss.h"

// MLFQ levels — must match sched.c.  Kept as a compile-time constant
// here so cpu_t can size its per-CPU runqueue arrays inline.
#define SCHED_MLFQ_LEVELS 4

// ── Per-CPU state ────────────────────────────────────────────────────────
//
// Every CPU in the system owns a `cpu_t` holding all of its private state:
// the task currently running, the preemption depth, the scheduler run
// queue slot, per-CPU slab magazines, per-CPU page frame cache, IRQ
// bookkeeping.  None of these fields require locking when accessed by the
// CPU that owns them, which is the whole point of the design — fast paths
// never touch a shared cache line.
//
// `this_cpu()` returns a pointer to the current CPU's cpu_t.
//
// Implementation: every cpu_t starts with a self-pointer at offset 0, and
// the per-CPU GS_BASE MSR points at that same cpu_t.  Reading `%gs:0`
// therefore yields the cpu_t* in a single instruction with no MSR access,
// no atomic, no global lookup.  Per-CPU field reads can also use
// `mov %gs:offsetof(field), %reg` directly via the this_cpu_read_*
// helpers below — saves one indirection vs. `this_cpu()->field` and
// compiles to a single instruction.
//
// SETUP ORDERING: cpu_init_bsp() MUST be called before any code reads
// the GS base, which means before the first this_cpu() / preempt_disable
// / current_task call.  kmain() does this immediately after BSS clear.
// Calling this_cpu() before cpu_init_bsp() returns garbage (whatever
// firmware left in GS_BASE, typically 0 → null deref).
//
// USERLAND GS INVARIANT: this kernel never exposes GS to userland (no
// arch_prctl, no ldt syscalls, nothing that lets a user process write
// GS_BASE).  Therefore we do NOT need swapgs on kernel entry — GS_BASE
// always points at the kernel's per-CPU data.  If we ever add a syscall
// that lets userland set GS_BASE, every IRQ/syscall entry MUST grow a
// swapgs and the corresponding exit a matching swapgs.
//
// Ordering note: any code that touches this_cpu()->rq / current / pcp
// must have preemption disabled (or be running inside sched_tick with
// IRQs disabled), otherwise it can be migrated to another CPU mid-
// operation and see a different cpu_t.

struct task_t;

// Per-CPU run queue: MLFQ with SCHED_MLFQ_LEVELS priority levels,
// each level a Chase-Lev work-stealing deque.  The owner pushes/pops
// lock-free on its own `bottom`; remote thieves CAS `top` to steal
// load-balancing work without touching the owner's hot cache line.
// sleep_head and zombie_head remain linked lists protected by
// cpu_t.rq_lock — they're cold paths (one op per sleep/wake) and
// don't justify the deque complexity.
#include "chaselev.h"
typedef struct cpu_rq_t {
    chaselev_deque_t levels[SCHED_MLFQ_LEVELS];
    struct task_t* sleep_head;   // tasks in TASK_SLEEPING owned by this CPU
    struct task_t* zombie_head;  // zombies owned by this CPU
    volatile uint32_t nr_running;  // advisory: ready-task count (for steal)
} cpu_rq_t;

// Per-CPU slab magazine — a freelist of recently-freed objects for each
// kmalloc size class.  Placeholder for Phase 4; declared now so cpu_t has
// a stable layout.
#define SLAB_MAGAZINE_CLASSES 10
typedef struct {
    void*    free_head[SLAB_MAGAZINE_CLASSES];
    uint32_t free_count[SLAB_MAGAZINE_CLASSES];
} slab_magazine_t;

// Per-CPU pageset — freelist of recently-freed order-0 pages.  Placeholder
// for Phase 4.
#define PCP_BATCH_SIZE 32
typedef struct {
    uint64_t pages[PCP_BATCH_SIZE];  // physical addresses
    uint32_t count;
} pmm_pcp_t;

typedef struct cpu_t {
    // ── Self-pointer at offset 0 (load-bearing) ─────────────────────────
    // GS_BASE points at this struct, so `mov %gs:0, %reg` yields the
    // cpu_t* itself.  Every per-CPU access compiles to one instruction.
    // Must stay at offset 0 — moving it breaks this_cpu().
    struct cpu_t*   self;

    // Identity.
    uint32_t        id;                 // 0..MAX_CPUS-1
    uint32_t        apic_id;            // LAPIC ID (may differ from id)

    // Scheduling.
    struct task_t*  current;            // task currently executing
    struct task_t*  idle;               // this CPU's idle task
    cpu_rq_t        rq;                 // run queue / sleep list / zombie list
    spinlock_t      rq_lock;            // protects rq state; IRQ-safe

    // Preemption: depth counter + pending-reschedule flag.  depth > 0 means
    // voluntary context switches are suppressed; IRQs still fire normally.
    uint32_t        preempt_depth;
    uint32_t        reschedule_pending; // set by sched_wake / timer when a
                                        // context switch should happen as
                                        // soon as preempt_depth hits zero

    // Work-stealing random-victim PRNG (xorshift64).  Per-CPU so no
    // contention; seeded lazily from TSC+id on first use.
    uint64_t        steal_rng;

    // Per-CPU allocator magazines (Phase 4).
    slab_magazine_t slab;
    pmm_pcp_t       pcp;

    // IRQ bookkeeping.  Pending counts for IRQ lines this CPU services.
    // The IRQ waiter lists themselves are per-IRQ (in irq_wait.c) and the
    // IRQ handler always runs on the CPU that's been assigned that line
    // by the IOAPIC, so access is naturally single-CPU in the common path.
    uint8_t         irq_pending[256];

    // RCU quiescent-state counter.  Bumped on every context switch and
    // every idle-loop iteration.  synchronize_rcu() waits until every
    // CPU's counter has advanced since the grace period began.
    // Written non-atomically by the owning CPU; read by any CPU via
    // atomic_load_relaxed (torn reads are fine — they just delay grace
    // period detection by one cycle).
    volatile uint64_t rcu_qs_count;

    // Statistics — non-atomic, owner-only.
    uint64_t        sched_ticks;
    uint64_t        context_switches;

    // Per-CPU Task State Segment.  The CPU reads this on every ring-3 → 0
    // transition (rsp[0]) and on exception delivery via IST (ist[0..6]).
    // Embedded inline — not a pointer — so syscall_entry can load RSP0
    // with a single `mov %gs:CPU_TSS_RSP0, %rsp`.  One shared GDT in
    // tss.c holds a TSS descriptor per CPU at slot (6 + 2*cpu_id),
    // pointing here.  Cold field: only touched on ring transitions,
    // kept at the end of cpu_t so the scheduler's hot cache line
    // (current/preempt_depth/rq) stays compact.
    tss_t           tss;

    // ── Per-CPU syscall / signal / exec scratch area ────────────────────
    //
    // These fields replace the previous global `g_syscall_user_*` /
    // `g_signal_*` / `g_exec_*` variables.  Under SMP round-robin,
    // those globals raced between concurrent syscalls on different
    // CPUs — observed as a kernel #GP inside iretq where the pushed
    // RIP was a non-canonical value that belonged to another task's
    // in-flight syscall.  Putting them in cpu_t makes each CPU's
    // in-flight syscall state strictly private.
    //
    // syscall_entry.asm accesses these via %gs:CPU_SYSCALL_* using
    // the offsets generated by asm_offsets.c.  C code accesses them
    // via this_cpu()->syscall_*.
    uint64_t        syscall_user_rsp;
    uint64_t        syscall_user_rip;
    uint64_t        syscall_user_rflags;
    uint64_t        syscall_user_rbp;
    uint64_t        syscall_user_rbx;
    uint64_t        syscall_user_r12;
    uint64_t        syscall_user_r13;
    uint64_t        syscall_user_r14;
    uint64_t        syscall_user_r15;
    uint64_t        syscall_arg5;      // r8  (mmap fd)
    uint64_t        syscall_arg6;      // r9  (mmap offset)

    uint8_t         signal_deliver;    // 1 = enter handler, 2 = sigreturn
    uint8_t         signal_in_syscall; // set while syscall_dispatch runs
    uint8_t         _sig_pad[6];       // align rdi to 8
    uint64_t        signal_rdi;        // signum passed as rdi to handler

    uint8_t         exec_requested;    // sys_exec sets this → asm reloads
    uint8_t         _exec_pad[7];      // align entry to 8
    uint64_t        exec_entry;
    uint64_t        exec_rsp;
    phys_addr_t     exec_pml4;
} cpu_t;

// Compile-time guarantee: the self-pointer is at offset 0.  this_cpu()
// reads %gs:0 so this offset is hard-coded into the inline asm below.
_Static_assert(__builtin_offsetof(cpu_t, self) == 0,
               "cpu_t.self must be at offset 0 — this_cpu() reads %gs:0");

// Array of per-CPU slots, sized to MAX_CPUS.  Only slots with a running
// CPU are live; g_num_cpus tracks the count.
extern cpu_t    g_cpus[MAX_CPUS];
extern unsigned g_num_cpus;

// ── this_cpu() — single-instruction per-CPU lookup ──────────────────────
//
// Reads the self-pointer at offset 0 of the current CPU's cpu_t via the
// GS segment base.  On a modern x86-64 this compiles to literally one
// instruction:  `mov %gs:0, %rax`.
//
// Pre-condition: cpu_init_bsp() (and cpu_init_ap() for non-BSP cores)
// must have run on the current CPU.  Calling this before that on an
// uninitialised CPU dereferences whatever firmware left in GS_BASE,
// which is almost certainly 0 → null deref → triple-fault.
ALWAYS_INLINE cpu_t* this_cpu(void) {
    cpu_t* p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}

// ── Direct per-CPU field access ─────────────────────────────────────────
//
// Reading `this_cpu()->field` compiles to two instructions:
//   mov %gs:0, %rax        ; load self-pointer
//   mov offset(%rax), %edx ; load the field via the pointer
//
// The `this_cpu_read_*` / `this_cpu_write_*` / `this_cpu_inc_*` helpers
// below encode the field offset directly as a `%gs:offset` displacement,
// collapsing the access to ONE instruction.  Use these on the hot
// path — preempt_disable, current_task, sched counters — anywhere the
// extra indirection matters.
//
// The `field` argument is a member name on cpu_t.  `__builtin_offsetof`
// gives a compile-time integer; the inline asm uses `%cN` to format it
// as an immediate displacement.
//
// SAFETY: these macros only read/write the OWNING CPU's field.  Cross-
// CPU access must use `g_cpus[id].field` and any necessary synchronisation.

#define this_cpu_read_u32(field) ({                                       \
    uint32_t __v;                                                          \
    __asm__ volatile("mov %%gs:%c1, %0"                                    \
        : "=r"(__v)                                                        \
        : "i"(__builtin_offsetof(cpu_t, field)));                          \
    __v;                                                                   \
})

#define this_cpu_read_u64(field) ({                                       \
    uint64_t __v;                                                          \
    __asm__ volatile("mov %%gs:%c1, %0"                                    \
        : "=r"(__v)                                                        \
        : "i"(__builtin_offsetof(cpu_t, field)));                          \
    __v;                                                                   \
})

#define this_cpu_read_ptr(field) ({                                       \
    void* __v;                                                             \
    __asm__ volatile("mov %%gs:%c1, %0"                                    \
        : "=r"(__v)                                                        \
        : "i"(__builtin_offsetof(cpu_t, field)));                          \
    __v;                                                                   \
})

#define this_cpu_write_u32(field, val) do {                               \
    __asm__ volatile("mov %0, %%gs:%c1"                                    \
        :                                                                  \
        : "r"((uint32_t)(val)),                                            \
          "i"(__builtin_offsetof(cpu_t, field))                            \
        : "memory");                                                       \
} while (0)

#define this_cpu_write_u64(field, val) do {                               \
    __asm__ volatile("mov %0, %%gs:%c1"                                    \
        :                                                                  \
        : "r"((uint64_t)(val)),                                            \
          "i"(__builtin_offsetof(cpu_t, field))                            \
        : "memory");                                                       \
} while (0)

#define this_cpu_write_ptr(field, val) do {                               \
    __asm__ volatile("mov %0, %%gs:%c1"                                    \
        :                                                                  \
        : "r"((void*)(val)),                                               \
          "i"(__builtin_offsetof(cpu_t, field))                            \
        : "memory");                                                       \
} while (0)

// Atomic-on-this-CPU increment / decrement of a u32 field.  Single
// instruction; no LOCK prefix because the memory is per-CPU and only
// the owning CPU writes it.  Used by preempt_disable / preempt_enable.
#define this_cpu_inc_u32(field) do {                                      \
    __asm__ volatile("incl %%gs:%c0"                                       \
        :                                                                  \
        : "i"(__builtin_offsetof(cpu_t, field))                            \
        : "memory", "cc");                                                 \
} while (0)

// Decrement and return whether the post-decrement value is non-zero
// (mirrors `decl + setnz`).  Used by preempt_enable to check if the
// outermost critical section is exiting.
#define this_cpu_dec_u32_nonzero(field) ({                                \
    uint8_t __nz;                                                          \
    __asm__ volatile("decl %%gs:%c1\n\tsetnz %0"                           \
        : "=q"(__nz)                                                       \
        : "i"(__builtin_offsetof(cpu_t, field))                            \
        : "memory", "cc");                                                 \
    __nz;                                                                  \
})

// Initialise cpu0's slot AND program GS_BASE so this_cpu() works.
// Called once during kmain, immediately after BSS clear, before any
// other code reads per-CPU state.  Phase 9 adds cpu_init_ap() for
// non-BSP cores.
void cpu_init_bsp(void);

// ── Convenience accessors used everywhere ───────────────────────────────
// These exist so call sites don't need to know the cpu_t layout.
//
// current_task / set_current_task use the direct gs-relative accessor so
// each call compiles to a single instruction.  This is the hottest
// per-CPU read in the kernel — it fires on every fdget, every
// preempt_enable's reschedule check, every signal delivery, etc.

ALWAYS_INLINE struct task_t* current_task(void) {
    return (struct task_t*)this_cpu_read_ptr(current);
}

ALWAYS_INLINE void set_current_task(struct task_t* t) {
    this_cpu_write_ptr(current, t);
}
