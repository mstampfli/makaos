# SMP Known Races — Tracking

Every race / unsafe shared-state access that is **currently harmless on
UP** but **will be a bug the instant APs boot**.  Every entry lists
which phase is scheduled to fix it.  Phase 9 (AP bring-up) will not be
allowed to proceed until this list is empty.

Format:
- **Location** — file:symbol
- **Race** — what's unsafe
- **Why safe on UP** — why it doesn't matter today
- **Fix phase** — which phase is responsible
- **Fix strategy** — what the phase will do

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

## 2. Scheduler wait/run-queue lists

- **Location:** `kernel/proc/sched.c` — `s_sleep_head`, `s_heads[]`,
  `s_tails[]`, `s_zombie_head`, `s_reschedule`
- **Race:** `sched_wake`, `sched_sleep`, `sched_tick`, `enqueue`,
  `dequeue`, `sched_add_zombie`, `sched_reap_*` all manipulate these
  global lists with no synchronization.
- **Why safe on UP:** only one CPU → only one thread of execution can
  be in any of these functions at a time. `cli` on IRQ paths keeps IRQ
  handlers (like timer ticks) from interleaving.
- **Fix phase:** Phase 5 — per-CPU scheduler.
- **Fix strategy:** the entire scheduler becomes per-CPU. Run queues
  live in `cpu_t.rq`. Sleep list is per-CPU. Wake path sends the task
  to its home CPU via MPSC mailbox + IPI. `sched_tick` only touches
  the current CPU's state. Global lists cease to exist.

---

## 3. `task_idx` (pgid/tgid/sid) hash tables

- **Location:** `kernel/proc/sched.c` — `s_pgid_ht`, `s_tgid_ht`,
  `s_sid_ht` and their slot arrays
- **Race:** `task_idx_insert` / `task_idx_remove` / `task_idx_pgid_changing`
  / `task_idx_sid_changing` modify the hash tables with no lock.
  Readers (`signal_send_group`, `signal_send_pgrp`, `tty_get_ctty`)
  walk them without RCU protection.
- **Why safe on UP:** single thread of execution.
- **Fix phase:** Phase 6 — RCU-ize global tables.
- **Fix strategy:** same pattern as `s_pid_ht`. State bundled in a
  pointer published via `rcu_assign_pointer`. Readers get a zero-lock
  walk. Writers serialize on a per-table spinlock; grow drops the lock
  before `synchronize_rcu` to reclaim the old state.

---

## 4. Zombie list

- **Location:** `kernel/proc/sched.c` — `s_zombie_head`
- **Race:** `sched_add_zombie`, `sched_reap_zombie`, `sched_reap_child_zombie`
  manipulate the zombie linked list with no synchronization.
- **Why safe on UP:** same.
- **Fix phase:** Phase 5 (merged with scheduler rewrite).
- **Fix strategy:** zombies can live on the exiting task's home CPU
  until a waiter claims them. Parent's `wait()` searches the per-CPU
  zombie lists of its children's home CPUs (or uses a per-parent
  zombie list — TBD during Phase 5).

---

## 5. `sched_wake` cross-CPU preemption flag

- **Location:** `kernel/proc/sched.c:sched_wake` — `s_reschedule = 1`
- **Race:** setting a global reschedule flag from one CPU to force
  another CPU to context-switch doesn't make sense under SMP — the
  flag is per-CPU semantically.
- **Why safe on UP:** there's only one CPU, one flag is fine.
- **Fix phase:** Phase 5.
- **Fix strategy:** the flag moves into `cpu_t.reschedule_pending`
  (already declared in Phase 1). `sched_wake` sets the target CPU's
  flag and, if the target is idle, sends an IPI to wake it.

---

## 6. Hash-table writers that haven't been RCU-ized yet

- **Location:**
  - `kernel/net/arp.c` — `s_cache`
  - `kernel/net/socket.c` — `s_udp_slots`
  - `kernel/net/unix_sock.c` — `s_unix_ns`
  - `kernel/mm/shmem.c` — `s_namespace`
  - `kernel/net/tcp.c` — `s_pcb_head` list
  - `kernel/syscall/syscall.c` — epoll `s_pid_slots`-style grow paths
- **Race:** each of these has an insert/remove/grow path with no
  locking. Readers walk without RCU protection.
- **Why safe on UP:** single thread of execution.
- **Fix phase:** Phase 6 — RCU-ize global tables.
- **Fix strategy:** bulk-convert all read-mostly hash tables to the
  same RCU-published-state pattern. Shared helper macro so each table
  doesn't duplicate 100 lines of resize plumbing.

