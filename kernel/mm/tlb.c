// ── TLB shootdown (Phase 9-7) ───────────────────────────────────────────
//
// See tlb.h for the overall design.  This file owns:
//   - per-CPU shootdown MPSC queue
//   - tlb_flush_range / tlb_flush_mm senders
//   - tlb_shootdown_drain receiver (called from VEC_IPI_TLB_FLUSH)
//
// Lock-free: senders allocate a `tlb_flush_slot_t` on their own kstack,
// CAS-prepend it to each target CPU's queue head, send VEC_IPI_TLB_FLUSH
// to each target, then spin on `done` with cpu_relax.  Targets drain the
// whole queue via one __atomic_exchange_n of the head, reverse the LIFO
// list, execute each slot, publish done with RELEASE.  Mirrors the
// ipi_call_slot_t pattern already proven in ipi.c.
//
// Self-flush is inline — we never IPI ourselves.

#include "tlb.h"
#include "common.h"
#include "cpu.h"
#include "preempt.h"
#include "smp.h"
#include "lapic.h"

// ── Per-CPU shootdown queue ─────────────────────────────────────────────
//
// One head per CPU.  Senders push; the owner CPU drains in the IPI
// handler.  No lock — CAS on the head word provides MPSC semantics.
// Stored out-of-line (not in cpu_t) because the type lives only here.

typedef struct tlb_flush_slot {
    uint64_t start;                   // user va inclusive
    uint64_t end;                     // user va exclusive; 0 = full flush
    volatile uint32_t done;           // receiver → sender ack
    struct tlb_flush_slot* next;      // MPSC link
} tlb_flush_slot_t;

static tlb_flush_slot_t* s_tlb_head[MAX_CPUS];

// ── Per-sender slot pool ────────────────────────────────────────────────
// A sender needs one outgoing slot per remote target.  We can't put
// MAX_CPUS slots on the kstack (2 KiB would consume a quarter of the
// 8 KiB kstack — observed stack corruption on deep syscall chains).
// Instead keep a per-sender-CPU row here in BSS:
//     s_send_slots[sender_cpu][target_cpu]
// Only ONE shootdown is in flight per sender at a time (the entire
// tlb_flush_common runs under preempt_disable — see below), so the row
// owned by `me` is exclusively ours.  Target slots are populated only
// for actual targets; we don't care what's in the unused cells.
// Bounded at compile time: 64 * 64 * 32 B = 128 KiB BSS, no heap.
static tlb_flush_slot_t s_send_slots[MAX_CPUS][MAX_CPUS];

// Invalidate one 4 KiB page on the current CPU.  Non-global entries only;
// we don't use global pages, so invlpg is sufficient (no CR4.PGE dance).
ALWAYS_INLINE void tlb_invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

