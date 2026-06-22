// Per-CPU pageset (Phase 4E) — see pcp.h for design and safety claims.
//
// The PCP sits between kernel callers and pmm_buddy_alloc/free for
// order-0 only.  Hot paths use cmpxchg16b on cpu_t.pcp_hdr; the
// backing array (cpu_t.pcp_pages[]) is per-CPU memory, indexed by
// the count word in pcp_hdr.
//
// Refill / drain take g_pmm_lock once per batch (SLAB_PCPU_PCP_REFILL
// pages = 32), so the hot-path lock contention for order-0 collapses
// to ~1 in 32 allocs after warmup.

#include "pcp.h"
#include "pmm.h"
#include "cpu.h"
#include "smp.h"
#include "common.h"

// IRQ save/restore — same idiom as slab_pcpu.c.  Used to bracket the
// refill/drain slow paths against re-entry from interrupts that
// could also call pcp_alloc/free and corrupt the {pcp_hdr, pcp_pages}
// invariant.  The fast path's cmpxchg16b handles re-entry naturally
// via tid; only the slow paths need cli.
ALWAYS_INLINE static uint64_t local_irq_save_pcp(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
ALWAYS_INLINE static void local_irq_restore_pcp(uint64_t f) {
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}

// pmm.c internal helpers re-exported for batched refill/drain so the
// PCP can hold g_pmm_lock for the whole batch instead of acquiring
// once per page.
extern phys_addr_t pmm_buddy_alloc_locked_for_pcp(uint8_t order);
extern void        pmm_buddy_free_locked_for_pcp(phys_addr_t addr, uint8_t order);
extern void        pmm_pcp_lock(uint64_t* flags_out);
extern void        pmm_pcp_unlock(uint64_t flags);

// Refill: pull SLAB_PCPU_PCP_REFILL order-0 pages from buddy in one
// locked critical section, push them onto this CPU's pcp_pages.
// Caller is in IRQ-save context.
static phys_addr_t pcp_refill_one(cpu_t* c) {
    phys_addr_t batch[SLAB_PCPU_PCP_REFILL];
    int n = 0;

    uint64_t pmm_flags;
    pmm_pcp_lock(&pmm_flags);
    for (int i = 0; i < SLAB_PCPU_PCP_REFILL; i++) {
        phys_addr_t p = pmm_buddy_alloc_locked_for_pcp(0);
        if (p == PMM_INVALID_ADDR) break;
        batch[n++] = p;
    }
    pmm_pcp_unlock(pmm_flags);

    if (n == 0) return PMM_INVALID_ADDR;

    // Save one for the caller.
    phys_addr_t result = batch[--n];

    // Push the rest onto pcp_pages.  We hold IRQs disabled, so no
    // re-entry on this CPU.  Other CPUs don't touch our pcp_*.
    uint32_t cnt = c->pcp_hdr.count;
    int      space = SLAB_PCPU_PCP_DEPTH - (int)cnt;
    int      pushed = (n < space) ? n : space;
    for (int i = 0; i < pushed; i++) {
        c->pcp_pages[cnt++] = batch[--n];
    }
    // Bump tid + count under cmpxchg semantics — owner-only write,
    // but use the cmpxchg loop so that any IRQ-time fast paths that
    // raced in see the updated tid (their CAS will fail and they'll
    // retry against the fresh state).
    uint64_t old_lo, old_hi;
    this_cpu_load16b_field(pcp_hdr, &old_lo, &old_hi);
    uint64_t new_lo = ((uint64_t)cnt) & 0xFFFFFFFFULL;  // count field
    while (!this_cpu_cmpxchg16b_field(pcp_hdr, &old_lo, &old_hi,
                                       new_lo, old_hi + 1)) {
        // Some IRQ bumped tid while we were assembling — re-publish
        // count (we still own the array writes; only the header
        // changed).  Take whatever count is current and just bump.
        new_lo = ((uint64_t)cnt) & 0xFFFFFFFFULL;
    }

    // Anything left over (rare — only if pcp was almost full) goes
    // straight back to buddy.
    while (n > 0) {
        pmm_pcp_lock(&pmm_flags);
        pmm_buddy_free_locked_for_pcp(batch[--n], 0);
        pmm_pcp_unlock(pmm_flags);
    }

    return result;
}

// Drain half the pcp back to the buddy in one locked critical
// section.  Called when pcp is full on free, and from pcp_drain_all().
static void pcp_drain_one(cpu_t* c, uint32_t how_many) {
    if (how_many == 0) return;
    uint32_t cnt = c->pcp_hdr.count;
    if (cnt == 0) return;
    if (how_many > cnt) how_many = cnt;

    phys_addr_t batch[SLAB_PCPU_PCP_DEPTH];
    for (uint32_t i = 0; i < how_many; i++) {
        cnt--;
        batch[i] = c->pcp_pages[cnt];
    }

    // Publish new count under cmpxchg.
    uint64_t old_lo, old_hi;
    this_cpu_load16b_field(pcp_hdr, &old_lo, &old_hi);
    uint64_t new_lo = ((uint64_t)cnt) & 0xFFFFFFFFULL;
    while (!this_cpu_cmpxchg16b_field(pcp_hdr, &old_lo, &old_hi,
                                       new_lo, old_hi + 1)) {
        new_lo = ((uint64_t)cnt) & 0xFFFFFFFFULL;
    }

    // Hand the batch back to the buddy in one locked block.
    uint64_t pmm_flags;
    pmm_pcp_lock(&pmm_flags);
    for (uint32_t i = 0; i < how_many; i++) {
        pmm_buddy_free_locked_for_pcp(batch[i], 0);
    }
    pmm_pcp_unlock(pmm_flags);

    c->pcp_drains++;
}

phys_addr_t pcp_alloc(void) {
    // The old "migration-safe" fast path read pcp_pages[count-1] on
    // one CPU and then CAS'd whatever CPU the task had migrated to.
    // When the two CPUs' {count, tid} pairs coincided (trivially
    // common early on — both counters start at 0), the CAS succeeded
    // against the WRONG header and the same physical frame was handed
    // to two owners.  Frames double-allocated this way showed up as
    // foot's heap overlapping the page cache's font reads ("FFTM"
    // bytes inside pointers), thread stacks overlapping each other,
    // and the buddy freelist #GP.  Pop with IRQs off and this_cpu()
    // resolved exactly once — no remote CPU ever touches our slots,
    // so plain reads are safe; the header CAS stays (pcp_drain_all
    // CASes remote headers during OOM reclaim).
    for (;;) {
        uint64_t flags = local_irq_save_pcp();
        cpu_t*   c     = this_cpu();
        uint64_t snap_lo, snap_hi;
        this_cpu_load16b_field(pcp_hdr, &snap_lo, &snap_hi);
        uint32_t count = (uint32_t)snap_lo;
        if (count == 0) {
            phys_addr_t r = pcp_refill_one(c);
            c->pcp_misses++;
            local_irq_restore_pcp(flags);
            return r;
        }
        phys_addr_t p = c->pcp_pages[count - 1];
        uint64_t    new_lo = ((uint64_t)(count - 1)) & 0xFFFFFFFFULL;
        if (this_cpu_cmpxchg16b_field(pcp_hdr, &snap_lo, &snap_hi,
                                       new_lo, snap_hi + 1)) {
            c->pcp_hits++;
            local_irq_restore_pcp(flags);
            return p;
        }
        local_irq_restore_pcp(flags);
        // CAS failed (remote drain raced): retry.
    }
}

void pcp_free(phys_addr_t phys) {
    if (phys == PMM_INVALID_ADDR) return;
    // Frames enter the per-CPU stash with refcount cleared, so a later
    // pcp_alloc starts every frame from a known-zero count (it then
    // sets 1).  Without this a page-table frame freed via the public
    // pmm_buddy_free (which routes order-0 here, NOT through
    // pmm_ref_dec) carried its stale rc into the stash.
    extern void pmm_ref_zero(phys_addr_t addr);
    pmm_ref_zero(phys);
    // Mirror of pcp_alloc: the old path wrote pcp_pages[count] on one
    // CPU and could publish count+1 on another after migrating — the
    // second CPU's slot held a stale frame that a later pop handed
    // out as "free" while its real owner still used it.  IRQs off +
    // single this_cpu() resolution; see pcp_alloc.
    for (;;) {
        uint64_t flags = local_irq_save_pcp();
        cpu_t*   c     = this_cpu();
        uint64_t snap_lo, snap_hi;
        this_cpu_load16b_field(pcp_hdr, &snap_lo, &snap_hi);
        uint32_t count = (uint32_t)snap_lo;
        if (count >= SLAB_PCPU_PCP_DEPTH) {
            pcp_drain_one(c, SLAB_PCPU_PCP_REFILL);
            local_irq_restore_pcp(flags);
            // Now retry the push — should fit.
            continue;
        }
        c->pcp_pages[count] = phys;
        uint64_t new_lo = ((uint64_t)(count + 1)) & 0xFFFFFFFFULL;
        if (this_cpu_cmpxchg16b_field(pcp_hdr, &snap_lo, &snap_hi,
                                       new_lo, snap_hi + 1)) {
            c->pcp_hits++;
            local_irq_restore_pcp(flags);
            return;
        }
        local_irq_restore_pcp(flags);
    }
}

void pcp_drain_all(void) {
    // Walk every CPU's pcp and drain it to buddy.  Used by the shrinker (4F)
    // and as a synchronous reclaim hook in pmm_buddy_alloc on OOM.  This is a
    // genuine cross-CPU access to another core's pcp_hdr/pcp_pages, which the
    // owning core also mutates lock-free.  We do NOT rely on a quiescence
    // "contract" (the previous code did, and it was false: pcp_alloc/free take
    // no g_pmm_lock, so a remote owner can race a drain — the source of the
    // [pmm] DOUBLE-ALLOC).  Instead each per-CPU drain snapshots the frames and
    // then claims the header with a LOCKed cmpxchg16b; any concurrent owner op
    // bumps tid and fails the claim, forcing a retry.
    extern cpu_t g_cpus[MAX_CPUS];
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        cpu_t* c = &g_cpus[i];
        if (!c->self) continue;  // CPU slot not initialised
        // Cross-CPU drain: the owning core may concurrently pcp_alloc/free.
        // It bumps pcp_hdr.tid on EVERY op, so we snapshot the frames first
        // and then claim the whole stash with a LOCKed cmpxchg16b that only
        // succeeds if the header (count,tid) is unchanged — which proves the
        // snapshot was consistent.  On any concurrent owner op the CAS fails
        // and we retry.  (The old code used plain reads then plain
        // count=0/tid++ writes here, so a concurrent pcp_alloc could hand out
        // a frame that this drain also freed to buddy — the [pmm] DOUBLE-ALLOC
        // class.  Read-then-claim closes it without an IPI.)
        for (;;) {
            // Read both 8-byte words exactly as the cmpxchg16b will compare
            // them: lo = [count|pad], hi = [tid].
            uint64_t lo = *((volatile uint64_t*)&c->pcp_hdr);
            uint64_t hi = *((volatile uint64_t*)&c->pcp_hdr + 1);
            uint32_t cnt = (uint32_t)lo;
            if (cnt == 0) break;
            if (cnt > SLAB_PCPU_PCP_DEPTH) continue;   // torn header; retry
            phys_addr_t frames[SLAB_PCPU_PCP_DEPTH];
            for (uint32_t j = 0; j < cnt; j++)
                frames[j] = c->pcp_pages[j];
            // Claim: header (lo,hi) -> (0, hi+1).  Fails (retry) if the owner
            // popped/pushed since our read, which also means our frame
            // snapshot would be stale.
            uint64_t exp_lo = lo, exp_hi = hi;
            if (!cmpxchg16b_abs(&c->pcp_hdr, &exp_lo, &exp_hi,
                                (uint64_t)0, hi + 1))
                continue;
            uint64_t pmm_flags;
            pmm_pcp_lock(&pmm_flags);
            for (uint32_t j = 0; j < cnt; j++)
                pmm_buddy_free_locked_for_pcp(frames[j], 0);
            pmm_pcp_unlock(pmm_flags);
            c->pcp_drains++;
            break;
        }
    }
}

void pcp_stats_get(uint32_t cpu, pcp_stats_t* out) {
    if (cpu >= MAX_CPUS || !out) return;
    extern cpu_t g_cpus[MAX_CPUS];
    out->hits   = g_cpus[cpu].pcp_hits;
    out->misses = g_cpus[cpu].pcp_misses;
    out->drains = g_cpus[cpu].pcp_drains;
}
