# Concurrency / safety audit findings (2026-06-24)

Multi-subsystem audit of the MakaOS kernel for data races, lost wakeups,
use-after-free, refcount races, and memory-safety bugs. Each item lists the
mechanism and the confirmation method. FIXED items have a regression test or a
verified clean boot; TODO items are confirmed by code reading and await a fix.

## FIXED

### F1. kmalloc big-alloc header overflow (`kernel/mm/kheap.c`) — commit pending
Big (buddy-backed) `kmalloc` sized the block with `size + 8` but returned
`raw + 16` (and `kfree` used `-16`), so a request in the band
`(2^k - 16, 2^k - 8]` over-ran its buddy block by up to 8 bytes (e.g.
`kmalloc(8184)` got 8176 usable). Fix: a single `KMALLOC_BIG_HDR == 16`
constant drives the sizing, the payload offset, and the free offset, plus a
`size + header` overflow guard. **Test:** `kheap_overflow_selftest`
(`SELFTESTS=1`) verifies every big allocation's usable span >= request and
prints the pre-fix `size+8` shortfall — confirmed FAIL pre-fix, PASS post-fix.

### F2. Chase-Lev deque idle-steal IRQ race (`kernel/proc/sched.c`) — commit pending
`cpu_idle_loop` -> `sched_try_steal` did an owner-side `chaselev_push` on the
LOCAL deque with IRQs enabled and no lock, while this CPU's `sched_tick` (timer
IRQ) does owner-side `chaselev_push`/`chaselev_pop` on the SAME deque under
`rq_lock`. A timer IRQ landing mid-push tears `bottom` -> a task lost or made
dispatchable twice (two CPUs into one kstack -> panic). Fix: take
`self->rq_lock` (irqsave) around the steal's local push, matching the lock the
IRQ side already uses (irqsave also masks the same-CPU IRQ, so no deadlock).
**Confirmation:** code-level (the single-owner contract is violated); existing
`chaselev_selftest` still PASSES and SMP boot is healthy post-fix.

## TODO — confirmed, high severity

### T1. dcache resurrect-from-zero UAF (`kernel/fs/dcache.c:236` vs `:467`)
`dcache_lookup` bumps `refcount` 0->1 with a bare `__atomic_fetch_add`, no
tryget. The shrinker reads `refcount == 0` under `g_dcache_wlock` and frees
(RCU-deferred). A lookup that resurrects a dentry the shrinker already committed
to free returns a pointer that gets freed after the grace period -> UAF +
refcount underflow in `dcache_put`. NOTE: unlike the fd path, here `refcount==0`
is the *normal alive* state, so the fix is NOT a plain `vfs_tryget` (which fails
from 0). Needs a DYING sentinel: shrinker CAS `0 -> DYING` (skip on fail);
lookup CAS-bumps from any non-DYING value (treat DYING as a miss). Highest
reachability (refcount-0-while-hashed is every warm cache hit).

### T2. CoW/fork PTE + refcount race (`kernel/mm/vmm.c` ~543-563 vs ~682-716)
`vmm_clone_user_ex` writes the parent PTE (drop WRITABLE) + `pmm_ref_inc`
WITHOUT `vma_lock`, while the CoW-break fault handler does its read-check-act
under `vma_lock`. They sync on different things, so a sibling thread faulting a
page mid-clone can take the `rc==1` "sole owner" no-copy branch -> parent and
child PTEs alias one frame (cross-address-space corruption) + skewed refcount ->
later double `pmm_ref_dec` frees a mapped frame (UAF). In-tree comments
(`process.c:420`, `vmm.c:670`) document the victims. Fix: hold `src_mm->vma_lock`
across the clone's parent-PTE walk.

### T3. AF_UNIX UAF: close frees vfs_file_t while peer sends (`kernel/net/unix_sock.c`)
`unix_sock_close` `kfree`s the `vfs_file_t` synchronously (line ~321) while only
RCU-deferring the `unix_sock_t`. The peer's `unix_sock_send_ex` (STREAM, no
`rcu_read_lock`) captures `peer = s->peer` and calls `unix_poll_wake(peer)` ->
derefs the freed `peer->file->waitq` -> UAF. Everyday Wayland disconnect
pattern. Fix: `rcu_read_lock` around the send/wake, and defer the `vfs_file_t`
free (or null `s->file` + DISCONNECT the peer synchronously under a lock).

### T4. TCP PCB fields mutated lock-free from 3 CPUs (`kernel/net/tcp.c`)
`tcp_timer_tick` retransmit (`snd_nxt = snd_una` rewind), `tcp_send`
(`txbuf_used += chunk`), and `tcp_recv` ACK (`txbuf_used -= acked`,
`txbuf_head += acked`, `snd_una = ack`) RMW the same non-atomic PCB fields from
different CPUs with no per-PCB lock. Torn `snd_nxt` mis-sends; lost-update on
`txbuf_used` wedges the sender or underflows -> heap OOB write into `txbuf`. Fix:
a per-PCB lock around the send/ack/retransmit critical sections.

### T5. signal_send non-atomic RMW of sigstate.blocked (`kernel/proc/signal.c:46`)
`if (sig == SIGKILL) t->sigstate.blocked &= ~bit;` is a non-atomic RMW on the
*target* task's `blocked` from the sender's CPU, while the target RMWs its own
`blocked` in `sys_sigprocmask`/`signal_setup_frame`. Lost-update -> SIGKILL
mis-blocked or a mask bit corrupted. (`pending` is correctly atomic; only
`blocked` is unprotected.) Fix: `atomic_and` here + atomic mask RMWs.

## TODO — lower priority / near-miss

- **sched_find_pid UAF window** (`sched.c:188`): returns a `task_t*` after
  `rcu_read_unlock`; callers (`sys_kill`, `sys_exit`'s parent `signal_send`)
  deref + `sched_wake` outside any RCU section -> UAF if the pid exits+destroys
  concurrently. Narrow window (zombie stays in `pid_ht` until destroy). Fix:
  hold the RCU section across the use, or refcount the task.
- **eventfd POLLOUT starvation** (`kernel/io/eventfd.c`): poll/select register
  only on `read_wq`; POLLOUT wakeups go to `write_wq`, so a task polling a FULL
  eventfd for POLLOUT is never woken. Unreachable in practice (full = 2^64-2).
- **unix_sock_accept** wakes client via `unix_wake` not `unix_poll_wake`
  (line ~612): latent missing-wakeup, harmless until non-blocking connect lands.
- **ext2_readdir over-read** (`kernel/fs/ext2.c:1462`): guards `off+8<=blk_bytes`
  then reads up to 254 name bytes; a dirent near the block tail can read past the
  4096-byte bcache slot. Bounded over-READ, needs a corrupt/concurrently-mutated
  dir block. Fix: bound `name_len` against the remaining block bytes.
- **sys_readdir missing copy_from_user** (`kernel/syscall/syscall.c:1397`):
  `__builtin_memcpy(raw, upath, pathlen)` straight from a user pointer -> kernel
  fault on a bad pointer instead of `-EFAULT`.
- **AF_UNIX data buffers non-atomic** (`unix_sock.h:106`): `buf_count`/`head`/
  `tail` plain fields RMW'd by sender+receiver on different CPUs. The lost-wakeup
  interlock is TSO-safe, but the byte-copy loops tear under concurrent r/w.
