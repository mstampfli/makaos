# SMP Known Races — Tracking

Every race / unsafe shared-state access that is **currently harmless on
UP** but **will be a bug the instant APs boot**.  Every entry lists
which phase is scheduled to fix it.  Phase 9 (AP bring-up) will not be
allowed to proceed until this list is empty.

> **Phase 9 status (2026-04-15):** APs are online.  Commits `b675076`
> (9-1), `0b00a84` (9-2), `45f0be0` (9-3), `65247a9` (9-4a), `9687842`
> (9-4b/c) landed in that order.  Under `-smp 4`, all four CPUs reach
> `cpu_init_ap` and the BSP runs the full userland.  APs still do NO
> work — they spin in a `pause`-loop that bumps `rcu_qs_count` only,
> so the scheduler has never scheduled on anything but CPU 0.
>
> This means the entries below that talk about cross-CPU data-plane
> races have been **tested against AP-existence** (memory barriers,
> atomics, cache-line ownership) but NOT against **multi-CPU
> execution**.  Phase 9-5 (per-CPU idle task + AP timer + IPI
> reschedule) is the commit that first actually runs user-visible
> work on APs.  Re-verify this file once 9-5 lands.

Format:
- **Location** — file:symbol
- **Race** — what's unsafe
- **Why safe on UP** — why it doesn't matter today
- **Fix phase** — which phase is responsible
- **Fix strategy** — what the phase will do

---

## 0. PMM / kheap all internal state — RESOLVED (Phase 4 bridge)

- **Location:** `kernel/mm/pmm.c`, `kernel/mm/kheap.c`
- **What was unsafe:** every buddy free list, slab cache head, per-frame
  refcount, per-frame pincount, slab tracker array. Zero synchronization
  before Phase 4.
- **Fix:** global `g_pmm_lock` spinlock (IRQ-safe) wraps every public
  PMM mutating entry point. Read-only queries stay lock-free on
  aligned loads. See `docs/PHASE4_REDESIGN.md` for the per-CPU
  magazine plan that eventually replaces this bottleneck.
- **Status:** SMP-correct, but single-lock contention under multi-CPU
  load. Acceptable bridge until Phase 4 proper lands.

---

## 1. `irq_wait` waiter list head — RESOLVED (Phase 3)

- **Location:** `kernel/arch/x86_64/irq_wait.c`
- **Fix:** `s_head[irq]` linked list replaced with
  `s_wq[irq]` of `wait_queue_t`. Waiters stack-allocate a
  `task_we_t` and push onto the queue via the lock-free MPSC CAS.
  `irq_notify` calls `wait_queue_wake_all` which detaches the whole
  chain with one xchg.  Same-CPU races covered by `cli`; cross-CPU
  races covered by the atomic primitives in the wait queue itself.

---

## 2. Scheduler wait/run-queue lists — RESOLVED (Phase 5)

- **Fix:** all of `s_heads[]`, `s_tails[]`, `s_sleep_head`,
  `s_zombie_head`, and `s_reschedule` are gone.  Their contents moved
  into `cpu_t.rq` (a `cpu_rq_t` struct with per-CPU run queues, sleep
  list, and zombie list) and `cpu_t.reschedule_pending`.  Every access
  goes through the owning CPU's `rq_lock` (IRQ-safe spinlock).
- **Cross-CPU wake:** `sched_wake(t)` takes `g_cpus[t->home_cpu].rq_lock`
  directly and enqueues on the target CPU's run queue.  Sets
  `reschedule_pending` on the target.  TODO Phase 9-5: if target CPU
  is idle/halted, send `VEC_IPI_RESCHEDULE` so the target notices
  without waiting for its next timer tick.  Until 9-5 lands, a task
  enqueued onto an AP's run queue would sit there forever because APs
  have no timer and no IPI handler yet — this is why `sched_add()`
  still pins to CPU 0.
- **Single lock, SMP-safe.**  On one CPU the lock is always
  uncontended (`lock cmpxchg` = ~20 cycles).  Under SMP the lock
  serializes only the few ops that touch the target CPU's rq, which
  is by construction a small fraction of scheduler work (most is
  enqueue_on / dequeue_local on the running CPU).

---

## 3. `task_idx` (pgid/tgid/sid) hash tables — RESOLVED (Phase 6)

