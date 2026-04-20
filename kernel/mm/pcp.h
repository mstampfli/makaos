#pragma once
#include "common.h"

// ── Per-CPU pageset (Phase 4E) ─────────────────────────────────────────
//
// A per-CPU cache of recently-freed order-0 physical pages.  pmm_buddy
// allocations of order 0 (the overwhelming majority — every demand-page,
// every CoW break, every small kmalloc-via-bypass, every page table
// page, every PCB, every fd_install slot) hit this layer first.  Only
// on cold pcp does the call fall through to pmm_buddy_alloc_locked.
//
// Layout: 16-byte aligned `pcp_hdr_t {count, _pad, tid}` per CPU,
// guarded by lockless cmpxchg16b on `%gs:pcp_hdr`.  Backing storage
// is a per-CPU `phys_addr_t pcp_pages[SLAB_PCPU_PCP_DEPTH]` indexed
// by `count`.  See cpu.h for the struct definitions.
//
// Migration safety (same argument as slab_pcpu): the cmpxchg16b
// resolves `%gs:offset` at instruction execution time.  A migration
// between LOAD16 and CMPXCHG16B leaves the new CPU's pcp_hdr with a
// different tid → CAS fails → retry on the new CPU.  The interim
// write to pcp_pages[count] (push) or read from pcp_pages[count-1]
// (pop) on the old CPU is harmless: the old CPU's count didn't
// advance (we never CAS-committed there), so any subsequent push on
// that CPU overwrites the slot before reading it.
//
// Order ≥ 1 allocations bypass the pcp entirely — they're rare and
// coarse, the buddy lock is fine for them.

phys_addr_t pcp_alloc(void);                // returns PMM_INVALID_ADDR on OOM
void        pcp_free(phys_addr_t phys);
void        pcp_drain_all(void);            // shrinker (4F) hook

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t drains;
} pcp_stats_t;

void pcp_stats_get(uint32_t cpu, pcp_stats_t* out);
