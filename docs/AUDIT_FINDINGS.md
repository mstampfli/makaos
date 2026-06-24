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

### F3. dcache resurrect-from-zero UAF (`kernel/fs/dcache.c`) — commit pending
`dcache_lookup` bumped `refcount` 0->1 with a bare `__atomic_fetch_add` (no
tryget) while the shrinker freed `refcount == 0` dentries under `g_dcache_wlock`.
A lookup resurrecting (0->1) a dentry between the shrinker's refcount read and
its free left a freed slot referenced / on the LRU -> UAF (and, via
SLAB_TYPESAFE_BY_RCU slot reuse, a stale `child_ino` read). Here `refcount==0`
is the *normal alive* state, so the fix is NOT a plain `vfs_tryget`; it is a
`DCACHE_REF_DYING` sentinel that makes "claim for free" and "resurrect" mutually
exclusive on the refcount word: the shrinker CAS `0 -> DYING` (skips on fail),
and `dcache_lookup` CAS-bumps from any non-DYING value (treats DYING as a miss).
**Test:** `dcache_race_selftest` (`SELFTESTS=1`) stresses lookers + shrinker +
reinstalls and asserts no looked-up dentry yields a corrupted `child_ino` —
PASSES post-fix with no fault. The race window is tiny (the bump must slip
between the shrinker's refcount read and the free), so the stress test is a
regression guard; the primary confirmation is the TOCTOU code proof.

### F4. Unvalidated user-pointer memcpy across 6 syscalls (`kernel/syscall/syscall.c`) — commit pending
`sys_readdir`, `sys_stat`, `sys_unlink`, `sys_getcwd`, `sys_chdir`, `sys_mkdir`
did raw `__builtin_memcpy` directly on user pointers (the path argument, and
for readdir/getcwd the output buffer) with no validation. A bad pointer faulted
the kernel (DoS); worse, the `readdir`/`getcwd` *writes* took a user-supplied
destination -> passing a kernel address yielded an arbitrary kernel write
(privilege-escalation primitive). Fixed by routing all 7 sites through the
existing `copy_from_user`/`copy_to_user` (validate via `_access_ok` + prefault,
return `-EFAULT`), matching the pattern `sys_poll`/`sys_stat`'s siblings already
use. The guarded spawn-attr copy (had `_access_ok`) and the getcwd kernel-HHDM
write (kernel-owned frame) were correctly left alone. **Test:**
`copy_user_selftest` (`SELFTESTS=1`) asserts `copy_from_user`/`copy_to_user`
reject kernel / non-canonical / past-ceiling / NULL pointers with `-EFAULT`
instead of faulting -> PASSES. Boot exercises readdir/stat/chdir heavily, so a
healthy boot confirms no regression for valid pointers.

## TODO — confirmed, high severity

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
NOTE (re-scoped): not the quick fix originally assumed. `unix_sock_send_ex`
BLOCKS (WAIT_EVENT_HOOK on a full peer buffer), caches `peer` across iterations,
and derefs `peer` directly (cbuf_write), not just `peer->file`. A correct fix
must keep the peer + its file alive across a blocking send (re-read + re-validate
under RCU each iteration, or refcount the peer). Treat like T2 - careful
refactor, give it a dedicated iteration.

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
- **unvalidated user-pointer memcpy in syscalls** -> FIXED, see F4 (the
  sys_readdir near-miss expanded into a 6-syscall sweep).
- **AF_UNIX data buffers non-atomic** (`unix_sock.h:106`): `buf_count`/`head`/
  `tail` plain fields RMW'd by sender+receiver on different CPUs. The lost-wakeup
  interlock is TSO-safe, but the byte-copy loops tear under concurrent r/w.