- **Location:** `kernel/proc/sched.c` — `s_pgid_ht`, `s_tgid_ht`, `s_sid_ht`
- **Fix:** each `task_idx_t` now carries its own IRQ-safe spinlock.
  All insert/remove and `task_idx_*_walk` operations acquire that lock
  for the duration; writers hold it across slot mutation, walkers hold
  it across the full callback invocation so concurrent insert/remove
  cannot tear the doubly-linked bucket list.  `signal_send_group` and
  `signal_send_pgrp` now use the new `task_idx_tgid_walk` /
  `task_idx_pgid_walk` APIs, which are O(threads-in-bucket).
- **Why spinlock and not full RCU:** signal delivery is not on the
  per-packet hot path; the bucket walks are microsecond-scale and the
  lock is uncontended in practice.  The RCU-COW machinery used by the
  network tables would add complexity with no measurable win here.

---

## 4. Zombie list — RESOLVED (Phase 5)

- **Fix:** each CPU has its own `cpu_t.rq.zombie_head`.  When a task
  exits, `sched_add_zombie` puts it on ITS home CPU's zombie list
  under that CPU's `rq_lock`.  `sched_reap_*` walks every CPU's
  zombie list (also under the per-CPU lock) to find a match.  Under
  SMP this means the reaper does `num_cpus` lock acquisitions in
  the worst case — acceptable because wait()/waitpid() are cold path.

---

## 5. `sched_wake` cross-CPU preemption flag — RESOLVED (Phase 5)

- **Fix:** the flag lives in `cpu_t.reschedule_pending`.  `sched_wake`
  sets it on the TARGET CPU's cpu_t, under that CPU's rq_lock,
  after enqueuing the woken task on that CPU's run queue.
- **TODO Phase 9:** if target is halted in idle, send an IPI so the
  target CPU context-switches immediately instead of waiting for its
  next tick.

---

## 6. Hash-table writers — RESOLVED (Phase 6)

All of these are now lock-free on the read path via QSBR RCU.  The
whole table (not individual slots) is wrapped in a heap-allocated
object whose pointer is published via `rcu_assign_pointer`.  Writers
serialise on a small writer spinlock, build a fresh table via
copy-on-write, publish, and `call_rcu` the old table.  Readers do a
preempt-disable + plain pointer load + hash walk — zero atomics, zero
cache-line bouncing, per-packet cost is a single memory load.

- `kernel/net/arp.c` — `s_arp` (was `s_cache`).  `arp_lookup` (called
  on every outgoing packet) is lock-free.
- `kernel/net/socket.c` — `s_udp` (was `s_udp_slots`).  `udp_table_find`
  (called per UDP packet in `socket_deliver_udp`) is lock-free and
  wrapped in `rcu_read_lock()` that also covers the RX enqueue, so
  the socket cannot be freed under the delivery path.  `socket_t`
  teardown is `call_rcu`-deferred via `sock_free_rcu`.
- `kernel/net/unix_sock.c` — `s_unix_ns` (was an open-addressing array).
  `unix_sock_close` defers the full teardown via `call_rcu`, so
  concurrent `connect`/`sendto` readers inside `rcu_read_lock()` see a
  consistent sock until they exit the reader section.
- `kernel/mm/shmem.c` — `s_namespace`.  `shmem_t.refcount` is now
  atomic; `shmem_ns_find` bumps the refcount via `shmem_tryget` inside
  `rcu_read_lock()` so the object cannot be freed under the caller.
  Final teardown runs from `shmem_free_rcu` after a grace period.
- `kernel/net/tcp.c` — `s_pcb_head` linked list.  `tcp_recv` and
  `tcp_timer_tick` walk under `rcu_read_lock()`; `tcp_pcb_free` unlinks
  then `call_rcu`s the pcb destruction.

See LOCKS.md entries for exact writer-lock / call_rcu usage.

---

## 7. fd table reads vs. mutation — RESOLVED (Phase 8A)

