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

### F11 (was T2). CoW/fork PTE + refcount race (`kernel/mm/vmm.c`) - FIXED, commit pending
`vmm_clone_user_ex` writes the parent PTE (drop WRITABLE) + `pmm_ref_inc`
WITHOUT `vma_lock`, while the CoW-break fault handler does its read-check-act
under `vma_lock`. They sync on different things, so a sibling thread faulting a
page mid-clone can take the `rc==1` "sole owner" no-copy branch -> parent and
child PTEs alias one frame (cross-address-space corruption) + skewed refcount ->
later double `pmm_ref_dec` frees a mapped frame (UAF). In-tree comments
(`process.c:420`, `vmm.c:670`) document the victims. Fix: hold `src_mm->vma_lock`
across the clone's parent-PTE walk. FIXED: the clone now takes src_mm->vma_lock per-PTE around the read-decide-write (src_pt[si] read -> VMA lookup -> RO write + pmm_ref_inc), the same lock+decision the fault handler uses, so a sibling fault can no longer interleave. Per-PTE (not whole-walk) so concurrent faults on other pages still progress; callers (task_fork, sys_thread) hold no vma_lock so no self-deadlock; the clone touches only page-table/HHDM memory so it cannot fault under the lock. Tiny-window race so no deterministic test (like F2/F3/F5); confirmed by code proof + a clean boot that fork-execs login/svcmgr (the clone path) with no deadlock/corruption + all 10 selftests pass.

### T3. AF_UNIX UAF: close frees vfs_file_t while peer sends (`kernel/net/unix_sock.c`)
Split into two distinct UAF classes once verified against the real code:

