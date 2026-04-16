#pragma once
//
// ── TLB shootdown (Phase 9-7) ───────────────────────────────────────────
//
// Keeps TLB state coherent across CPUs that share an address space.  When
// a mapping is torn down or tightened (munmap, mprotect drop-perms, CoW
// break, exit), every CPU holding the affected pml4 in its CR3 needs to
// invalidate the corresponding TLB entries before the sender can assume
// the virtual address is safe to reuse.
//
// Targeted, not broadcast: the sender walks `task_mm_cpumask_read(mm)`
// which tracks exactly the CPUs whose CR3 currently points at this mm's
// pml4 (see process.h cpumask helpers).  CPUs outside that mask have a
// different address space loaded and need no action — mov-to-cr3 on the
// switch-away already flushed their TLB of this mm's entries.
//
// Lazy TLB for kernel threads: kthreads set no bit in the mask because
// they run in the previously-loaded pml4 without holding any user
// mappings themselves.  Any pending shootdown against that user mm will
// have targeted the CPU directly via its real bit, so the kthread
// detour adds no extra work.
//
// ── API ─────────────────────────────────────────────────────────────────
//
// tlb_flush_range:   invalidate [start, end) for `mm` on every CPU
//                    currently holding it, plus self.  Blocks until all
//                    targets ACK; cheap when mm is only on one CPU.
// tlb_flush_mm:      full flush for `mm` — sends the IPI with end==0,
//                    receivers do `mov cr3, cr3` (full non-global flush).
//
// Both are safe to call with preemption enabled; they disable it for the
// duration so the sender doesn't migrate mid-shootdown.  Neither takes a
// lock — senders run in parallel, each publishing its own stack-
// allocated descriptor to per-target MPSC queues.

#include "process.h"

void tlb_flush_range(task_mm_t* mm, uint64_t start, uint64_t end);
void tlb_flush_mm   (task_mm_t* mm);

// IPI handler entry — wired from ipi.c ipi_tlb_flush_handler().
// Runs on the target CPU; drains its shootdown MPSC queue and executes
// each descriptor, publishing done back to the sender.
void tlb_shootdown_drain(void);