- **Fix:** `task_files_t` now holds a pointer to a separately-allocated
  `fdtable_t { vfs_file_t** fd_table; uint32_t* fd_flags; uint32_t cap }`
  published via `rcu_assign_pointer` and loaded on the read side with
  `__atomic_load_n(..., ACQUIRE)`.  `fd_table_grow` allocates a fresh
  `fdtable_t`, copies the live slots, installs the new pointer, and
  hands the old `fdtable_t` to `call_rcu(fdtable_free_rcu, old)` so
  any in-flight reader that already loaded the old pointer stays
  valid until the grace period ends.

  `vfs_file_t.refcount` is now atomic (`__atomic_fetch_add/sub`),
  with a new `vfs_tryget` (CAS-if-nonzero) so a reader that observes
  a slot can bump the refcount safely even against a concurrent last-
  close.  The hot lookup is a new helper `fdget(fd)` which does
  `rcu_read_lock` → acquire-load `ft` → bounds check → load slot →
  `vfs_tryget` → `rcu_read_unlock`.  Callers drop the reference with
  `fdput(f)` (aliased to `vfs_close`).  Writer paths (`sys_dup`,
  `sys_dup2`, `sys_close`, `sys_pipe`, `fd_install`, `sys_fcntl`
  F_DUPFD / F_GETFD / F_SETFD, `sys_open`, `sys_shm_open`, CLOEXEC
  sweep in exec) serialise on a per-`task_files_t` writer spinlock.

  Hot path cost on x86: `lock xadd` (pin), single fence, tag load,
  `lock cmpxchg` for tryget, `lock xadd` on release.  No spinlock on
  the read path.

---

## 8. VMA list reads vs. mutation — RESOLVED (Phase 8B)

- **Fix:** `mm_t` now holds a writer-side spinlock `vma_lock` and
  `mm->vmas` is an RCU-protected singly-linked list.  Readers
  (`mm_vma_find`, page fault handler, `/proc/<pid>/maps`, user buffer
  prefault) walk via `rcu_dereference(v->next)` inside `rcu_read_lock()`
  with zero atomics on the hot path.  Writers (`mm_vma_add`,
  `mm_vma_remove`, `mm_vma_find_free`, `sys_brk`, `sys_mmap` MAP_FIXED
  unmap, `sys_munmap`) serialise on `mm->vma_lock` and publish list
  updates via `rcu_assign_pointer`.  Removed VMAs are handed to
  `call_rcu(vma_free_rcu)` for deferred reclamation so a concurrent
  page-fault walker dereferencing the old `->next` stays valid until
  the grace period ends; `vma_free_rcu` also drops the shmem ref.
  In-place trims (cases 2/3 of `mm_vma_remove`) are safe plain-field
  stores because readers only observe bounds via `addr >= start &&
  addr < end`.  Split (case 4) builds the right fragment privately
  and publishes it via `rcu_assign_pointer(v->next, right)` before
  shrinking the left half.  `mm_clone` walks the src list under RCU,
  snapshots each VMA into a stack array, then drops the reader before
  calling `mm_vma_add` on the dst — so no cross-mm lock ordering is
  required.  Page-fault demand-page path snapshots `vma->flags`,
  `vma->shmem`, `vma->shmem_pgoff` inside the reader section (using
  `shmem_tryget` to pin the shmem across the subsequent physical
  work) so the actual `vmm_page_map` / `shmem_get_page` calls run
  with no RCU or spinlock held.

---

## 9. ext2 bcache slot contents — RESOLVED (Phase 7 + 7.2)

- **Phase 7.2 fix (zero-copy):** per-slot meta is now
  `{ spinlock_t wlock; uint32_t tag; uint32_t pin; }`.  Hot readers call
  `bcache_get(blk, scratch)` which does an optimistic
  atomic-fetch-add on `pin`, a full fence, and a tag load — on a hit
  that returns a `const uint8_t*` straight into the cache slot **with
  no memcpy** and the caller pins the slot until `bcache_put`.
  Writers (`bcache_fill`, called on miss-fill and write-through) use
  `trylock` + pin-then-tag-invalidate + pin-recheck, bailing silently
  on slot conflict.  On mismatch the reader falls back to `ahci_read`
  into the caller's scratch, then opportunistically publishes into
  the cache.  ext2 hot paths (`inode_get_block`, `dir_lookup`,
  `read_bgd`, `read_inode`, `ext2_vfs_read` partial/multi-block,
  `ext2_readdir`) now read the 4–128 bytes they actually need out of
  a pinned slot instead of paying a 4 KiB memcpy per block lookup.
  Bitmap walks and RMW paths still go through the `read_block`
  compat shim because they need a mutable working copy.

---

## 10. TCP PCB list walk vs. alloc/free — RESOLVED (Phase 6)

- **Fix:** `s_pcb_head` is now an RCU-protected singly-linked list.
  `tcp_recv` and `tcp_timer_tick` walk the list inside `rcu_read_lock()`
  using `rcu_dereference` on each `->next`.  `tcp_pcb_alloc` publishes
  at the head via `rcu_assign_pointer` under `s_pcb_wlock`.  `tcp_pcb_free`
  unlinks under the writer lock and defers the full teardown (txbuf,
  rxbuf, struct) to `tcp_pcb_free_rcu` via `call_rcu`, so any in-flight
  reader still dereferencing the pcb sees a live object until its
  reader section ends.