---

## 7. fd table reads vs. mutation

- **Location:** `kernel/proc/process.c` — `task_files_t.fd_table`
- **Race:** `fd_to_file` is called on every syscall that takes an fd.
  `sys_dup`, `sys_close`, `sys_open`, `sys_pipe`, `sys_socket`,
  `fd_table_grow` all mutate the array. No synchronization.
- **Why safe on UP:** single thread inside any given syscall at a time.
- **Fix phase:** Phase 8 — fd table + VFS + ext2 metadata.
- **Fix strategy:** per-`task_files_t` mutex for mutation, RCU-published
  pointer for the `fd_table` array. Read path stays a plain indexed
  load. Grow reallocates and publishes via `rcu_assign_pointer`.

---

## 8. VMA list reads vs. mutation

- **Location:** `kernel/mm/mm.c` — the per-`mm_t` VMA linked list
- **Race:** `sys_mmap`, `sys_munmap`, `sys_brk` add/remove VMAs. Page
  fault handler walks the list on every fault. No synchronization.
- **Why safe on UP:** fault and syscall can't overlap.
- **Fix phase:** Phase 8.
- **Fix strategy:** per-address-space mutex for mutation. Fault walk
  uses RCU (or a seqlock if the list becomes lookup-heavy).

---

## 9. ext2 bcache slot contents

- **Location:** `kernel/fs/ext2.c` — `s_bcache_data[slot]` and
  `s_bcache_tag[slot]`
- **Race:** cache readers memcpy from a slot while an I/O completion
  might be writing the same slot. Writers blindly overwrite the tag
  then the data.
- **Why safe on UP:** block I/O is serialized through the AHCI
  io_thread, and reads happen in caller context — they can't overlap
  with a write to the same slot without first yielding.
- **Fix phase:** Phase 7 — seqlock hot caches.
- **Fix strategy:** one seqlock per cache slot. Readers retry on a
  version mismatch. Writers (cache-miss fill) take the write side of
  the seqlock around the tag + memcpy.

---

## 10. TCP PCB list walk vs. alloc/free

- **Location:** `kernel/net/tcp.c` — `s_pcb_head`
- **Race:** `tcp_recv` walks `s_pcb_head` for every incoming segment
  while `tcp_pcb_alloc` / `tcp_pcb_free` mutate the list.
- **Why safe on UP:** same.
- **Fix phase:** Phase 6.
- **Fix strategy:** RCU-ize the list. Walkers do an RCU read section;
  allocators push under a writer lock + `call_rcu` to defer free.

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

## 12. Signal delivery free-after-exit

- **Location:** `kernel/proc/signal.c:signal_send` and task_t access
- **Race:** `signal_send(t)` is called by `signal_send_pgrp` which
  walks the pgid list. If `t` exits between the list walk and the
  signal_send call, we'd touch a freed task_t.
- **Why safe on UP:** the pgid list walk is atomic wrt scheduling, so
  `t` can't exit mid-walk.
- **Fix phase:** Phase 6 (RCU-ize pgid/tgid/sid tables) +
  Phase 8 (task_t lifecycle management).
- **Fix strategy:** task_t free is deferred via `call_rcu` after
  removal from the pid_ht. All task lookups happen inside rcu_read_lock.
  A task obtained from a lookup is guaranteed valid until
  rcu_read_unlock.

---

## 13. `g_current` global

- **Location:** `kernel/proc/sched.c:g_current` — 438 call sites
- **Race:** `g_current` is a single global pointer set by `do_switch`.
  Under SMP every CPU has its own "current task" — a single global is
  meaningless.
- **Why safe on UP:** exactly one CPU, one current task.
- **Fix phase:** Phase 5.
- **Fix strategy:** `g_current` becomes a compatibility macro for
  `(this_cpu()->current)`. Since Phase 1 already mirrors the value
  into `this_cpu()->current` on every context switch, flipping the
  macro is mechanical and all 438 sites work unchanged.

---

## Exit criteria for Phase 9

Phase 9 (AP boot) cannot land until **every entry above** is resolved
or explicitly waived with justification.  Each phase takes a slice:

- Phase 3 resolves entries **1, 11**
- Phase 5 resolves entries **2, 4, 5, 13**
- Phase 6 resolves entries **3, 6, 10, 12**
- Phase 7 resolves entry **9**
- Phase 8 resolves entries **7, 8, 12 (task lifecycle)**

After Phase 8 finishes, re-audit this file.  Any entry still open is a
blocker for Phase 9.
