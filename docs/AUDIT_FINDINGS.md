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
`txbuf_used` wedges the sender or underflows.

DATA-PLANE LOCK -> FIXED (F44, the recommended first decomposition below). Added
a per-PCB `spinlock_t lock` (taken via tcp_pcb_lock = preempt_disable + spin_lock
-- the F35/fb.c pattern: RX runs in the net kernel thread, never an IRQ handler,
so preempt_disable prevents the same-CPU-preempt deadlock without irqsave, and
because the lock guards ONLY the field RMWs the transmit stays OUTSIDE it,
sidestepping the irqsave-across-virtio-tx hazard noted below). It serializes the
ring accounting -- txbuf_used/txbuf_head + snd_una (RX ACK drain vs tcp_send),
rxbuf_used/rxbuf_tail/rcv_nxt (RX data write vs tcp_recv_data) -- between the RX
thread and the socket syscall path. tcp_send / tcp_recv_data use a RESERVE
pattern (reserve the ring index range under the lock, copy the user payload
OUTSIDE the lock -> no user-memory fault under a spinlock, multi-producer/
consumer-safe); pcb_wake, tcp_send_segment, and sched_sleep all stay outside the
lock. Pure helper tcp_ring_reserve + deterministic tcp_ring_reserve_selftest
(pairs with tcp_ring_consume_selftest); code-proof of the lock placement + clean
boot (the locked paths are not boot-exercised -- no TCP data at boot -- so the
arithmetic is unit-tested and the placement code-proven).
accept-queue child<->listener ordering -> FIXED (F45): the listener's
accept_queue (accept_head/tail/count) was RMW'd by the RX thread's enqueue
(SYN_RCVD->ESTABLISHED) and accept()'s dequeue with no lock -- torn uint8_t
count, lost/double-dequeued child. Fixed by extracting the ring into pure
accept_q_push/accept_q_pop helpers (single source of truth) taken under the
LISTENER's own pcb->lock at both sites -- one lock, NO child<->listener two-lock
cycle (the queue belongs to the listener); pcb_wake stays outside the lock.
Deterministic tcp_accept_q_selftest (FIFO, full-reject, empty, wrap).
REMAINING (smaller, separate item, recorded): `snd_nxt` (advanced inside
tcp_send_segment, which also transmits -> needs the seq-advance/transmit split
before it can be locked) and the connection state-machine transitions
(state/snd_una/rcv_nxt set in SYN_SENT/SYN_RCVD/FIN paths). Single-writer-
dominant or benign-read races, NOT the dangerous txbuf_used corruption (F44) or
the accept-queue tear (F45).