---

## 11. Pipe / PTY / TTY / unix / socket single-waiter fields — RESOLVED (Phase 3)

- **Locations (all now using lock-free MPSC wait queues):**
  - `kernel/fs/pipe.c` — `pipe_buf_t.reader` removed; uses
    read_file->waitq / write_file->waitq
  - `kernel/drivers/tty/tty.c` — `tty_t.reader` removed; uses tty->waitq
  - `kernel/drivers/tty/pty.c` — `pty_t.m_reader` removed; uses
    master_waitq and slave.waitq
  - `kernel/net/unix_sock.c` — `unix_sock_t.waiter` removed; new
    `wait_queue_t waitq` field
  - `kernel/net/socket.c` — `socket_t.waiter` removed; new
    `wait_queue_t waitq` field
- **Fix:** every blocking caller now stack-allocates a `task_we_t`,
  registers it on the relevant wait_queue_t, re-checks the condition
  under the registration (closes lost-wakeup window), then sleeps.
  Writers call `wait_queue_wake_all` which atomically drains the
  chain and fires each entry's callback.
- **SMP safety:** push is lock-free CAS, drain is lock-free xchg;
  cancellation (`wq_remove`) takes a tiny per-queue spinlock that is
  never held on the hot wake path.

---

## 12. Signal delivery free-after-exit — RESOLVED (Phase 6)

- **Fix:** `task_destroy` no longer frees the task synchronously.
  It removes from `pid_ht`, frees the pid slot, and then hands the
  task_t to `call_rcu(task_free_rcu, t)` which runs kstack/mm/files/
  kfree after a grace period.  `signal_send_group` / `signal_send_pgrp`
  walk the pgid/tgid hash buckets under the per-table spinlock, which
  prevents a concurrent `task_idx_*_remove` from splicing the bucket
  list mid-walk; any task the walker reached has already observed
  state != TASK_DEAD so its `rcu_read_lock`-equivalent bucket-lock
  window covers the signal_send call.

---

## 13. `g_current` global — RESOLVED (Phase 5)

- **Fix:** `g_current` is now a macro `(this_cpu()->current)` in
  `kernel/proc/sched.h`.  Storage lives in `cpu_t.current`; the global
  is deleted entirely.  Assignment and read sites continue to work
  unchanged (field access through a pointer is an lvalue), but every
  access now routes through `this_cpu()`, so cross-CPU reads naturally
  see their own current task.
- **vmm.c** was the only file with a stale `extern task_t* g_current;`
  forward declaration — replaced with `#include "sched.h"`.

---

## 15. ext2 metadata scratch is kmalloc-per-call — DEFERRED OPT

- **Location:** `kernel/fs/ext2.c` — every metadata helper that needs a
  4 KiB scratch buffer (read_bgd, write_bgd, inode_load, write_inode,
  free_block, free_inode_num, alloc_block, alloc_inode, dir_*) does
  `kmalloc(4096)` via `EXT2_SCRATCH_ALLOC`.
- **Cost today:** ~10–20 ns per metadata op for the slab pop.  Invisible
  next to a typical ms-scale ahci_read on a cache miss; on a bcache
  hit it's wasted because the scratch is never written.
- **Fix plan:** once Phase 4-proper lands per-CPU magazines / per-CPU
  scratch slots, change `EXT2_SCRATCH_ALLOC` to point at
  `this_cpu()->ext2_scratch` instead of going through kmalloc.  Zero
  alloc cost and all existing call sites get the speedup for free
  because they already use the macro.
- **Why deferred:** the bottleneck won't show up until APs are running
  and ext2 is hot.  We measure first, then optimise.  Phase 4-proper
  is itself deferred until post-Phase 9, so this rides on its coat-tails.
- **Not a Phase 9 blocker.**

---

## 14. ext2 superblock free-counter writeback — DEFERRED

- **Location:** `kernel/fs/ext2.c` — superblock `s_free_blocks_count` /
  `s_free_inodes_count` and the matching BGD per-group counters.
- **Status today:** the kernel reads the superblock at mount and never
  writes it back.  We update the in-memory BGD counters under the
  per-group lock (Phase 8C-2) but the on-disk superblock totals stay
  stale until next mount, which `fsck` would flag.
- **Why deferred:** correct mount/unmount + free-counter writeback is
  its own small project (sync points, ordering, crash safety) and is
  orthogonal to the SMP-safety work Phase 8 is about.  No ongoing
  in-memory race exists — readers just see slightly stale counts.