(A) the `->file` UAF -> FIXED (F14): `unix_sock_close` `kfree`d the `vfs_file_t`
synchronously (old line ~321) while only RCU-deferring the `unix_sock_t`, even
though a peer reaches that file via the raw `peer->file` back-pointer
(`unix_poll_wake` derefs `->file->waitq`; called from `free_rcu`, `send_ex`,
`recv`, `connect`). So the file had a SHORTER lifetime than the sock that points
at it -> derefs of a freed file. Fixed by deferring the file free into
`unix_sock_free_rcu` (freed together with the sock), establishing the invariant
"the vfs_file_t lives exactly as long as its unix_sock_t". Also fixed a real
(rare) DOUBLE FREE in the `unix_sock_pair` ENOMEM path (`unix_sock_close(a)`
already owns a's teardown; the trailing `kfree(a)` freed it twice).

(B) the peer-SOCK UAF -> FIXED (F15). `unix_sock_send_ex` BLOCKED
(WAIT_EVENT_HOOK on a full peer buffer), cached `peer = s->peer` across
iterations, and dereffed the cached `peer` (`cbuf_write(peer)`, `peer->buf_count`
in the HOOK condition, `unix_wake(peer)`) with NO `rcu_read_lock`; `recv`
post-drain `unix_wake(s->peer)`, `poll` `s->peer->buf_count`, `shutdown`, and
`sendfd` had the same synchronous shape. The peer `unix_sock_t` could be
RCU-freed during the block/deref -> UAF on the sock object. Fixed with the Linux
af_unix model: a refcount on `unix_sock_t` (init 1 = owner), a pure CAS-from-
nonzero `unix_refcount_tryget`, `unix_get`/`unix_put` (RCU-deferred free at 0),
and `unix_pin_peer` (read `s->peer` + tryget under `rcu_read_lock`). `send_ex`
pins the peer for the whole call (held across sleeps); the synchronous derefs
take a brief `rcu_read_lock` (or pin, for sendfd which mutates peer state).
Prerequisite invariant added: `unix_sock_close` now notifies the connected peer
SYNCHRONOUSLY (clears the symmetric back-pointer `peer->peer==s`, marks it
DISCONNECTED, wakes it) under a peer pin BEFORE dropping the owner ref, so a
reader sees NULL not a dangling pointer; the notify was moved out of
`unix_sock_free_rcu`. Also added the disconnect terms to the `send_ex` HOOK
condition so a peer-close during a full-buffer block wakes the sender instead of
hanging (lost-wakeup). Verified by `unix_refcount-selftest` (pure tryget +
get/put balance + pin_peer) + clean boot (socketpair/scm_rights still PASS;
Wayland churns the path). Scoped out (separate, not part B): the SOCK_DGRAM
default-destination `s->peer = target` (set in connect, line ~716) is a raw
pointer with no connection ref and target does not point back, so it can dangle
if the target closes; DGRAM connect is rare (Wayland uses STREAM). The clean fix
there is to store the target PATH and re-resolve via `ns_find` per `sendto`
(already the RCU-safe path for by-path sends), or hold a connection ref.

### T4. TCP PCB fields mutated lock-free from 3 CPUs (`kernel/net/tcp.c`)
`tcp_timer_tick` retransmit (`snd_nxt = snd_una` rewind), `tcp_send`
(`txbuf_used += chunk`), and `tcp_recv` ACK (`txbuf_used -= acked`,
`txbuf_head += acked`, `snd_una = ack`) RMW the same non-atomic PCB fields from
different CPUs with no per-PCB lock. Torn `snd_nxt` mis-sends; lost-update on
`txbuf_used` wedges the sender or underflows -> heap OOB write into `txbuf`. Fix:
a per-PCB lock around the send/ack/retransmit critical sections.

### F5. signal_send non-atomic RMW of sigstate.blocked (`kernel/proc/signal.c`) — FIXED, commit pending
`signal_send`'s `t->sigstate.blocked &= ~bit` (SIGKILL unblock) was a non-atomic
RMW on the *target* task's `blocked` from the sender's CPU, racing the target's
own RMWs in `sys_sigprocmask` (`|=`/`&=`) and `signal_setup_frame` (`|=`) ->
lost update (SIGKILL clear undone, or a mask bit corrupted). `pending` was
already atomic; only `blocked` RMWs were not. Fixed: `atomic_and`/`atomic_or`
(smp.h) at all four RMW sites (signal_send, the two sigprocmask RMWs, the two
setup_frame ORs); the SETMASK full-store and init stores stay plain (aligned
32-bit stores are atomic on x86 and SIGKILL is masked out of SETMASK). No
deterministic test: tiny-window cross-CPU data race, and signal_send logs per
call so a high-iteration faithful test floods the serial. Confirmed by code
proof + clean boot (login/shell exercise sigprocmask) + all 8 selftests pass.

## TODO — lower priority / near-miss

- **sched_find_pid UAF window** (`sched.c:188`): returns a `task_t*` after
  `rcu_read_unlock`; callers (`sys_kill`, `sys_exit`'s parent `signal_send`)
  deref + `sched_wake` outside any RCU section -> UAF if the pid exits+destroys
  concurrently. Narrow window (zombie stays in `pid_ht` until destroy). Fix:
  hold the RCU section across the use, or refcount the task.
- **eventfd POLLOUT starvation** (`kernel/io/eventfd.c`) -> FIXED (F7).
  poll/select registered only on `read_wq` (`f->waitq`); the POLLOUT wakeup
  fires on `write_wq` when a reader drains a full eventfd, so a task polling a
  FULL eventfd for POLLOUT was never woken. Fixed: `f->secondary_waitq =
  &s->write_wq` (sys_poll/sys_select register on both waitqs). Extended
  `eventfd_selftest` (Case 5) asserts a full eventfd reports POLLIN-not-POLLOUT
  and that secondary_waitq points at write_wq -> PASSES. (Low reachability:
  full = 2^64-2, but the fix is one line and correct.)
- **unix_sock_accept** wakes client via `unix_wake` not `unix_poll_wake`
  (line ~612): latent missing-wakeup, harmless until non-blocking connect lands.
- **ext2_readdir over-read** (`kernel/fs/ext2.c`) -> FIXED (F6). The walk guarded
  only `off+8<=blk_bytes` (header) then read up to `name_len` (<=255) name bytes,
  so a corrupt dirent near the block tail read past the 4096-byte bcache slot
  (info leak). Fixed: extracted `ext2_dirent_namelen_clamp(off,name_len,blk_bytes)`
  (min of name_len and bytes-left-after-the-8-byte-header) and use it before the
  name copy. Deterministic `ext2_readdir_clamp_selftest` (SELFTESTS=1) checks the
  boundary cases (the 4088/4090 tail dirents clamp to 0) -> PASSES; valid dirs are
  unaffected (name always fits), confirmed by a clean boot (ls/path walks).
- **unvalidated user-pointer memcpy in syscalls** -> FIXED, see F4 (the
  sys_readdir near-miss expanded into a 6-syscall sweep).
- **AF_UNIX data buffers non-atomic** (`unix_sock.h:106`): `buf_count`/`head`/
  `tail` plain fields RMW'd by sender+receiver on different CPUs. The lost-wakeup
  interlock is TSO-safe, but the byte-copy loops tear under concurrent r/w.

## Second audit pass (2026-06-24) - un-audited subsystems

### F8. ext2 dir_lookup name over-read (`kernel/fs/ext2.c`) - commit pending
The F6 readdir clamp MISSED the parallel dirent walk in `dir_lookup` (path
resolution - more reachable than readdir: every open/stat/exec hits it). Same
class: `kmemeq(de->name, name, name_len)` with only the `off+8<=blk_bytes`
header guard -> OOB read past the 4096-byte bcache slot on a corrupt dirent.
Fixed by routing through the existing `ext2_dirent_namelen_clamp` helper (only
compare when the name fits the block). Covered by `ext2_readdir_clamp_selftest`.

### F9. io_uring OP_READ/OP_WRITE unvalidated sqe->addr (`kernel/io/io_uring.c`) - commit pending
CRITICAL. OP_READ/OP_WRITE passed the user-controlled `sqe->addr` straight to
`vfs_read`/`vfs_write`/`pread` with no validation (every other opcode taking a
user pointer routes through a `sys_*` wrapper that validates). OP_READ with a
kernel address = arbitrary kernel WRITE; OP_WRITE = arbitrary kernel READ -> LPE
from an unprivileged process. Fixed: new exported `user_buf_check(addr,len)`
(= `_access_ok` range-check, the mm-independent LPE guard, + `user_buf_prefault`
for unmapped-user-ptr panic-prevention) called before both ops. `copy_user_selftest`
extended to assert user_buf_check rejects kernel/non-canonical/ceiling/NULL.

### TODO (from second pass) - careful but high value:
- **PTY ioctl arbitrary kernel R/W** -> FIXED (F10): routed pty_master/slave_ioctl
  through a shared pty_tty_ioctl_common using copy_*_user (also fixed the slave's
  missing-SIGWINCH TODO); pty_ioctl_selftest asserts -EFAULT for bad pointers.
  (original:) (`kernel/drivers/tty/pty.c` pty_master_ioctl
  ~414, pty_slave_ioctl ~474): TCGETS/TIOCGWINSZ/TIOCGPGRP/TIOCGPTN do
  `*(user_ptr) = tty->field` (arbitrary kernel WRITE) and TCSETS/TIOCSWINSZ/
  TIOCSPGRP do `tty->field = *(user_ptr)` (arbitrary kernel READ) on the raw
  ioctl `arg`. CRITICAL LPE, reachable via /dev/ptmx + ioctl. The console tty0
  path was fixed (copy_*_user) but PTY was missed. Fix: copy_from_user/copy_to_user
  each case (ideally hoist a shared tty_ioctl_common - master/slave/console are 3
  drifting copies). Deterministically testable (call pty_*_ioctl with bad ptrs).
- **ELF e_phoff integer overflow** -> FIXED (F12): extracted overflow-safe
  elf_phtab_in_bounds(e_phoff,phnum,size) (offset>size catches the wrap, then a
  remainder check) + elf_phtab_bounds_selftest (the -56 wrap + boundary cases) ->
  PASSED; valid ELFs (login/svcmgr) still load. Follow-up -> FIXED (F13):
  p_vaddr not range-checked + seg_end overflow (elf.c:157) could make
  seg_end<seg_start -> bogus/empty VMA, or a kernel-half/non-canonical base ->
  an un-faultable mapping; added pure elf_seg_range_ok(vaddr,bias,memsz,user_top)
  rejecting overflow + base>=user_top + base+memsz past user_top, called per
  PT_LOAD before seg_start/seg_end; covered by the extended elf self-test.
  (original:) (`kernel/proc/elf.c:43`): the sole guard
  `e_phoff + phnum*56 > size` overflows; e_phoff=-56 wraps to 0 (<= size) ->
  program-header reads land 56 bytes before hdr_buf (OOB heap read driving
  segment VA/offset decisions). Fix: overflow-safe bounds (pure helper
  `elf_phtab_in_bounds`, like ext2_dirent_namelen_clamp) + reject phentsize<56;
  deterministically testable. Secondary: p_vaddr not range-checked + seg_end
  overflow (elf.c:107).
