// Inter-processor interrupt handlers.
//
// Three vectors are reserved for cross-CPU coordination:
//
//   VEC_IPI_RESCHEDULE (0x40) — "a runnable task has landed on your
//       run queue; check reschedule_pending and context-switch if
//       you're in a preemptible state."  Fired by sched_wake and
//       sched_add when the target CPU is not the sender.
//
//   VEC_IPI_CALL       (0x42) — generic cross-CPU function call
//       (smp_call_function_single / _all).  Drains a per-CPU MPSC
//       queue of {fn, arg, done} slots.  Used by TLB shootdown and
//       any future "run this on CPU N" primitive.  Phase 9-5 adds
//       only the skeleton queue; real callers arrive in 9-6+.
//
//   VEC_IPI_TLB_FLUSH  (0x41) — range TLB invalidation.  Phase 9-6
//       owns this.  For 9-5 it's an EOI-only no-op so a stray fire
//       doesn't panic.
//
// The asm stubs in irq_stubs.asm save/restore GPRs and call lapic_eoi
// BEFORE entering each C handler — do not EOI again here.  `iretq`
// restores IRQs (RFLAGS.IF), so the handler runs with IRQs masked.

#include "common.h"
#include "cpu.h"
#include "sched.h"
#include "smp.h"

// ── VEC_IPI_RESCHEDULE ──────────────────────────────────────────────────
//
// The sender has already enqueued a task onto this CPU's rq and
// written reschedule_pending.  Our job is just to call sched_preempt()
// so that the return-from-IRQ path context-switches if we're preemptible
// (same as the LAPIC timer tick path).  sched_preempt respects
// preempt_depth, so if we were mid-critical-section it defers the
// switch until the enclosing preempt_enable().
//
// Setting reschedule_pending here too is harmless redundancy that makes
// the handler correct even if a buggy sender forgets to — cheap
// insurance against future callers going wrong.
void ipi_reschedule_handler(void) {
    // Set the flag and return.  DO NOT call sched_preempt() here: if the
    // target CPU was sitting inside do_switch's sti;hlt idle loop (which
    // is where every idle or sleeping CPU parks), a nested sched_preempt
    // → do_switch(1) would consume the freshly-enqueued task from the rq
    // before the outer do_switch loop's sti;hlt wakes and re-dequeues.
    // The outer loop would then find an empty rq and hlt forever, while
    // the inner identity-context-switch unwound harmlessly back through
    // the IPI stack — a guaranteed hang on any "task enqueued while I'm
    // sleeping in my own do_switch" path (ahci_submit → sched_sleep was
    // the smoking gun).
    //
    // The correct wake-from-idle path is: IPI wakes hlt, iretq returns
    // into do_switch's `cli`, loop re-acquires rq_lock, dequeues the
    // newly-enqueued task, and proceeds naturally.  For the case where
    // the target CPU was NOT idle (it was running a real task), setting
    // reschedule_pending is enough — sched_preempt() runs on the next
    // timer tick or preempt_enable, which is within one quantum (~1 ms).
    this_cpu()->reschedule_pending = 1;
}

// ── VEC_IPI_CALL ────────────────────────────────────────────────────────
//
// Drain the per-CPU call queue.  Each entry is a {fn, arg, done} tuple
// the sender placed into the MPSC ring via smp_call_function_single;
// we run fn(arg) for each one and publish `done=1` so the sender's
// spin-wait exits.  RELEASE ordering on done pairs with the sender's
// acquire load.
//
// Phase 9-5 lands the skeleton; there are no callers yet.  The body is
// kept minimal but complete so future users (TLB shootdown, cross-CPU
// kick) just populate the queue and send the IPI — no further kernel
// plumbing needed.
typedef struct ipi_call_slot {
    void     (*fn)(void*);
    void*     arg;
    volatile uint32_t done;     // 0 = pending, 1 = complete
    struct ipi_call_slot* next;
} ipi_call_slot_t;

// Per-CPU MPSC head — senders push, owner pops in ipi_call_handler.
// Lives here (not in cpu_t) because Phase 9-5 does not yet expose the
// type to other subsystems; moving it into cpu.h after 9-6 is a no-op
// rename.  Zero-initialised by BSS.
static ipi_call_slot_t* s_call_head[MAX_CPUS];

// Push onto the target CPU's MPSC queue.  Lock-free via xchg on the
// head pointer.  Sender invokes this, then sends VEC_IPI_CALL, then
// spins on slot->done.
//
// Not wired into any caller yet — exported for Phase 9-6's TLB path.
void smp_call_push(uint32_t cpu, ipi_call_slot_t* slot) {
    for (;;) {
        ipi_call_slot_t* old = atomic_load_relaxed(&s_call_head[cpu]);
        slot->next = old;
        if (__atomic_compare_exchange_n(&s_call_head[cpu], &old, slot, 0,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return;
        cpu_relax();
    }
}

void ipi_call_handler(void) {
    uint32_t me = cpu_id();

    // Detach the entire queue in one xchg — new pushes land on a fresh
    // (NULL) head and will be picked up by the next IPI.
    ipi_call_slot_t* list = __atomic_exchange_n(&s_call_head[me], NULL,
                                                  __ATOMIC_ACQUIRE);

    // The list is LIFO from the push order; reverse so we execute FIFO.
    // This is a 3-pointer in-place reverse — O(n), no allocation.
    ipi_call_slot_t* rev = NULL;
    while (list) {
        ipi_call_slot_t* nxt = list->next;
        list->next = rev;
        rev = list;
        list = nxt;
    }

    while (rev) {
        ipi_call_slot_t* slot = rev;
        rev = rev->next;

        slot->fn(slot->arg);

        // RELEASE so the sender, which acquires on done, sees every
        // write fn() made to slot->arg outputs before observing done=1.
        __atomic_store_n(&slot->done, 1u, __ATOMIC_RELEASE);
    }
}

// ── VEC_IPI_TLB_FLUSH ───────────────────────────────────────────────────
//
// Phase 9-5: no-op.  Phase 9-6 drains a per-CPU shootdown slot with
// {start, end, flush_all, done_count} and invlpg's the range (or reloads
// CR3 if flush_all).  Leaving the handler wired up now means enabling
// the vector on the BSP in idt_init() doesn't explode the first time a
// future caller tests it.
void ipi_tlb_flush_handler(void) {
    /* Phase 9-6 lands the real body. */
}