- **Fix phase:** later, post-Phase 9.  Track here so we don't forget.

---

## 16. ext2 metadata mutation — RESOLVED (Phase 8C)

- **Fix:** every per-inode RMW (write_inode tail in vfs_write, mkdir,
  unlink, rename, ext2_write_file overwrite/create) now goes through
  `inode_lock(ino)` → mutate `leaf->inode` → `inode_writeback(leaf)` →
  `inode_unlock(leaf)`.  The leaf carries a per-inode seqlock so
  readers (`irtree_get`) take zero atomics on a consistent snapshot
  and writers serialise per inode without contending across inodes.
- **dir_add_entry / dir_remove_entry** hold the parent directory's
  inode lock across the whole dirent walk so concurrent dirent
  mutations on the same directory cannot tear the rec_len chain.
- **alloc_inode / alloc_block / free_block / free_inode_num** now hold
  a per-block-group spinlock from a `s_group_locks[]` array allocated
  at mount time.  Two CPUs allocating in different groups never
  contend; two CPUs allocating in the same group serialise on that
  one group's lock.  The mkdir `bg_used_dirs_count++` update also
  takes the per-group lock.
- **ext2_rename** is wrapped in a single global `s_rename_lock`
  spinlock for the whole operation — same approach as Linux's
  per-superblock `s_vfs_rename_mutex`.  Rename is rare and avoiding
  the dual-parent lock-ordering complexity is worth a single
  uncontended spinlock acquire on the rename hot path.
- **Static `s_blk_buf` / `s_blk_buf2`** are gone — they were a latent
  SMP corruption vector (multiple CPUs writing the same 4 KiB BSS
  scratch).  Replaced with per-call `EXT2_SCRATCH` (kmalloc with the
  `__cleanup__` attribute auto-freeing on scope exit).

---

## 17. AP per-CPU CPU-state setup (CR0/CR4/MSRs) — RESOLVED (Phase 9-5)

- **Location:** `kernel/proc/cpu.c:cpu_init_ap`
- **What was unsafe:** Phase 9-4b/c brought APs up into `cpu_init_ap`
  but skipped the per-CPU CPU-state bring-up that kmain does on the
  BSP: `IA32_PAT`, `CR0.{EM,TS}`, `CR4.{OSFXSR,OSXMMEXCPT,SMEP}`, and
  `syscall_init()` (which sets `IA32_EFER.SCE`, `IA32_STAR`, `IA32_LSTAR`,
  `IA32_SFMASK`).  Every one of those is a per-CPU MSR/control register,
  not inherited from the BSP via CR3 or GDT.
- **Why it was latent in 9-4b/c:** APs never ran user tasks.  They
  sat in a pause-spin loop bumping `rcu_qs_count`.
- **Symptoms once APs started scheduling user tasks (Phase 9-5):**
  - Bash's first `movaps`/`xorps` on an AP → `#UD` → SIGILL because
    `CR4.OSFXSR` was zero, SSE was disabled, every SSE opcode illegal.
  - `syscall` instruction → `#UD` → SIGILL because `IA32_EFER.SCE=0`
    and `LSTAR` was NULL on the AP.
  - `context_switch`'s fxrstor silently no-ops without OSFXSR, so the
    incoming task's FPU state was whatever the AP happened to boot with.
- **Fix:** `cpu_init_ap` now mirrors the kmain CPU-feature bring-up
  before touching GS_BASE / TSS / IDT.
- **Status:** Resolved in Phase 9-5.

---

## 18. `do_switch` early-return missed `rcu_note_qs()` — RESOLVED (Phase 9-5)

- **Location:** `kernel/proc/sched.c:do_switch` preempted + empty-rq path
- **What was unsafe:** When `sched_preempt` fired from a timer IRQ on
  a CPU running a non-idle task with an empty rq, `do_switch(1)` took
  the lock, found nothing to dequeue, dropped the lock and returned
  **without calling `rcu_note_qs()`**.  The only call to `rcu_note_qs`
  in `do_switch` was on the post-context-switch path, which never
  executed for the "stay on current task" case.
