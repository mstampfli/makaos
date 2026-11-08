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

    // Push the leftover `n` frames onto pcp_pages.  We hold IRQs disabled, so
    // this CPU won't re-enter -- but a CROSS-CPU pcp_drain_all (slab shrinker /
    // OOM reclaim) CAN, concurrently, snapshot our stash, claim the header
    // (count->0, tid++), and free those frames to buddy.  The old code read the
    // count ONCE, wrote frames at that offset, and on a tid-mismatch retry
    // re-published the SAME pre-drain count -- resurrecting the drained (now
    // buddy-owned) frames as "available" in the stash, so a later pop handed a
    // buddy-free frame to an owner: the [pmm] DOUBLE-ALLOC that surfaced as
    // kstack/heap/pagetable double-ownership under exec+shrinker load.  Fix:
    // mirror pcp_free -- re-read the LIVE count on every attempt and write our
    // frames at the current top, so a racing drain that emptied the stash just
    // makes us publish our own frames from 0, never the frames it freed.
    for (;;) {
        uint64_t old_lo, old_hi;
        this_cpu_load16b_field(pcp_hdr, &old_lo, &old_hi);
        uint32_t cur = (uint32_t)old_lo;
        if (cur > SLAB_PCPU_PCP_DEPTH) continue;   // torn read; retry
        int space  = SLAB_PCPU_PCP_DEPTH - (int)cur;
        if (space <= 0 || n == 0) break;
        int pushed = (n < space) ? n : space;
        for (int i = 0; i < pushed; i++)
            c->pcp_pages[cur + i] = batch[n - 1 - i];
        uint64_t new_lo = ((uint64_t)(cur + pushed)) & 0xFFFFFFFFULL;
        if (this_cpu_cmpxchg16b_field(pcp_hdr, &old_lo, &old_hi,
                                       new_lo, old_hi + 1)) {
            n -= pushed;    // committed; the rest (if any) loops or spills below
        }
        // else: a drain/alloc bumped tid -> our page writes are stale, retry
        // against the fresh count (the frames we wrote were never published).
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

    // Snapshot-then-claim under a single fresh header read, mirroring the
    // pcp_refill_one fix: the old code read the count, snapshotted frames, then
    // re-read the header SEPARATELY and re-published a count derived from the
    // STALE first read -- so a concurrent cross-CPU pcp_drain_all that reset our
    // count to 0 and freed those frames in between would still let this claim
    // "succeed", double-freeing the batch and resurrecting a stale count.  Read
    // the header, snapshot the top `took`, and cmpxchg against THAT header; a
    // racing drain fails the cmpxchg and we retry from a fresh read.
    phys_addr_t batch[SLAB_PCPU_PCP_DEPTH];
    uint32_t took = 0;
    for (;;) {
        uint64_t old_lo, old_hi;
        this_cpu_load16b_field(pcp_hdr, &old_lo, &old_hi);
        uint32_t cnt = (uint32_t)old_lo;
        if (cnt == 0) return;
        if (cnt > SLAB_PCPU_PCP_DEPTH) continue;     // torn read; retry
        took = (how_many > cnt) ? cnt : how_many;
        uint32_t newcnt = cnt - took;
        for (uint32_t i = 0; i < took; i++)
            batch[i] = c->pcp_pages[newcnt + i];     // the top `took` frames
        uint64_t new_lo = ((uint64_t)newcnt) & 0xFFFFFFFFULL;
        if (this_cpu_cmpxchg16b_field(pcp_hdr, &old_lo, &old_hi,
                                       new_lo, old_hi + 1))
            break;                                    // claimed exclusively
        // else: a concurrent drain/alloc bumped tid -> re-read and retry.
    }

    uint64_t pmm_flags;
    pmm_pcp_lock(&pmm_flags);
    for (uint32_t i = 0; i < took; i++)
        pmm_buddy_free_locked_for_pcp(batch[i], 0);
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

// Owner-local drain callback: runs ON the CPU whose cache is being drained --
// remotely inside the VEC_IPI_CALL handler (IRQs already off) or inline for
// self (we disable IRQs to match).  Either way this CPU's own lock-free
// pcp_alloc/pcp_free/refill CANNOT run concurrently, so pcp_drain_one operates
// on a stable {pcp_hdr, pcp_pages} with no cross-CPU racer at all.
static void pcp_drain_self_cb(void* arg) {
    (void)arg;
    uint64_t flags = local_irq_save_pcp();     // no-op-ish in IPI ctx (already cli)
    pcp_drain_one(this_cpu(), SLAB_PCPU_PCP_DEPTH);
    local_irq_restore_pcp(flags);
}

void pcp_drain_all(void) {
    // IPI-based OWNER-LOCAL drain.  Used by the slab shrinker (4F) under memory
    // pressure.  The previous implementation drained each remote CPU's pcp
    // stash CROSS-CPU (snapshot + LOCKed cmpxchg16b claim).  That claim is
    // atomic against a SINGLE owner op, but the owner's multi-slot refill push
    // (pcp_refill_one) writes pcp_pages[] then republishes the count in a
    // separate step: a drain that snapshots between the slot write and the
    // count publish -- or that a refill retries around -- can leave a frame
    // both freed-to-buddy AND live in the stash, so a later pcp_alloc hands a
    // buddy-owned frame to a caller ([pmm] pcp-returned-owned, then the
    // write-after-free free-list scribble under exec+shrinker load, SMP-only).
    //
    // The robust fix: never touch a remote CPU's stash.  IPI each CPU to drain
    // its OWN cache with IRQs off, exactly like pcp_alloc/pcp_free already do.
    // The hot path stays 100% lock-free; only this rare reclaim path pays the
    // IPI.  g_pmm_lock is always held IRQs-off and pcp_drain_all is only called
    // with it released (slab_shrinker unlocks first), so the IPI handler taking
    // g_pmm_lock in pcp_drain_one cannot self-deadlock.
    extern cpu_t g_cpus[MAX_CPUS];
    extern void smp_call_function_single(uint32_t cpu, void (*fn)(void*), void* arg);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (!g_cpus[i].self) continue;         // CPU slot not initialised
        // self -> runs inline; remote -> IPI + wait, target runs it IRQs-off.
        smp_call_function_single(i, pcp_drain_self_cb, NULL);
    }
}

void pcp_stats_get(uint32_t cpu, pcp_stats_t* out) {
    if (cpu >= MAX_CPUS || !out) return;
    extern cpu_t g_cpus[MAX_CPUS];
    out->hits   = g_cpus[cpu].pcp_hits;
    out->misses = g_cpus[cpu].pcp_misses;
    out->drains = g_cpus[cpu].pcp_drains;
}