VERIFIED DESIGN NOTES (from a deep read this pass, to de-risk the dedicated run):
  - Contexts: tcp_send = syscall CPU; tcp_recv = net_rx kthread; tcp_timer_tick
    runs from BOTH net_tcp_timer_thread AND net_rx_thread (net.c:118 and :133),
    so even two timer ticks can race. Three+ CPUs touch one PCB.
  - `ipv4_send` does NOT sleep (ARP miss returns -1 + queues a request;
    `eth_send` queues to virtio tx), so the per-PCB lock CAN be held across
    `tcp_send_segment` (which RMWs snd_nxt + transmits). It just must be DROPPED
    around `sched_sleep` in tcp_send / tcp_recv_data (buffer-full / empty waits).
  - Lock ordering is clean for the data plane: pcb->lock never nests with
    `s_pcb_wlock` (send/recv/timer walk under rcu_read_lock, only insert/remove
    take s_pcb_wlock); the tx path takes arp/virtio locks strictly UNDER
    pcb->lock with no path back into TCP -> no cycle. Move `pcb_wake` (which
    takes rq_lock) to AFTER the unlock to avoid holding the data lock across a
    scheduler op.
  - THE HARD PART: a FULL-correctness lock must also cover the handshake
    (`snd_nxt`/`snd_una` set in SYN_SENT/SYN_RCVD race the timer's SYN-rewind)
    and the TCP_LISTEN/SYN_RCVD cases touch TWO PCBs -- the child is linked into
    `s_pcb_head` by tcp_pcb_alloc the instant it is created (so the timer can
    retransmit on a half-initialised child) and SYN_RCVD mutates the LISTENER's
    `accept_queue` while holding the child. That is a child<->listener two-lock
    ordering hazard (tcp_accept locks the listener) that must be designed (e.g.
    a separate accept-queue lock, or a fixed listener-before-child order, or
    lock the child at alloc). A narrower "data-plane-only" lock (txbuf/rxbuf +
    send-seq, NOT the state machine / accept queue) sidesteps the two-lock
    hazard and fixes the dangerous txbuf_used corruption, leaving the
    state-transition races as a smaller separate item; that is the recommended
    first decomposition. Also add an underflow clamp to `txbuf_used -= acked`
    (defense-in-depth; deterministically unit-testable as a pure helper).
  - NEW CONSTRAINTS found this pass that REVISE the lock design (must be honored
    by the dedicated run):
      * `spin_lock` does NOT disable preemption (plain test-and-set, smp.h), so a
        per-PCB lock taken from BOTH syscall (tcp_send/recv_data) and kthread
        (net_rx/timer) contexts MUST use `spin_lock_irqsave` (like s_pcb_wlock) or
        a preempted holder + same-CPU spinner deadlocks.
      * `virtio_net_tx` (virtio_net.c:387) does a SYNCHRONOUS POLLING WAIT for tx
        completion (up to 10M `cpu_relax` spins) while holding `s_tx_lock`. So the
        earlier "the lock CAN wrap tcp_send_segment" note is WRONG for an irqsave
        lock: holding pcb->lock with IRQs OFF across tcp_send_segment -> ipv4_send
        -> virtio_net_tx would keep IRQs disabled for a whole tx poll (ms-scale).
        => the per-PCB lock must guard ONLY the field RMWs; the transmit must
        happen OUTSIDE the lock. That requires splitting tcp_send_segment into a
        locked seq-advance (snapshot snd_nxt, advance it, arm rto) + an unlocked
        build-and-transmit -- a refactor of its ~8 callers. This is why the full
        lock is a dedicated, careful iteration, not a one-turn change.
  - DONE this pass (F18, defense-in-depth, NOT the lock): the underflow clamp is
    landed as a pure shared helper `tcp_ring_consume(head,used,n,mask)` (clamps n
    to *used) used at the txbuf ACK drain and the rxbuf read drain, with a
    deterministic tcp_ring_consume_selftest. This bounds the race's WORST outcome
    (txbuf_used/rxbuf_used underflow -> producer wedge) to a transient
    mis-account. The per-PCB lock itself remains OPEN.

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

- **sched_find_pid UAF window** (`sched.c:188`): `pid_ht_find` drops its OWN
  rcu_read_lock before returning a bare `task_t*`; callers deref it outside any
  RCU section -> UAF if the pid exits+task_destroy (RCU-deferred free via
  task_free_rcu) completes in the window. Narrow (zombie stays in `pid_ht` until
  destroy). Fix: hold rcu_read_lock across the lookup+use (matches pcb_find /
  ns_find). PART A -> FIXED (F16): the high-reachability syscall + signal paths.
  Added DRY helper `signal_send_pid(pid, sig)` (signal.c) = lookup + deliver
  under one rcu_read_lock; routed `sys_exit`'s SIGCHLD-to-parent, `do_exit`'s
  (signal.c) SIGCHLD-to-parent, and `sys_kill`'s lookup case through it; wrapped
  `sys_setpgid`/`sys_getpgid`/`sys_getsid` field reads in rcu_read_lock.
  signal_send is rcu/lock-safe (atomic bit + sched_wake->rq_lock, no sleep), so
  holding the section across delivery is sound. Deterministic
  signal_send_pid_selftest (unknown pid -> -ESRCH, proves the rcu section
  balances). PART B -> FIXED (F17): the `/proc` (`proc.c` proc_open /
  proc_readdir / proc_stat) and `virtfs.c` callers. Confirmed none of the deref
  helpers sleep (no sched_sleep/WAIT in proc.c) and none store `t` beyond their
  call (gen_* snapshot into a heap buffer, proc_fd_open takes its own fd ref,
  readdir fills the caller's out array), and `rcu_read_lock == preempt_disable`
  is a nestable counter (so proc_fd_open's inner section nests cleanly). Wrapped
  each call site in rcu_read_lock spanning the lookup + all `t` derefs, with a
  SINGLE exit (goto open_out/rd_out/stat_out, scoped blocks so the gotos don't
  cross `sub`'s init) so the section is never leaked; virtfs reads t->cred into
  locals so it is a tight 3-line wrap.  sched_find_pid UAF now FULLY closed.
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

## THIRD AUDIT PASS (5-agent fan-out, 2026-06-25) — backlog refill

Five HIGH-confidence, clean, deterministically-testable findings from a fan-out
over still-un-audited subsystems. Fixed the highest reachability x severity x
cleanliness one (sys_select) this pass; the other four are the refreshed backlog.

- **sys_select unvalidated user fd_set read** (`kernel/syscall/syscall.c` sys_select)
  -> FIXED (F19). The input fd_sets were read by RAW DEREF of the user pointers
  (`((fd_set_t*)rset_ptr)->bits[i]`) with no _access_ok/copy_from_user, while the
  timeout and the writeback already used copy_*_user. A kernel pointer -> arbitrary
  kernel read (the selected bits surface in the returned output sets); a bad
  pointer -> kernel-fault DoS. Reachable from ANY select() call (no opt-in). The
  input twin of sys_poll's hardened path (F4/F9 class). Fixed: copy_from_user each
  non-NULL fd_set (-EFAULT on failure), memset NULL ones; also checked the
  previously-ignored timeout copy_from_user return (was an uninitialized-tv read).
  copy_user_selftest extended to assert sys_select(1, bad_ptr, 0,0,0) == -EFAULT.

- **ext2 block size unbounded from on-disk superblock** (`kernel/fs/ext2.c:767`
  ext2_init) -> FIXED (F21). `s_block_size = 1024u << sb->s_log_block_size` with
  no clamp; the bcache slots + DMA scratch are hard-fixed at 4096. A
  crafted/corrupt image with s_log_block_size >= 3 -> block size > 4096 ->
  ahci_read DMAs past the 4096 scratch (heap overflow) and bcache_fill_way memcpy
  overruns the 4096 BSS slot; s_log_block_size >= 32 is also shift UB. Fixed by a
  pure validated helper `ext2_block_size_checked(log)` (returns 1024/2048/4096
  for log 0/1/2, else 0) used at mount, which refuses the mount on an unsupported
  size; also introduced the named constant EXT2_BLOCK_SIZE_MAX=4096 as the single
  source of truth for the bcache slot dimension + the three EXT2_SCRATCH kmalloc
  sizes + the helper's bound (DRY). Deterministic ext2_block_size_selftest
  (0/1/2 -> 1024/2048/4096 all <=MAX; 3/10/31/32 -> 0). Boot still mounts the
  real disk + fork-execs /bin/login (mount runs before login) so no regression.

- **unveil sandbox bypass via /dev|/proc prefix without boundary**
  (`kernel/syscall/syscall.c:411-413` sys_open got_file) -> FIXED (F20). The
  unveil exemption tested the literal prefix `/dev`/`/proc` with no next-char
  `/`-or-`\0` check, so a REAL on-disk file named `/devsecrets` or
  `/processData` skipped unveil_check entirely -> sandbox escape. unveil_check
  itself is correct; the inline exemption was a drifted duplicate of "is this a
  virtual mount?". Fixed (DRY + security): replaced the two hand-rolled prefix
  tests with the single existing boundary-correct `virtfs_is_virtual(path)`
  (find_mount requires `path[n]=='/'||'\0'`; the mount table is exactly
  /proc,/dev, so the exemption set is unchanged but boundary-correct).
  Deterministic virtfs_is_virtual_selftest: /dev, /dev/, /dev/tty, /proc,
  /proc/self/status -> virtual; /devsecrets, /processData, /device_keys,
  /home/user, / -> NOT virtual -> PASSED. Boot still fork-execs /bin/login
  (every open runs this block) so no regression to normal opens.

- **virtio-net rx desc_id OOB (device-controlled index)**
  (`kernel/net/virtio_net.c:417,434` virtio_net_rx_poll) -> FIXED (F22). `desc_id`
  was a uint32 read verbatim from the device-written used ring and used to index
  s_rx_bufs[256] and s_rxq.desc[256] with NO bounds check (adjacent pkt_len IS
  clamped with an "untrusted device" comment) -> OOB read of a buffer pointer +
  memcpy from it + OOB WRITE of 4 descriptor fields. Same unchecked id at the TX
  free (line 395) -> virtq_free_desc OOB-writes desc[idx] and corrupts free_head.
  Fixed with pure `virtio_desc_id_valid(id) = id < VIRTQ_SIZE`: rx-poll advances
  last_used_idx first then drops a bad-id completion (return 0, no table index, no
  re-post); TX frees the device id only if valid, else the known-submitted `idx`
  (no device trust, no descriptor leak); and virtq_free_desc itself now guards
  (defense-in-depth at the OOB-write primitive). Deterministic
  virtio_desc_id_valid_selftest (0, 255 valid; 256, 1000, 0xFFFFFFFF invalid).
  Boot DHCP/rx still works (the guard only drops ids a well-behaved device never
  sends). Threat model: a malicious/buggy device (hypervisor/passthrough/future
  real NIC); QEMU SLIRP is trusted today, so this is robustness/hardening.

- **DRM create_dumb pitch 32-bit overflow** (`kernel/drivers/video/drm.c:486`
  drm_ioctl_create_dumb) -> FIXED (F23). `pitch = a.width * 4` was a 32-bit
  multiply (the uint64_t cast on the next line too late); width=0x40000000 ->
  pitch wraps to 0 -> tiny backing allocation while resource_create gets the full
  huge width/height -> undersized buffer, host-side OOB on transfer/flush. Only
  gate was width/height nonzero; the advertised max_width=8192 was never enforced.
  Reachable by any process opening /dev/dri/card0 (compositors). Fixed with a pure
  helper drm_dumb_size(w,h,bpp,&pitch,&size) doing all multiplies in 64-bit +
  rejecting bpp!=32, zero dims, and dims > DRM_MAX_FB_DIM (16384, above the
  advertised 8192 so a well-behaved compositor never trips it; keeps pitch<=65536
  in u32 and size in u64); replaced the inline computation (and the now-redundant
  nonzero/bpp checks) with it. The cursor path (drm.c) already did the analogous
  (uint64_t)w*h*4 check correctly (precedent). Deterministic drm_dumb_size_selftest
  (1920x1080 -> pitch 7680 size 8294400; 1x1; MAX x MAX -> 65536 / 2^30; 0x40000000
  & 0x40000001 -> -EINVAL [the overflow rows]; zero w/h; bpp 24; MAX+1 -> -EINVAL).
  Boot still fork-execs /bin/login and the compositor create_dumb path works.

## FOURTH AUDIT PASS (5-agent fan-out, 2026-06-25) — backlog refill

Fan-out over futex, mmap/VMA, pipe+fd-table, signal/sigreturn, and
storage-queue/allocator. Signal/sigreturn was audited and found CORRECTLY
HARDENED (sigreturn validates RIP/RSP < 2^47, forces user CS/SS via hardcoded
iretq immediates, sanitizes RFLAGS to (x & 0xCD5)|0x202, sanitizes MXCSR; auxv/
argv/envp stack construction is fully bounded) -- no fix needed there. Four real
findings; fixed the highest reachability x severity (mmap LPE) this pass.

- **sys_mmap/sys_munmap unbounded MAP_FIXED -> live kernel-frame free (LPE)**
  (`kernel/syscall/syscall.c` sys_mmap/sys_munmap) -> FIXED (F24). The
  page-rounding `len = (len + PAGE_MASK) & ~PAGE_MASK` wrapped to 0 for a huge
  len, and `[addr, addr+len)` was never bounded to the user half (MAP_FIXED only
  checked nonzero+aligned; munmap likewise). Every process shares the kernel's
  higher half (PML4[256..511] shallow-copied, vmm.c:198), so a MAP_FIXED or
  munmap at a kernel/HHDM address (e.g. 0xFFFF800000000000) drove
  mm_unmap_range_flush_free -> vmm_page_unmap -> pmm_ref_dec on a LIVE kernel
  frame -> kernel memory corruption / LPE, reachable from ANY unprivileged
  process (pledge only gates PROT_EXEC). Fixed with two pure overflow-safe
  helpers reusing the existing _access_ok predicate: mmap_round_len(len) (rejects
  zero/overflow) and mmap_range_ok(addr,len,*olen,*oend) (page-aligned + strictly
  in the user half). sys_mmap MAP_FIXED + munmap now reject an out-of-range
  request with -EINVAL; a non-fixed addr HINT that fails validation falls back to
  kernel-chosen placement (correct POSIX). Deterministic mmap_range_selftest
  (NULL/kernel-half/non-canonical/len-overflow/end-past-ceiling/misaligned all
  rejected; legal range accepted with rounded len + exclusive end). Boot
  fork-execs /bin/login (mmap/munmap are on every process's hot path) -> no
  regression. (Found + drafted by the fan-out subagent; independently verified:
  _access_ok rejects NULL/>USER_ADDR_MAX/overflow/end-past-ceiling, the shared
  higher half + unmap->pmm_ref_dec chain confirmed, full SELFTESTS build + clean
  boot.) Note: no sys_mremap exists; sys_mprotect changes perms (not a frame
  free) so it is a separate lower-severity concern if it lacks a user-half bound.

- **futex_wait raw user deref under the bucket spinlock**
  (`kernel/proc/futex.c` futex_wait) -> FIXED (F27). The authoritative
  post-enqueue compare was `cur = *(volatile uint32_t*)uaddr;` done UNDER
  b->lock; a concurrent munmap/mprotect(PROT_NONE) of that page (same address
  space, another CPU, unserialized vs the futex bucket) faulted the read -> the
  page-fault handler takes mm->vma_lock and may alloc/sleep while a raw spinlock
  is held (fault-under-spinlock + lock-nest -> panic/DoS, attacker-controlled).
  The "stays mapped, cannot fault" comment was false (no swap != no munmap).
  Fixed: restructured to enqueue under b->lock, UNLOCK, then do the authoritative
  re-read with a fault-safe copy_from_user OUTSIDE the lock. The lost-wake fence
  still holds: the spin_lock ACQUIRE barrier (not the enqueue store) orders the
  re-read after any waker's prior bucket-unlock, so a concurrent FUTEX_WAKE
  either already saw our enqueue (and set w.woken) or wrote the new value we now
  read. On fault/mismatch: re-take b->lock, consume the wake if w.woken (return
  0) else unlink + return -EFAULT/-EAGAIN (mirrors the existing timeout-path
  unlink). Added an early `cur != val -> -EAGAIN` fast-path (no enqueue needed).
  Verified: code-proof (no raw user deref remains under b->lock; copy_from_user
  -> user_buf_prefault uses RCU + returns -EFAULT for a no-VMA page, and is
  called with the lock dropped so no nest) + the copy_from_user safety is already
  covered by copy_user_selftest + clean boot (qemu healthy through userland, no
  panic/hang; a futex lock bug would deadlock the desktop's pthread mutexes).
  Race-class (F2/F3/F5/F11 precedent) so no bespoke deterministic test.

- **pipe double-free / UAF on concurrent last-close**
  (`kernel/fs/pipe.c` pipe_read_close/pipe_write_close) -> FIXED (F25). The
  shared pipe_buf_t was freed by a NON-ATOMIC decrement-and-test
  (`if (reader_refs==0 && writer_refs==0) kfree(p)`) with zero locking; the read
  end and write end are distinct vfs_file_t sharing one pipe_buf_t, so a
  simultaneous last-close of both ends on two CPUs both observed refs==0 and both
  kfree(p) -> double free. TWO further UAFs from the same root (each hook freed
  its own vfs_file_t with kfree(self)): (a) each close hook wakes the PEER end's
  waitq (p->write_file / p->read_file) which the peer hook frees -> wake-vs-free
  UAF; (b) pipe_read/pipe_write deref p->write_file/p->read_file while their end
  stays open but the peer closed+freed its file -> UAF. Same CLASS as F14/F15.
  Fixed by tying p + both end files to ONE lifetime (the F14 invariant): added
  `uint32_t open_ends` (init 2), a pure `pipe_last_end_release(uint32_t*)` =
  `__atomic_sub_fetch(open_ends,1,ACQ_REL)==0`, and `pipe_destroy(p)` that frees
  read_file + write_file + p together; both hooks now wake the peer then call
  pipe_destroy ONLY on the last release (no bare kfree(self)). The ACQ_REL makes
  every prior hook's peer-wake happen-before the last closer's free, so the free
  can't race a concurrent wake. Deterministic pipe_refcount_selftest (two
  releases from open_ends=2: exactly one returns "last" -> exactly one free).
  Boot exercises pipes (shell pipelines, swaybar status_command) -> no regression.

- **NVMe completion CID OOB (device-controlled index)**
  (`kernel/drivers/storage/nvme.c` nvme_irq_handler) -> FIXED (F26). `cqe.cid`
  (uint16, device-written into the completion queue) indexed the 64-entry
  q->req[] with NO bounds check -> OOB write of req->status/->done + a
  wait_queue_wake_all on a fabricated wait_queue_t (walks/writes a wake-list
  through garbage pointers). Exact F22 class for NVMe (the submit side IS bounded
  by the 64-bit cid_free_bitmap via cid_pop; only the device-echoed completion
  CID was trusted). AHCI is unaffected (slots derived via ctz of the hardware
  mask into a 32-entry array). Fixed with pure
  nvme_cid_valid(cid)=cid<NVME_IOQ_DEPTH and `if (!nvme_cid_valid(cqe.cid))
  continue;` after the CQE is consumed (cq_head/phase advanced) and before
  indexing req[] -- drops the corrupt completion while the doorbell still credits
  it. Deterministic nvme_cid_valid_selftest (cid 0, 63 valid; 64, 1000, 0xFFFF
  invalid). Boot uses AHCI (no NVMe device in the QEMU config) so the completion
  path is not live-exercised, but the helper is unit-tested and the guard is a
  minimal continue that does not touch the valid path. Confidence HIGH. Threat
  model: malicious/buggy NVMe controller or CQ-page DMA corruption (the repo
  treats this device-echo-index class as a real bug worth a guard, per F22).

## FIFTH AUDIT PASS (2-agent fan-out, 2026-06-25) — backlog refill

- **epoll watched-file UAF (close-before-epoll-close)**
  (`kernel/syscall/syscall.c` epoll_watch_register/unregister) -> FIXED (F28).
  epoll registered persistent epoll_we_t entries on a watched fd's waitq and
  saved raw wq1/wq2 pointers, but took NO reference on the watched vfs_file_t.
  The authors' "save the waitq pointers so we can unregister after the file is
  freed" mitigation only works for LONG-LIVED GLOBAL queues (g_mouse_waitq); for
  every fd whose waitq is EMBEDDED in the freed object (pipe f->_waitq, eventfd
  s->read_wq/write_wq, unix sock f->_waitq, pty master_waitq), closing the
  watched fd before the epoll fd freed that memory, so wq1/wq2 dangled and the
  later epoll_ctl(DEL/MOD) or epoll_close -> epoll_we_remove(wq1, &w->entry)
  walked freed memory (UAF). Reachable from pure userland, NO race (deterministic
  ordering: ADD a pipe/eventfd, close it, close epfd) -- the ordinary wlroots/
  libinput event-source teardown. Fixed with the Linux model: epoll_watch_register
  now vfs_tryget()s the watched file into w->file (pinning it + its embedded
  waitq for the watch's lifetime) and epoll_watch_unregister vfs_close()s it
  AFTER epoll_we_remove unlinks our entries; w->file freed exactly once, never
  before the entries are unlinked. unregister no longer needs a fresh fd lookup
  (uses w->file/wq1/wq2), so DEL/close/MOD drop the stale-lookup arg; MOD
  re-pins. Deterministic epoll_watch_refcount_selftest: register a watch on a
  heap vfs_file_t (embedded waitq, refcount 1) -> refcount 2; vfs_close the file
  -> refcount 1 (survives, pre-fix would free it); unregister -> freed. Same
  lifetime class as F14/F15/F25. (Found by the fan-out subagent; independently
  verified the register/unregister/ADD/DEL/MOD/close flow + that free_rcu does
  not touch w->file + full SELFTESTS build + clean boot.) Confidence HIGH.

- **TTY canonical ring framing** (part a) -> FIXED (F29). The cooked read_buf
  ring usable capacity was TTY_READ_BUF_SIZE-1 = 4095, exactly the max cooked
  line (4094 data + '\n'), so if the ring held even one un-drained byte
  ldisc_flush_line's per-byte rd_push silently DROPPED the line tail including
  the terminating '\n' -> canonical line framing corruption (reader merges lines
  / blocks for a dropped '\n'). NOT memory-unsafe (line_len <= 4095 enforced).
  Fixed: bumped TTY_READ_BUF_SIZE to 8192 (strictly > the max line, so a line
  fits even with backlog), added a pure rb_free(head,tail,size) helper, and made
  ldisc_flush_line ALL-OR-NOTHING -- commit the whole line only if it fits in the
  ring's free space, else drop the WHOLE line (POSIX-acceptable input loss, not a
  '\n'-less partial line). EOF (empty-line ^D) unchanged (rb_free >= 0 always, so
  the empty flush still wakes the reader -> 0-byte EOF read). Deterministic
  tty_rb_free_selftest (rb_free cases on a size-8 ring + the "empty ring holds a
  max line" invariant). The line-buffer bounds were already clean (line_buf
  guarded by < TTY_LINE_BUF_SIZE-1; c_cc indices constant in [0,18]).
- **TTY unlocked flush-vs-producer race** (part b) -> FIXED (F35). tty_flush_input
  (ioctl TCFLSH/TCSETSF + the ^C/^Z/^\ path) reset rd_head/rd_tail/line_len with
  NO lock while the keyboard thread / a concurrent pty_master_write ran rd_push /
  line_buf accumulation on another CPU, and dup-shared PTY slave fds let two
  consumers pop the ring at once -> lost/duplicated bytes and stale-content
  delivery (NOT OOB: indices are aligned uint32 always masked < bufsize).
  Fixed: added a per-tty `spinlock_t lock` (a plain spinlock under
  preempt_disable -- the producer is a kernel thread/syscall, never an IRQ, so no
  irqsave; mirrors fb.c). It is a LEAF lock serializing EVERY mutation of the
  ring + canonical line; echo (fb I/O), wait_queue_wake_all, and the blocking
  sleep stay OUTSIDE it. rd_push/rd_pop are pure (caller holds the lock); the
  ldisc line ops (append/erase/kill/flush) and tty_flush_input take it; a shared
  tty_ldisc_drain() (used by BOTH tty_vfs_read and pty_slave_read -- DRY, removed
  the duplicated inline pop) does the locked drain. Verified: cross-CPU-race
  code-proof (every ring/line MUTATION under tty->lock) + a deterministic
  tty_ldisc_selftest that drives the locked push/drain/flush path and asserts
  byte-correct output (proves the lock is balanced and cannot self-deadlock --
  a missing unlock would freeze the boot at the test) + clean boot to login.
  35 selftests PASS (was 34).

## SIXTH AUDIT PASS (read-only fan-out, 2026-06-25) — backlog refill

- **ext2 path_split kernel stack overflow** (`kernel/fs/ext2.c` path_split)
  -> FIXED (F30). path_split copied the parent portion of a path into a
  caller-supplied fixed stack buffer (`char[256]` at all 6 call sites:
  ext2_write_file/create/mkdir/unlink/rename x2) with NO length bound
  (`for (i=0; i<last_slash; i++) parent_out[i]=path[i]`).  sys_open copies the
  user path into `char[512]` and, on O_CREAT, calls ext2_create -> path_split,
  so an unprivileged `open("/<~480 chars>/x", O_CREAT)` wrote ~480 bytes into the
  256-byte stack buffer, smashing the saved return address (RCE-class kernel
  stack overflow). Fixed: added a `cap` (dest size) argument; path_split returns
  NULL if the parent does not fit (last_slash >= cap) -- every caller already
  treats NULL as an error; bumped all 6 buffers to EXT2_PATH_MAX (512) so a path
  sys_open accepts is never spuriously rejected, and pass sizeof(buf) as cap (so
  the copy is bounded regardless of buffer size). Deterministic
  ext2_path_split_selftest (normal split, root parent, exact-fit cap 9 vs
  reject cap 8, long-parent reject). HIGH confidence. (Found by a read-only
  subagent; independently verified all 6 caller buffer sizes, the NULL-return
  handling, the sys_open 512-byte path reaching ext2_create, + full build/boot.)

- **io_uring user-writable ring_mask OOB read+write**
  (`kernel/io/io_uring.c` io_uring_post_cqe / io_uring_enter_impl / SQPOLL) ->
  FIXED (F33), was CRITICAL. io_uring indexes the sqes[]/cqes[] arrays with
  the ring `mask` read from the USER-MAPPED, USER-WRITABLE ring header
  (sq_hdr->ring_mask / cq_hdr->ring_mask, mapped VMA_R|VMA_W|VMA_USER) instead of
  the kernel-trusted sq_entries/cq_entries.  A process that overwrites
  ring_mask=0xFFFFFFFF in its mapped ring gets an arbitrary OOB read of an SQE
  and an attacker-controlled OOB WRITE of a CQE past the allocation (CRITICAL).
  The adjacent fullness check correctly uses the TRUSTED count
  (`(tail-head) >= cq_entries`), proving the mask used for the subscript is the
  wrong source.  Sites: the mask read at ~:309 (cq) / ~:1005 (sq) / ~:543
  (SQPOLL) feeding `cqes[tail & mask]` (~:366), the overflow-drain (~:321),
  `sqes[head & mask]` (~:1013/:1057), io_wq_enqueue_chain (~:785).  Fix: never
  trust the user mask for kernel indexing -- derive the masks from the
  kernel-set, power-of-two sq_entries/cq_entries (io_sq_mask = sq_entries-1,
  io_cq_mask = cq_entries-1) and use those at every sqes[]/cqes[] subscript;
  setup already clamps entries to a power of 2 <= IO_URING_MAX_ENTRIES=4096.
  Deterministically testable (a pure index-in-bounds helper + assert overwriting
  ring_mask no longer changes the computed slot). Touches ~8 index sites --
  careful but contained; VERIFY each site against real code before editing.
  Good NEXT item (CRITICAL severity, clean fix once verified).

## SIXTH AUDIT PASS continued (2 re-run agents) — 2 more findings

- **sys_spawn SPAWN_ATTR_CRED root privilege escalation**
  (`kernel/syscall/syscall.c` sys_spawn) -> FIXED (F31), was CRITICAL.
  The SPAWN_ATTR_CRED block sets the child's uid/gid to attacker-chosen values
  with NO caller-privilege check: `child->cred.ruid=euid=suid=a.uid;
  child->cred.r/e/sgid=a.gid;` where `a` is copied verbatim from the user attr
  pointer. So ANY unprivileged process calls
  spawn("/bin/x", argv, envp, stdio, {flags=SPAWN_ATTR_CRED, uid=0, gid=0}) and
  the child runs with euid=ruid=suid=0 = full effective root (vfs_check_perm
  root-bypasses DAC; every cred.euid==0 / cred_is_root gate opens). Trivial local
  privilege escalation, no preconditions. The intended caller is the uid=0
  svcmgr dropping privilege DOWN; the kernel never verifies the caller is
  entitled to the requested creds, so it works UP. Contrast: the pledge/unveil
  fields in the same attr block ARE tighten-only (down-only is the documented
  intent for those), but cred has no gate. Fix: before applying SPAWN_ATTR_CRED,
  require cred_is_root(caller) OR (a.uid in {ruid,euid,suid} AND a.gid in
  {rgid,egid,sgid} / supplementary) -- the same down-only rule as cred_setuid /
  sys_setreuid; on failure tear down the child and return -EPERM (do NOT silently
  continue). Pure-helper testable: spawn_cred_allowed(caller, uid, gid) -> caller
  {1000,..} requesting uid 0 = deny, requesting uid 1000 = allow, root = allow
  any. Confidence HIGH. (FIX THIS NEXT.)

- **init->children plain-store vs cross-CPU CAS race** -> FIXED (F34)
  (`kernel/syscall/syscall.c:~655` sys_exit + `kernel/proc/signal.c:~427`
  signal_terminate, the `g_init_task == g_current` branch) -> OPEN (HIGH, but
  init-exit is rare). init->children is a lock-free Treiber stack whose contract
  (documented at sched.c:879-882) requires EVERY mutation to be an atomic CAS,
  because exiting tasks on other CPUs CAS-prepend their orphan chains onto
  init->children (task_child_add_chain) to reparent. But when init (= /bin/login,
  the first non-kthread sched_add, which is exitable / fatal-signalable) itself
  exits, both exit paths clear init->children with a PLAIN non-atomic store
  (`g_current->children = NULL;`), racing those CAS-prepends -> torn list / a
  reparented orphan chain dropped (unreapable task_t + pid leak) + dangling
  children pointer in a task_t about to be RCU-freed. Fix: replace the plain
  store with `__atomic_exchange_n(&g_current->children, NULL, __ATOMIC_ACQ_REL)`
  (the same XCHG-drain task_children_reap already uses), ideally hoisted into one
  shared task_children_clear() helper so the two exit paths can't drift (DRY).
  Race-class -> code-proof (grep shows no remaining plain ->children stores
  except pre-publication inits) + clean boot. Confidence HIGH.
- **setuid-on-exec not implemented (ksec escalation unwired)** -> FIXED (F32)
  (`kernel/security/perm.c` vfs_check_exec + `kernel/syscall/syscall.c` sys_exec
  + `kernel/proc/elf.c` elf_exec_from_ext2). was a real fail-closed item (per explicit
  user direction: every gap is worth fixing). Currently FAIL-CLOSED (a setuid
  binary runs with euid UNCHANGED -- no escalation, no vulnerability today; the
  system is over-restrictive). Two parts: (1) BUG: both exec callers build
  inode_perm_t with `.mode = i_mode & 0x1FF`, which strips the setuid bit
  (S_ISUID=04000, above 0x1FF), so vfs_check_exec's `(ip->mode & S_ISUID)` test
  can never fire -> setuid_uid is always 0xFFFFFFFF (detection is dead code).
  (2) FEATURE: even with detection working, both callers DISCARD setuid_uid and
  never apply the escalation. Fixing = implementing privilege ESCALATION, so do
  it CAREFULLY in a dedicated turn (a botched escalation IS the F31 bug class).
  Plan, per the system's own design (cred.h: escalation requires ksec; the
  mechanism already works for sys_setuid at syscall.c:3492 via ksec_agent_present
  + KSEC_OP_SETUID): (a) pass the full mode (include 04000/02000) to
  inode_perm_t; (b) when vfs_check_exec returns setuid_uid != 0xFFFFFFFF, do the
  SAME ksec round-trip sys_setuid does and, only if ksec AUTHORIZES, set the
  child's euid (and suid) to setuid_uid (keep ruid) -- applied to g_current in
  sys_exec (current process) / to the new child in elf_exec_from_ext2; if ksec
  denies or is absent, exec WITHOUT escalation (preserve today's fail-closed
  behaviour, do NOT fail the exec). Deterministically test the pure decision
  (apply-iff-authorized) helper. Do this as a focused turn -- it is the kind of
  code that must NOT be rushed.

## ksec wiring audit (2026-06-25, prompted by user) — NOT vulnerabilities

Verified every credential-changing path is fail-closed: sys_setuid/seteuid
escalate ONLY via ksec (KSEC_OP_SETUID/SETEUID); sys_setgid/setegid return
-EPERM on escalation (no path offered); sys_setreuid/setregid restrict non-root
to the POSIX in-set {ruid,euid,suid}; sys_setgroups is root-only; exec setuid is
now ksec-gated (F32). No ungated escalation remains (SPAWN_ATTR_CRED was the one
hole, fixed in F31). The following ksec opcodes are DEFINED but UNWIRED -- they
are OVER-RESTRICTIVE (fail-closed/safe) FEATURE gaps, NOT security holes; wiring
them would EXPAND what policy can grant and must be done very carefully (a
botched grant = the F31 bug class). Low priority, do NOT treat as urgent.

- **KSEC_OP_RESOURCE_FD (4) unwired** -> OPEN (feature, fail-closed). Privileged
  resources (framebuffer map, sys_register_policy_agent, sys_setgroups, the root
  gates at syscall.c:3023/2524) use direct cred_is_root checks; the ksec
  resource-fd grant path is never used, so ONLY root can obtain them. Wiring it
  would let policy grant a specific resource fd (with attenuated rights, the
  ksec_response_t.granted_rights field) to a specific non-root process. Adds a
  grant path -- careful.
- **KSEC_OP_CLRUID_BIT (6) unhandled** -> OPEN (feature, fail-closed). ksec can
  request the kernel clear a setuid bit on an inode (anti-tamper after policy
  revokes a binary); no kernel handler exists, so the request is a no-op. Also
  blocked today because sys_chmod can't persist a setuid bit (TODO stub,
  syscall.c:~4665). Wiring needs ext2 setuid-bit persistence first.
- **setgid/setegid have no ksec escalation path** -> OPEN (feature, fail-closed).
  Asymmetric with setuid/seteuid (which ask ksec): a non-root process can never
  gain a gid via policy (returns -EPERM). For symmetry, sys_setgid/setegid could
  do the KSEC_OP round-trip on the escalation case like sys_setuid does. Adds a
  grant path -- careful. (KSEC_OP_SETUID_BIT (5) IS wired: sys_chmod notifies
  ksec on u+s, though chmod's persistence is a separate TODO.)

## SEVENTH AUDIT PASS (5-agent read-only fan-out, 2026-06-25)

- **ext2 unbounded physical block number -> wild device LBA** -> FIXED (F36).
  Block numbers taken from untrusted on-disk data (inode i_block[], single/double
  indirect-block entries, BGD bg_inode_table) were turned into device LBAs
  (lba = s_part_lba + blk*spb) with NO upper bound: s_blocks_count was read at
  mount but only used to derive s_num_groups, never stored or compared, and no
  block_valid() helper existed. A crafted/corrupt ext2 image (or on-disk
  corruption) with a wild i_block[] or indirect entry made the kernel DMA a
  sector OUTSIDE the filesystem -- cross-partition read / off-device access on
  read(), and an OOB WRITE on inode/bitmap writeback. Fixed: store s_blocks_count
  at mount; add pure, unit-tested helpers ext2_block_in_range / ext2_run_in_range
  (overflow-safe) behind ext2_block_valid / ext2_run_valid; gate EVERY block ->
  LBA sink -- bcache_get (read), write_block (write), and both fast-path run DMAs
  (ext2_vfs_read / pread) validate the whole consecutive run. Also rejected a
  degenerate superblock (s_blocks_per_grp / s_inodes_per_grp / s_blocks_count == 0
  -- the first two were a div-by-zero DoS at mount). Deterministic
  ext2_block_valid_selftest (boundary + overflow-safe run) + clean boot to login
  (all userland is read from ext2 through the gated sinks -> no over-rejection).
- mremap / mprotect / madvise / msync -> SAFE (NOT IMPLEMENTED; no syscall nrs).
  If added later they MUST route addr/old_size/new_size through mmap_range_ok
  before any vmm_* call, like sys_munmap, or they reintroduce the F24 class.
- ptrace -> SAFE (NOT IMPLEMENTED; no cross-process peek/poke/getregs surface).
- AHCI / NVMe DMA descriptor construction -> SAFE as reached. PRDT bounded by
  nprdt < 248; ext2 pre-clamps transfers to AHCI_DMA_SECTORS; NVMe rejects >8KB;
  AHCI slots are derived from the hardware SACT/CI ctz (never device-echoed);
  NVMe cid is F26-guarded. Latent AHCI submit self-guards -> FIXED (F43):
  ahci_submit_hhdm issued the command even when the PRDT build loop exited with
  leftover rem (transfer > 248 PRD entries) -> silent truncation; now `if (rem)`
  frees the un-issued slot (OR back into s_free_mask + wake s_slot_avail_wq) and
  returns 0, mirroring do_rw_direct. ahci_submit_sg filled a phys_pages[130]
  stack array with `for (i < npages)` trusting the caller -> a stack overflow if
  npages > 130; now self-guards `if (npages > 130) return 0` up front. Both
  callers already clamp (ext2 -> <=1024 sectors, ahci_read_user -> npages<=130),
  so latent, but the submit functions no longer trust their inputs. Code-proof +
  clean boot (every ext2 read at boot exercises both submit paths).
- virtio-gpu / DRM transfer/scanout/backing -> SAFE. The kernel sends fixed
  x=y=offset=0 full-surface (w,h) from each resource's own create dims; all three
  resource-creating paths (CREATE_DUMB F23, ADDFB2 subset-clamp, CURSOR 64-bit
  footprint check) tie the declared footprint to a backing that covers it. DRM
  MODE_ATOMIC unbounded per-object counts[i] -> FIXED (F40): the inner prop loop
  `for (j < counts[i])` (drm.c) ran with an attacker-supplied per-object count,
  so counts[i]=0xFFFFFFFF drove up to 2^32 copy_from_user iterations (DoS) and
  props_off/values_off `+= counts[i]*4/8` could overflow. Fixed: bound each
  counts[i] to DRM_MAX_PROPS_PER_OBJ (256, generous vs the <32 a real CRTC/plane
  carries) right after the counts[] copy -> -EINVAL otherwise. Pure
  drm_atomic_count_ok helper + deterministic drm_atomic_count_selftest.
- shm/shmem -> SAFE (string-named, RCU + tryget + per-frame refcount + size cap).
  VFS mount table -> SAFE (no sys_mount/umount; static const mount array).

## EIGHTH AUDIT PASS (5-agent read-only fan-out, 2026-06-25)

- **fd_install inconsistent failure-ownership -> double-free / fd leak** -> FIXED
  (F37). fd_install closed `f` on its table-grow failure path but NOT on its
  !g_current path, so callers couldn't tell who owned `f`. ~10 of the 14 callers
  closed `f` on failure (openpty, io_uring x2, eventfd, timerfd, recvmsg(SCM),
  epoll, socketpair x2) -> DOUBLE-FREE / UAF with the grow path; 4 (socket,
  accept x2, recvfd) relied on fd_install closing -> would LEAK on the !g_current
  path. Reachable when fd_table_grow fails (fd-table exhaustion / OOM) during any
  fd-returning syscall. Root-cause fix (one consistent contract): fd_install
  NEVER closes `f`; on ANY negative return the CALLER owns and closes it. Removed
  the grow-path vfs_close; added the missing close to the 4 non-closers; the 10
  closers are now correct unchanged. Also fixed two pre-existing socketpair leaks
  (the fd0 slot on fd1-install failure, and both fds on copyout failure -- the
  comment claimed "close both" but didn't). Verified by an exhaustive code-proof
  (every one of the 14 sites closes `f` on its failure branch; fd_install on
  none) + clean boot (the success path -- every fd at boot, sockets/accept via
  wayland -- is unregressed). The OOM failure path is not reproducible at boot,
  so no bespoke selftest (F34 precedent).
- signal sigframe / sigreturn -> SAFE. The classic ring-0 LPE is closed: CS/SS
  are hardcoded RPL-3 selectors on every iretq (never sourced from the sigframe),
  RFLAGS is masked to condition codes + forced IF (`& 0xCD5 | 0x202`), RIP/RSP
  are canonical-checked, the frame copy is bounds-checked copy_from_user, the
  frame-setup write validates the user rsp range + VMA coverage, and FXRSTOR
  MXCSR is sanitized. No finding.
- network RX (eth/arp/ipv4/udp/tcp/icmp + virtio-net) -> SAFE. The RX skb is
  allocated at the clamped real frame length, so every wire length field
  (ihl/tot_len/udplen/tcp doff) is validated against the true buffer; no IP
  reassembly, TCP is in-order-only with a masked+clamped ring; virtio descid is
  F22-guarded. LOW: no ICMP echo rate-limit (DoS pressure). DHCP is parsed in
  userland, not the kernel.
- fork/clone/wait/zombie -> SAFE (memory-safety). Double-reap closed by the
  XCHG-drain of children (one winner) + RCU-deferred task free; F34 holds. MED
  (POSIX-correctness) -> FIXED (F41): wait() was per-thread not per-process --
  the children Treiber stack was anchored on g_current, so a child forked by
  thread T1 was unreapable by T2's wait(). Fixed by anchoring children on the
  thread-group LEADER (pid == tgid): a tg_leader(t) helper resolves any thread
  to its leader via pid_ht_find(tgid) under rcu_read_lock (pins it, no UAF, the
  F16/F17 pattern; g_current fallback so never NULL/dangling), used at fork +
  spawn (task_child_add) and wait (task_children_reap). task_children_reparent's
  drain was made atomic (XCHG) since threads can now CAS-add to the leader's
  list during its exit-reparent (restores the F34 all-atomic invariant). The
  rare straggler-leak (a thread forking onto an already-exited leader before it
  is reaped) is closed by a safety-net reparent-to-init in task_destroy (a
  no-op for normal reaps). Deterministic tg_leader_selftest (fake leader+thread
  +zombie-child: a child "forked by the thread" is reaped via tg_leader). LOW:
  sys_thread does not zero the task_t (slab-drift risk) -> FIXED (F42): sys_thread
  was the one task constructor that kmalloc'd a task_t and set fields one by one
  with NO `__builtin_memset(t, 0, sizeof(*t))`, unlike task_fork / task_create_user
  / task_create_kthread (process.c:252/286/417) -- so any field not explicitly set
  inherited slab garbage (a pointer field added later would be wild). Added the
  memset right after the kmalloc, restoring the zero-then-init invariant. Code-
  proof (matches the 3 siblings) + clean boot.
- VFS path resolution -> no symlink support (no ELOOP surface). **HIGH: unveil()
  was enforced ONLY in sys_open** -> FIXED (F38). unveil_check had one call site;
  every other path syscall escaped the sandbox. Fixed with ONE shared gate
  (unveil_ok, syscall.c) wired into open (refactored), unlink, rename (both
  endpoints), mkdir, truncate, exec, stat, access -- each normalizes the path
  (the mutating ones call normalize_path; stat/access/open via fs_lookup's
  in-place normalize) BEFORE the gate so a `..` cannot escape the visible prefix,
  with correct bits (CREATE for entry create/remove, WRITE for truncate, EXEC for
  exec, READ for stat/access). Deterministic unveil_gate_selftest. sys_rename's
  raw __builtin_memcpy of the user path was also replaced with copy_from_user.
  Not gated (recorded): chmod/chown are no-op stubs; chdir is navigation-only
  (its resolved file ops are gated on the absolute path, so no bypass).
  Raw path-copy deref -> FIXED (F39). sys_open/exec/spawn/access/truncate copied
  the path with a raw `while (upath[i])` deref (no _access_ok -> a kernel/non-
  canonical/unmapped path pointer #PF-panicked the kernel = DoS, or read kernel
  memory into the path buffer). Added one shared copy_path_from_user(dst,uptr,
  dstsz) built on copy_from_user (_access_ok + VMA-checked prefault), page-
  granular so it stops at the NUL without over-reading into an unmapped next
  page; returns length / -EFAULT / -ENAMETOOLONG (no silent truncation). All 5
  sites converted (sys_chmod's path copy was dead code -> removed). Deterministic
  copy_path_user_selftest (rejects kernel/non-canonical/NULL pointers) + clean
  boot (every exec/open/stat copies a real path through it).
- ELF loader (beyond F13) -> SAFE as reached. phdr table + PT_LOAD range are
  overflow-checked (elf_phtab_in_bounds / elf_seg_range_ok), argv/envp stack is
  capped (MAX_ARGS/MAX_ENVS + VMM_USER_STACK_PAGES), no PT_INTERP/dynamic loader
  exists. LOW/latent: no p_filesz<=p_memsz check; the eager-path `data+file_off`
  overflow guard is dead code (every caller uses the lazy backing-file path).
- AF_UNIX SCM_RIGHTS -> fd-array counts bounded (SCM_MAX_FD), sender refcount and
  queue-vs-teardown ownership correct. The recvmsg double-close it flagged is
  subsumed by the F37 fd_install fix (fd_install no longer closes, so recvmsg's
  vfs_close is now the single correct close).
- ext2 device-LBA wrap -> FIXED (F46): bcache_get/write_block formed `uint32_t lba
  = s_part_lba + blk * s_sectors_per_blk` (u32) before passing it to the uint64_t
  ahci_read/write. blk is bounded only by the untrusted on-disk s_blocks_count, so
  blk*spb (spb<=8) overflows u32 and WRAPS to a valid wrong sector (crafted-image
  info leak/corruption; also breaks >2 TB-class FS). Fixed by the new pure primitive
  ext2_blk_lba(part_lba,blk,spb) forming the LBA in u64 (product < 2^35 < u64, no
  ckd_mul needed -- correct WIDTH not a check) + ext2_blk_to_lba wrapper; deterministic
  ext2_blk_lba_selftest drives the wrap boundary. COMPLETED in a follow-up: a
  verify-all-angles "same class elsewhere" sweep found two MORE sites of the identical
  bug (the inode_build_run DMA read paths, ~1202/1299, using `phys_blk`) that the first
  grep missed; all FOUR ext2 LBA sites now go through ext2_blk_to_lba (zero `uint32_t
  lba` remain). Separate layer (recorded, not this fix): ahci_read/write do not bound
  lba vs device size.
- AHCI sector->byte size wrap -> FIXED (F47): ahci_submit_hhdm / ahci_submit_sg /
  ahci_read_user formed `uint32_t bytes = count * 512` then bounded the DMA by a
  byte-derived guard (npages<=130 / 248-PRDT rem-check) while issue_cmd got the raw
  count. A count >= 2^23 wraps count*512 to a small value -> the byte guard passes but
  the HBA moves the huge count -> DMA overrun. LATENT (every caller today caps count
  <= AHCI_DMA_SECTORS=1024; F40-class defense-in-depth on an exported API). Fixed by
  the new pure primitive xfer_bytes_ok(sectors, sector_size, max_bytes, *out) forming
  the length in u64 + a bound; applied at all three sites (read_user's max is the exact
  npages<=130 equivalent, so the redundant check was deleted). do_rw_direct left as-is
  (n capped at 65535). xfer_bytes_ok_selftest drives the 2^32 wrap. SAME-CLASS sibling
  recorded, not fixed (nvme not boot-verifiable): nvme_rw `nlb * s_ns_lba_size`.