- **Why it bit Phase 9-5:** `init_kthread` on the BSP returns from its
  bootstrap body and falls into `proc_trampoline.dead` (a `sti; hlt;
  jmp` spin).  It's always the "current task" on CPU 0 in that state.
  Every timer tick: `sched_tick` → `sched_preempt` → `do_switch(1)` →
  early-return → CPU 0's `rcu_qs_count` stays frozen at whatever value
  it had when init_kthread's last real QS happened.  Any other CPU
  calling `synchronize_rcu` then walks `[0, g_num_cpus)` waiting for
  CPU 0's counter to move, and hangs forever.  First-observed via
  `bash → sys_brk → mm_vma_remove → call_rcu → synchronize_rcu`
  blocked on CPU 0's qs counter.
- **Fix:** the preempted+empty-rq early-return now calls `rcu_note_qs()`
  before returning.  Safe because `sched_preempt` only reaches this
  call with `preempt_depth == 0`, which means the interrupted context
  cannot be inside an RCU reader section.
- **Status:** Resolved in Phase 9-5.

---

## 19. `synchronize_rcu` used `sched_yield` to busy-wait — RESOLVED (Phase 9-5)

- **Location:** `kernel/proc/rcu.c:synchronize_rcu`
- **What was unsafe:** the cross-CPU wait loop called `sched_yield()`
  between counter re-checks.  On UP `num_cpus() == 1` so the loop
  never executes; under SMP it would attempt to yield on an otherwise-
  idle CPU and **never return**: `sched_yield` → `do_switch(0)` with an
  empty rq → switch to idle → idle hlts forever because nothing is
  enqueued on this CPU's rq.  The while-predicate is never re-checked.
- **Fix:** replaced `sched_yield()` with `cpu_relax()`.  Timer IRQs
  and IPIs still interrupt the spin freely, and the target CPU's QS
  counter is advanced by its own `sched_tick` / context switch / idle
  loop entirely independently of us.
- **Status:** Resolved in Phase 9-5.

---

## 20. Idle task address space left stale CR3 loaded — RESOLVED (Phase 9-5)

- **Location:** per-CPU idle task in `kernel/proc/sched.c` (`s_idle_mm`)
- **What was unsafe:** `s_idle_mm.pml4_phys = 0` originally, which made
  `context_switch` skip the CR3 reload when switching INTO idle
  (`cmp rdx, rdx; jz .skip_cr3`).  On UP this was fine — the BSP kept
  using the previous mm's PML4 until the next real task was scheduled,
  and nothing ever freed a PML4 while it was installed in CR3.  Under
  SMP a user task's exit path frees its PML4 via `task_free_rcu`
  ONE grace period after sched_add_zombie; any CPU that then switched
  to idle without reloading CR3 was running kernel code on a DEAD
  PML4.  Any kernel data access → `#PF` to unmapped memory → kernel
  panic.
- **Fix:** `sched_init_idle_for_cpu` sets `s_idle_mm.pml4_phys =
  vmm_kernel_pml4_get()` (lazily, before allocating the first idle
  task_t).  Every context_switch into idle now reloads CR3 to the
  kernel PML4, which has no user mappings to go stale.
- **Status:** Resolved in Phase 9-5.

---

## 21. Per-CPU idle task must not be a shared singleton — RESOLVED (Phase 9-5)

- **Location:** scheduler idle-task construction
- **What was unsafe:** pre-9-5 code used a single static `s_idle`
  task_t referenced by every CPU's `cpu_t.idle`.  Under SMP two CPUs
  simultaneously switching INTO or AWAY FROM idle would fxsave/fxrstor
  the same 512-byte buffer concurrently (`ctx.fxsave_buf`) and step on
  each other's kstack (`s_idle_stack`).  Result: FPU state corruption
  + random kstack frame overwrites + task_t lifetime bugs when
  `task_free_rcu` ran on a zombie whose ctx was being fxsaved from
  another CPU.
- **Fix:** `sched_init_idle_for_cpu(id)` kmallocs a fresh `task_t` for
  every CPU, builds a proper proc_trampoline / cpu_idle_loop entry
  frame on its own `kstack_alloc()`d stack, and captures a valid FPU
  baseline via `fxsave` before publishing.  BSP and each AP call it
  exactly once.
- **Status:** Resolved in Phase 9-5.

---

## 22. `fb_term_putc` used `preempt_disable` instead of a lock — RESOLVED (Phase 9-5)

- **Location:** `kernel/drivers/video/fb.c`
- **What was unsafe:** `fb_term_putc` / `fb_term_write` serialised via
  `preempt_disable` / `preempt_enable`.  On UP that's sufficient (only
  one CPU, preempt off → no other task runs).  On SMP it is useless:
  preempt_disable only blocks local task switching; a remote CPU can
  enter `fb_term_putc` freely and race on `g_fb_col/row` + the pixel
  buffer + the `fb_term_scroll` memcpy.