// Full TLB flush on the current CPU via CR3 reload.  Preserves the
// currently loaded pml4 — we read CR3, write it back unchanged.
ALWAYS_INLINE void tlb_flush_local_all(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

// Threshold: invlpg is cheap per-entry but each one is ~100 cycles.
// Above this count a single CR3 reload beats iterating.  Matches
// Linux's tlb_single_page_flush_ceiling (33 in modern kernels, we
// pick a round 32 — one cache line's worth of PTEs).
#define TLB_INVLPG_CEILING  32u

// Execute one shootdown descriptor on the CURRENT cpu.  Receiver-side.
// No-op if the descriptor's (start,end) is empty or span exceeds the
// ceiling: promote to a full flush.
ALWAYS_INLINE void tlb_do_flush(uint64_t start, uint64_t end) {
    if (end == 0 || end <= start) {
        tlb_flush_local_all();
        return;
    }
    uint64_t span = (end - start + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (span > TLB_INVLPG_CEILING) {
        tlb_flush_local_all();
        return;
    }
    for (uint64_t va = start & ~PAGE_MASK; va < end; va += PAGE_SIZE)
        tlb_invlpg(va);
}

// Push onto the target CPU's shootdown queue.  CAS loop — no lock.
static void tlb_queue_push(uint32_t cpu, tlb_flush_slot_t* slot) {
    for (;;) {
        tlb_flush_slot_t* old = atomic_load_relaxed(&s_tlb_head[cpu]);
        slot->next = old;
        if (__atomic_compare_exchange_n(&s_tlb_head[cpu], &old, slot, 0,
                                         __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return;
        cpu_relax();
    }
}

// ── Receiver ─────────────────────────────────────────────────────────────
//
// Runs in VEC_IPI_TLB_FLUSH context on the target CPU.  Detach the whole
// MPSC list with one xchg, reverse it (LIFO → FIFO), then run each
// descriptor.  Publish done=1 with RELEASE so the sender observes any
// side effects (there are none, but keep the discipline).

void tlb_shootdown_drain(void) {
    uint32_t me = this_cpu()->id;

    tlb_flush_slot_t* list = __atomic_exchange_n(&s_tlb_head[me], NULL,
                                                  __ATOMIC_ACQUIRE);
    // Reverse in place.
    tlb_flush_slot_t* rev = NULL;
    while (list) {
        tlb_flush_slot_t* nxt = list->next;
        list->next = rev;
        rev = list;
        list = nxt;
    }

    // Coalescing opportunity: if any slot requests a full flush, or the
    // total unique pages exceed the ceiling, one mov-to-cr3 covers them
    // all.  Scan first, then either flush-once + ack-all, or per-slot.
    int need_full = 0;
    uint64_t total_pages = 0;
    for (tlb_flush_slot_t* s = rev; s; s = s->next) {
        if (s->end == 0 || s->end <= s->start) { need_full = 1; break; }
        total_pages += (s->end - s->start + PAGE_SIZE - 1) >> PAGE_SHIFT;
        if (total_pages > TLB_INVLPG_CEILING) { need_full = 1; break; }
    }

    if (need_full) {
        tlb_flush_local_all();
        while (rev) {
            tlb_flush_slot_t* slot = rev;
            rev = rev->next;
            __atomic_store_n(&slot->done, 1u, __ATOMIC_RELEASE);
        }
        return;
    }

    // Small total — iterate invlpg per slot.
    while (rev) {
        tlb_flush_slot_t* slot = rev;
        rev = rev->next;
        for (uint64_t va = slot->start & ~PAGE_MASK; va < slot->end;
             va += PAGE_SIZE)
            tlb_invlpg(va);
        __atomic_store_n(&slot->done, 1u, __ATOMIC_RELEASE);
    }
}

// ── Sender ───────────────────────────────────────────────────────────────
//
// Stack-allocate N descriptors (N = number of targets ≤ MAX_CPUS).
// For each target except self:
//   1. fill descriptor
//   2. push onto target's MPSC queue
//   3. IPI target
// Then flush locally inline and spin-wait on every descriptor's done=1.
//
// Preempt is disabled across the whole operation so:
//   (a) the cpumask snapshot stays accurate — we can't context-switch
//       onto another CPU while iterating.
//   (b) self-flush is in sync with the snapshot (no window where we
//       migrate away and a new bit lights up we didn't flush).

static void tlb_flush_common(task_mm_t* mm, uint64_t start, uint64_t end) {
    if (!mm) return;

    preempt_disable();

    uint32_t me   = this_cpu()->id;
    int self_hit  = 0;
    // Snapshot targets.  task_mm_cpumask_read is an ACQUIRE load so we
    // see every bit any CPU has published before this point.
    cpumask_t targets = task_mm_cpumask_read(mm);

    // Remote-target descriptors live in the per-sender row of the global
    // pool.  Exclusive ownership is guaranteed by preempt_disable: no
    // context switch, no reentrancy on this CPU until tlb_flush_common
    // returns, so no second caller can touch our row.  See the
    // s_send_slots definition above for the size rationale.
    tlb_flush_slot_t* my_slots = s_send_slots[me];
    uint32_t slot_idx = 0;

    // Phase 1: publish descriptors + IPIs.
    for (uint32_t cpu = 0; cpu < g_num_cpus; cpu++) {
        if (!(targets & ((cpumask_t)1u << cpu))) continue;
        if (cpu == me) { self_hit = 1; continue; }

        tlb_flush_slot_t* s = &my_slots[slot_idx++];
        s->start = start;
        s->end   = end;
        s->done  = 0;
        s->next  = NULL;
        tlb_queue_push(cpu, s);

        lapic_send_ipi(g_cpus[cpu].apic_id, VEC_IPI_TLB_FLUSH);
    }

    // Phase 2: self-flush inline.  Doing this between "IPIs are in
    // flight" and "wait for ACK" overlaps our own flush with the
    // receivers' work — a real wall-clock win on multi-target shots.
    if (self_hit)
        tlb_do_flush(start, end);

    // Phase 3: wait for every remote target to ACK.  If the target was
    // idle with IRQs on, the IPI wakes it immediately.  If it was in a
    // long IRQ-disabled kernel critical section, this blocks — same
    // cost as Linux.  Bounded: critical sections are short and the
    // receiver work is O(pages invalidated), so max latency is small.
    for (uint32_t i = 0; i < slot_idx; i++) {
        while (!__atomic_load_n(&my_slots[i].done, __ATOMIC_ACQUIRE))
            cpu_relax();
    }

    preempt_enable();
}

void tlb_flush_range(task_mm_t* mm, uint64_t start, uint64_t end) {
    tlb_flush_common(mm, start, end);
}

void tlb_flush_mm(task_mm_t* mm) {
    // end == 0 → full flush marker the receiver honours.
    tlb_flush_common(mm, 0, 0);
}