- **Observed:** intermittent bash prompts that rendered sometimes and
  not other times under `-smp 4`, depending on whether makaterm /
  bash / init were concurrently writing to the fb.
- **Fix:** replaced with a spinlock (`g_fb_lock`, see LOCKS.md entry).
  IRQ-safe so the panic-print path from exception handlers can also
  use `fb_term_putc` without deadlocking.  `fb_term_write` takes the
  lock once for the whole buffer, not per-byte — hot path cost is
  one lock per `write(1, ...)` syscall.
- **Status:** Resolved in Phase 9-5.

---

## 23. Lost-wakeup pattern: `if (state == SLEEPING) sched_wake()` — RESOLVED (Phase 9-5)

- **Location:** every wait-queue user across `tty/`, `pty/`, `net/`,
  `fs/pipe.c`, `proc/signal.c`, `ahci/`, `syscall/syscall.c` (wait,
  epoll, poll), `irq_wait`, `evdev`, etc.  And the kernel-internal
  `signal_send`.
- **What was unsafe:** Every sleeper used the pattern
  `while (!cond) sched_sleep();`.  Every waker used
  `if (t->state == TASK_SLEEPING) sched_wake(t);` — or worse, set cond
  and called sched_wake unconditionally without taking any lock.  The
  classic race:
  ```
  CPU A (sleeper):                 CPU B (waker):
    check cond → false
                                     set cond = true
                                     sched_wake(t)
                                       sees t.state != SLEEPING → DROPS
    sched_sleep → state=SLEEPING
    ...never woken again
  ```
- **Fix (Phase 9-5, core scheduler-level):**
  - Added `task_t.wake_pending` (u8) flag, owned by the task's home
    CPU's `rq_lock`.
  - `sched_wake` always takes the target's rq_lock.  If the target
    is not yet `TASK_SLEEPING`, instead of dropping the wake it sets
    `wake_pending = 1`.
  - `sched_sleep` checks `wake_pending` under the same rq_lock AFTER
    arriving in the lock and BEFORE committing to sleep.  If set,
    clears it and returns without sleeping — the caller's outer loop
    will re-check the condition and see the updated state.
  - `signal_send` now always calls `sched_wake(t)` (never `if state ==
    SLEEPING`).  sched_wake's lock + state check handles every racy
    RUNNING↔SLEEPING transition correctly.
  - `signal_deliver_pending`'s death path also sends SIGCHLD to the
    parent via `signal_send` instead of only conditionally waking.
- **Status:** The core scheduler primitive is fixed.  Every existing
  wait queue automatically benefits from it because the wake goes
  through `sched_wake`.  See entry #24 for the remaining per-subsystem
  audit work deferred to Phase 9-6.

---

## 24. Cross-CPU wait queues — per-subsystem audit — RESOLVED (Phase 9-6)

- **Status:** Phase 9-6 audited and fixed every cross-CPU sleep/wake
  path listed below.  `pick_home_cpu()` now round-robins across every
  online CPU (Phase 9-6g).  Every user task and kthread dispatched
  via `sched_add` lands on a CPU chosen by a simple atomic cursor;
  AHCI, pty/tty, pipe, unix socket, waitpid, and epoll all survive
  the resulting cross-CPU traffic.
- **Resolutions:**
  - **waitpid / zombie reap (Phase 9-6a, commit ec2c276)**: the
    per-task `children` list is now a lock-free Treiber stack.
    Producers (`task_child_add`, `task_child_add_chain`) CAS-prepend
    with `__ATOMIC_RELEASE`; the reaper (`task_children_reap`) does
    an `xchg`-drain with `__ATOMIC_ACQUIRE`, walks the private chain
    for a matching zombie, and CAS-splices the survivors back.
    `sys_exit` now uses `task_children_reparent` which snapshots
    its own children list and splices the whole chain onto init in
    one CAS — O(1) atomics per mass-exit instead of O(orphans).
    All dead legacy helpers (`sched_reap_child_zombie`,
    `sched_has_child_zombie`, `sched_wait_pid`, `sched_poll_pid`,
    `task_child_remove`) deleted.
  - **pty / tty (Phase 9-6b, commit c4c30b4)**: every
    sleeper/waker pair verified canonical.  `tty_vfs_read` now has
    a phase-1 fast check so it doesn't always `task_we_add` when
    data is already present.  All four waiters (master_waitq,
    tty->waitq, slave_drain_waitq, tty_vfs_read) remove their
    stack entry on every exit path including EINTR.  Wakers commit
    the ring push BEFORE `wait_queue_wake_all` throughout.
  - **epoll has_ready flag (Phase 9-6c, commit 2504e98)**:
    `epoll_wake_func`'s `has_ready=1` is now an `__ATOMIC_RELEASE`
    store.  `sys_epoll_wait` clears with RELAXED before `task_we_add`
    (so the CAS-release carries the clear) and re-reads with
    `__ATOMIC_ACQUIRE`.  The full poll re-scan under `state->lock`
    stays as the tiebreaker.  No new lock, no new smp_mb.
  - **pipe (Phase 9-6d, commit 46d89fd)**: `pipe_read` and
    `pipe_write` now return `-EINTR` (or the partial byte count so
    far) when `signal_has_actionable` fires.  Pre-9-6d they were
    effectively uninterruptible because the outer loop just
    re-checked and re-slept through every signal.  Commit-before-
    wake ordering already correct.
  - **unix socket (Phase 9-6e, commit 927624b)**: all six
    `sched_sleep` sites (accept, connect, send, recv stream, recv
    dgram, recvfd) now call `signal_has_actionable` after
    `task_we_remove` and return `-EINTR` (or NULL for accept/recvfd
    where the API can't carry an errno).  Canonical pattern was
    already in place.
  - **AHCI rendezvous (Phase 9-6f, commit 953cb7a)**: pre-9-6f
    submission was a fixed-size ring with multi-producer writes
    to `s_req_tail` (genuine SMP bug) and a `sched_yield` busy-wait
    on ring-full (hard-rule violation).  Replaced with a lock-free
    MPSC of stack-allocated `ahci_req_t` — submitters CAS-prepend
    on `s_req_head`, the kthread `xchg`-drains, reverses for FIFO,
    and processes each entry.  `r->done` is now
    `__ATOMIC_RELEASE` / `__ATOMIC_ACQUIRE` paired.
- **Placement flip (Phase 9-6g):** `pick_home_cpu()` returns
  `__atomic_fetch_add(&cursor, 1) % g_num_cpus`.  Every task
  dispatched via `sched_add` lands on whichever CPU the cursor
  points at; no kthread pinning because every kthread path is
  CPU-agnostic (ahci_io_thread, virtio_net, keyboard/mouse,
  irq_wait waiters all communicate via shared state + wait queues,
  not per-CPU registers).
- **Not a blocker for 9-7**: Phase 9-7 (TLB shootdown) and 9-8
  (work stealing) can proceed without further wait-queue work.
- **Locks added by Phase 9-6:** zero.

---

## Exit criteria for Phase 9

- Phase 3 resolves entries **1, 11**
- Phase 5 resolves entries **2, 4, 5, 13**
- Phase 6 resolves entries **3, 6, 10, 12**
- Phase 7 resolves entry **9**
- Phase 8A/8B resolve entries **7, 8, 12 (task lifecycle)**
- Phase 8C resolves entry **16** (ext2 metadata)
- Phase 9-5 resolves entries **17, 18, 19, 20, 21, 22, 23**
- Phase 9-6 resolves entry **24** (per-subsystem wait-queue audit
  + lock-free children list + lock-free AHCI MPSC + round-robin).
- **14 (superblock writeback) and 15 (ext2 scratch per-CPU) are
  explicitly deferred** with the rationale above and are not Phase 9
  blockers.

## Phase 9-6 runtime status (2026-04-15)

Committed state:
- All 4 CPUs boot online under `-smp 4`.  APs initialise their own
  PAT, CR0/CR4, syscall MSRs, TSS, IDT, LAPIC, run a per-CPU idle
  task on a private kstack, and respond to IPIs (reschedule / call /
  tlb-flush-stub).
- `pick_home_cpu()` round-robins across every online CPU.  New
  tasks (user and kernel) land on CPU `(cursor++) % g_num_cpus`
  via `__atomic_fetch_add`.
- Every cross-CPU wait-queue path has been audited (see entry 24
  for the per-subsystem summary).  Zero new locks were added by
  Phase 9-6; `children_list` became a lock-free Treiber stack and
  the AHCI submission ring became a lock-free MPSC of stack-
  allocated requests.
- Reproducer: `userland/apps/smp_test` exercises waitpid, pipe,
  epoll, AF_UNIX, and parallel AHCI reads end-to-end with
  deterministic pass/fail.
- Next: Phase 9-7 (TLB shootdown) and 9-8 (work stealing + load
  balance).
