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
  (n capped at 65535). xfer_bytes_ok_selftest drives the 2^32 wrap.
- nvme transfer-size wrap -> FIXED (F48, the F47 nvme sibling): nvme_rw `nlb *
  s_ns_lba_size` (u32) gated the 1-vs-2-PRP (<=8KB) layout while cdw12 sent a 16-bit
  MASKED sector count; a huge nlb wraps the byte length small (single PRP) but cdw12
  carries a large count -> 32 MB DMA into a 4 KB PRP -> OOB. Latent (only the nlb=8
  stress caller). Extracted the cross-cutting generic mul_within_u32(a,b,max,*out) into
  checked.h (form a*b in u64, accept iff <= max); ahci xfer_bytes_ok now wraps it
  (single source) and nvme_rw uses it (max 8192) keeping its pg_off-aware glue. Covered
  by the mul_within cases in checked_selftest; nvme path not boot-exercised.
- virtio-gpu scanout backing under-allocation -> FIXED (F49): vgpu_setup_scanout_buffer
  sized the framebuffer `uint32_t bytes = w * h * 4` (u32) then told the GPU the resource
  is the full w*h (+ the present paint loop writes w*h pixels). w/h are the DEVICE-reported
  scanout mode (s_scanouts[0], untrusted); a large/crafted mode wraps w*h*4 -> tiny backing
  but full-size GPU access -> host OOB read/write. Fixed via the new tagged primitive
  vgpu_fb_bytes(w,h,*out) -> mul_within_u32 (u64) capped at 256 MiB, rejecting 0-dim /
  over-cap (setup returns 0; both callers + the paint loop gate on it). drm.c size math
  already u64 (not in scope); fb.c glyph offset is kernel-controlled (out of class).
  vgpu_fb_bytes_selftest drives the 2^32 wrap + over-cap rejects; boot inits GPU at
  1280x800 unchanged.
- ext2 inode straddles bcache slot -> FIXED (F50): inode_load_into memcpy's
  sizeof(ext2_inode_t)=128 bytes from `r.data + off` and inode_disk_write to
  `scratch + off`, off = (local*s_inode_size) % s_block_size. s_inode_size is the
  untrusted superblock field taken with only a `> 0` check; a crafted value (not a
  power of two / < 128 / > block, e.g. 4032) makes off land near the slot tail so
  off+128 > block_size -> OOB read (info leak into the inode) + OOB write (heap
  corruption on writeback). Root fix: new pure tagged primitive ext2_inode_size_valid
  (power-of-2, >= sizeof(inode), <= block_size -> inode divides block, off+128 <=
  block) gates the mount (ext2_init returns 0). Sibling of ext2_block_size_checked.
  Other r.data+off memcpys are fixed-stride (bgd) or already clamped (dirent/file).
  ext2_inode_size_valid_selftest drives the reject cases; boot still mounts + loads
  /bin/login (real s_inode_size valid).
- socket syscalls deref raw user pointer -> FIXED (F51, HIGH/LPE): sys_recvfrom /
  sendto / accept / connect / bind passed the raw user buf_ptr / addr_ptr (only
  !NULL-checked) to code that derefs them DIRECTLY -- tcp_recv_data `dst[i]=` into
  buf_ptr, UDP/unix recv memcpy into buf_ptr, socket_accept writes peer sockaddr,
  connect/bind read sa->sin_*/sun_path -- none via _access_ok / user_buf_check /
  copy_*_user (the rest of the kernel does: sys_read prefaults, io_uring uses
  user_buf_check). `recvfrom(s,(void*)KADDR,n)` = arbitrary-kernel-WRITE of n
  attacker-chosen bytes (LPE: cred/funcptr/PTE); send/sendto buf_ptr=KADDR = kernel
  READ leak; the clamps only bound the count, not the destination. Fix (whole class,
  all 5 handlers): user_buf_check(buf_ptr,len) + user_buf_check(addr_ptr, sizeof
  sockaddr_un/in per the is_unix_sock branch) before each deref. Also covers the
  io_uring socket opcodes (they forward here). Boot DHCP over sendto/recvfrom still
  gets its lease (10.0.2.15) -> valid path unbroken. Residual (pre-existing, kernel-
  wide like sys_read): recv copy is post-block -> TOCTOU munmap could #PF; the range
  check still blocks the kernel-write; bounce-buffer is the fuller later hardening.
- user_buf_prefault overflow + sys_write raw deref -> FIXED (F52): (1) user_buf_prefault
  (the chokepoint for prefault-only callers) computed `end = (addr+len+0xFFF)&~0xFFF`; a
  huge len wraps it below `page`, the loop runs 0 times, returns 0 ("ok") validating
  nothing. sys_read (prefault-only, no _access_ok) -> read(sockfd, KADDR, ~0ull) ->
  tcp_recv_data writes the datagram to KADDR = arbitrary-kernel-WRITE LPE (F51 via read()).
  copy_*_user are safe (they _access_ok first). Root fix: reject wrapping addr+len
  (ckd_add_u64), iterate by PAGE COUNT -> overflow-proof, identical page set for valid
  ranges. (2) sys_write deref'd buf raw (sock_write->tcp_send) = write(sockfd,KADDR,n)
  kernel-READ leak; fixed with _access_ok range-check (NOT prefault: a write READS the
  buffer, so prefault zeroing absent pages is the wrong direction + wedged userspace --
  caught by a revert-test, not assumed flaky). Boot DHCP over read/write+sockets still
  gets its lease. Residual: a raw-deref write_op on an absent valid page relies on the
  demand-fault path; bounce-buffer is the fuller hardening.
- ioctl TIOCGSERIAL raw user-pointer write -> FIXED (F53, HIGH/LPE): the tty0 ioctl
  fallback zeroed a 60-byte serial_struct stub via `memset((uint8_t*)arg, 0, 60)` on the
  raw user `arg` (only !NULL-checked), while every sibling case (TIOCGWINSZ/TCGETS/...)
  uses copy_*_user. ioctl(fd, TIOCGSERIAL, KADDR) -> 60-byte zero-write to ANY kernel
  address (zero a cred=root / funcptr / lock = LPE) or #PF panic (unmapped = DoS);
  reachable unprivileged on fd 0/1/2 (tty0 fds have ioctl==NULL -> this fallback). Found
  by fan-out audit, confirmed + kernel-wide grep showed it was the ONLY raw-arg deref in
  any ioctl handler. Fix: copy_to_user the zero struct (validates via _access_ok +
  prefault) like the siblings. Boot reaches DHCP lease, no regression.
- ext2 dir_add_entry slack underflow -> heap OOB write -> FIXED (F54, HIGH): the dirent
  walk computed `slack = de->rec_len - actual_len` (u32) on an UNTRUSTED on-disk
  rec_len/name_len, bounding only `off+8<=blk_bytes`. A corrupt entry with actual_len >
  rec_len (e.g. name_len=255 -> actual_len=264, rec_len=16) underflows slack to ~4.29e9
  so the split always fires and writes new_de = dae_buf + off + actual_len (off up to
  ~4088) PAST the 4096-byte heap block = heap OOB write of the new filename/inode ->
  slab corruption / LPE. Reached by any create (O_CREAT/mkdir/rename/link) in a crafted/
  corrupt directory block (read-side dir_lookup/readdir already clamp via
  ext2_dirent_namelen_clamp; write-side had no guard). dir_remove_entry is the OOB-read
  sibling (kmemeq past the buffer). Fix: tagged primitive ext2_dirent_in_block(off,
  rec_len, name_len, blk_bytes) = off+rec_len<=blk_bytes && align4(8+name_len)<=rec_len,
  break the walk on a malformed entry in both (dir_add appends a fresh block, dir_remove
  fails-to-find). Valid dirs unaffected. ext2_dirent_in_block_selftest drives the rejects.
- unveil sandbox escape (create before check) -> FIXED (F55): sys_open gated unveil_ok
  only at got_file -- AFTER ext2_create -- while every other path syscall gates right
  after fs_lookup. So a confined (unveil'd) process could open("/outside", O_CREAT): the
  create ran, then the late check denied + vfs_close'd the fd but left the file on disk =
  filesystem-confinement escape on the create side (deny-by-side-effect; caller gets
  ENOENT yet the file exists). Unprivileged, single syscall. Fix: move the gate to right
  after fs_lookup, before any open/create (path is normalized by fs_lookup; /dev,/proc +
  no-rules exempt inside unveil_ok); removed the redundant got_file check (the early gate
  dominates all got_file paths). Boot reaches DHCP, no regression (zero boot denials -- the
  gate is a no-op for un-unveiled processes). NOTE: a 1-sample revert-test misleadingly
  pointed at this change during a flaky-boot stall; a denial-tracing debug (zero denials)
  was the decisive check -- confirm the mechanism, not just one revert sample.
- ext2_write_file overwrite race -> FIXED (F56): the overwrite branch read the inode as a
  read_inode SNAPSHOT (lock released), then ran free_inode_blocks + alloc/write/set-block
  with NO inode lock held, taking inode_lock only for the final writeback. Two concurrent
  writers to the SAME file (two O_TRUNC opens, ftruncate+write) both snapshot the same
  i_block[], both free_inode_blocks: the second frees a block the first already freed AND
  re-allocated -> a live block returns to the bitmap and is handed to another file = two
  inodes sharing a physical block (cross-file corruption; bitmap_test stops a pure double-
  free, not free-of-a-reallocated-block). Fix: hold inode_lock(existing_ino) across the
  whole free->realloc->writeback on a LOCAL copy (error-safe, commit on success only).
  Verified (no-assumptions) that inode_lock = seq_write_begin which takes the per-leaf
  write_lock spinlock (serialises writers), same lock order as the existing writeback (no
  cycle); scratch alloc'd before the lock (EXT2_SCRATCH_ALLOC's embedded return-on-OOM
  would leak it); single unlock+return. Boot DHCP + 0 fault, no deadlock; the race is not
  boot-reproducible -> code-proof (serialisation) + clean-boot. Closes recorded follow-up (a).
- exited pthread freed while still linked in signal-routing tables -> FIXED (F57): every
  task is linked into the pgid/tgid/sid intrusive lists by task_idx_insert (sched_add), but
  the ONLY remover task_idx_remove was called solely from sched_add_zombie. A pthread
  (TASK_FLAG_THREAD) exiting via sys_exit sets TASK_DEAD and skips sched_add_zombie (by
  design -- a thread is futex-joined, not wait()ed), so the reaper -> task_destroy ->
  RCU kfree frees the task_t while it is STILL linked in those lists. task_destroy unlinked
  pid_ht but not the routing tables, so the freed node stays permanently reachable;
  signal_send_pgrp / signal_send_group walk it and deref t->next/t->state from freed memory
  (once the slab is reused, the state guard passes and signal_send runs on a corrupted
  task_t = arbitrary kernel write). RCU does not help: it only defers the free of an
  UNLINKED node; this node is never unlinked. Reachable by any >=2-thread process where a
  thread exits, then any tty job-control signal / kill(0) / kill(-pgid) to it -- exactly the
  threaded-compositor pattern. Fix: move task_idx_remove(t) into task_destroy's synchronous
  phase beside pid_ht_remove -- the universal final-free chokepoint both exit paths reach --
  so a task_t is never freed while linked, for every current and future exit path.
  task_idx_remove is idempotent (zombie path already removed early; second call is a no-op);
  lock-safe (table lock > rq_lock, and task_destroy holds no rq_lock at either site). Boot
  DHCP + 0 fault, all selftests pass, no deadlock; UAF race not boot-reproducible -> code-
  proof (single-source idempotent unlink before the deferred free) + clean-boot. RECORDED
  follow-up (e): the fatal-signal path (signal.c:451-454) zombifies a THREAD instead of
  self-reaping it as TASK_DEAD -- a per-thread task-struct/pid LEAK (the very thing the
  sys_exit thread branch avoids), not a memory-safety bug; investigate separately.
- SOCK_DGRAM connect() default destination not refcounted -> FIXED (F58): the DGRAM connect
  branch cached `s->peer = target` with no unix_get and no back-pointer (asymmetric: target
  does not point back, so its close cannot clear the senders' cached pointers like a
  symmetric stream/socketpair peer's close does). So connect A->B, close B (RCU-freed,
  A->peer left dangling), then sendto(A,...,NULL) derefs freed B (type, dgram_tail enqueue,
  wake walking B's freed waitq/->file) = DETERMINISTIC unprivileged UAF -> arbitrary kernel
  corruption via a reused slab. RCU does not help: the pointer is cached at connect and
  survives grace periods, so a fresh rcu section in sendto cannot resurrect long-freed
  memory. Fix (reuse the refcount machinery): a SEPARATE owned-ref field `dgram_dest` (not
  the symmetric `peer`) -- connect unix_get()s it (drops the old on reconnect, publish-then-
  put), close unix_put()s it, send uses `dgram_dest ? dgram_dest : peer`. A separate field
  (not a flag on `peer`) avoids a new UAF in the socketpair-then-connect and mutual-connect
  cases (verify-all-angles). Behavior-changing -> a deterministic selftest
  (unix_dgram_peer_selftest) asserts the lifecycle: connect ref+1, reconnect drops old/takes
  new, THE INVARIANT (destination survives its own close while a sender references it ->
  refcount==1, not freed), sendto-after-dest-close returns 2, final close balances. Boot:
  `[unix_dgram_peer] SELF-TEST PASSED`, socketpair + refcount selftests still pass, 0 fault.
  RECORDED follow-ups (verified-real, NOT yet fixed): (f) epoll readiness scan re-resolves
  the watched file by raw `fd_table[w->fd]` + `f->poll(f,...)` with no ref/RCU (wrong lock)
  while the watch already pins `w->file` -> close+reuse on another thread of the same files
  table = UAF / freed-fnptr call; fix: poll through the pinned `w->file` (or fdget/fdput).
  (g) futex_wake (futex.c:158-159) sets w->woken (RELEASE) then derefs w->task; a waiter
  woken by a concurrent timeout/signal returns via the no-lock rc==0 path and can reclaim its
  on-stack node between the two lines -> sched_wake(garbage); fix: capture w->task before the
  RELEASE store.
- futex_wake derefs the waiter node after authorizing return -> FIXED (F59, follow-up (g)):
  the on-stack futex_waiter_t is reclaimable the instant futex_wake stores w->woken=1 (RELEASE)
  -- a waiter made runnable by a concurrent timeout/signal sees it via the lock-free ACQUIRE
  load (futex.c:112) and returns through the rc==0 path, which takes no b->lock. But line 159
  read w->task AFTER that store, so sched_wake could read a reused stack frame -> wild task
  pointer into the scheduler. Reachable on SMP via a wake-all (thread-exit task_notify_cleartid
  -> futex_wake(addr,0x7fffffff)) racing a woken waiter's timeout/signal. Fix: capture
  task_t* wtask = w->task BEFORE the RELEASE store, sched_wake(wtask) -- after the store the
  waker touches only the local, never w->*. Behavior-identical on the normal path (no lost
  wake). Code-proof + clean-boot (boot exercises futexes via pthread mutex/cond). NOTE: first
  verify boot stalled at the slab-pcpu stress test (kthread/RCU, no futex); a re-boot of the
  same build passed -> the known flaky early stall, not a regression (F55 lesson: deterministic
  regressions recur, flakiness does not).
- epoll readiness scan derefs raw fd_table[w->fd] -> FIXED (F60, follow-up (f)): all 3
  epoll scans (epoll_poll ~4609, epoll_wait primary ~4857, post-sleep recheck ~4930) read
  f = fd_table[w->fd] raw and called f->poll(f,...) under only state->lock (not the files
  table tf->lock). A concurrent close() on another thread of a shared files table
  (THREAD_SHARE_FILES) frees that file -> f->poll on freed memory = UAF / freed-fnptr call;
  fd reuse adds a wrong-file poll. The watch already pins the registered file (w->file =
  vfs_tryget at register, vfs_close at unregister; unregister runs under state->lock so the
  pin is alive for the whole scan). Fix: poll the pinned w->file (UAF-safe), and read
  fd_table[w->fd] only as a POINTER VALUE (never dereferenced) to detect close/reuse and keep
  the existing EPOLLERR|EPOLLHUP dangling-fd semantic (cur==w->file -> identical result via
  the pinned ref; cur!=w->file -> HUP instead of a freed/wrong-file poll). Code-proof (poll
  the pinned file, never deref the racy fd_table entry; lock keeps w->file alive) + clean
  boot ([epoll_pin] PASS, 0 fault, no WD, DHCP; sway/wayland exercise epoll). Closes (f).
- shmem pages[] array unsynchronised -> FIXED (F61): shmem_t (MAP_SHARED / POSIX shm
  backing) had no lock on pages/npages/max_pages. shmem_get_page (lock-free from the #PF
  handler; peers hold different per-mm vma_locks) races shmem_resize (ftruncate). The grow
  path does kfree(shm->pages)+realloc, so a concurrent fault dereferences the freed array =
  cross-CPU heap UAF -> reused phys_addr_t -> user PTE to an arbitrary frame (LPE/disclosure).
  Hit by the foot/wayland ftruncate-grow-while-mapped workload. (Per-frame refcounts only
  protected mapped FRAMES from recycle, not the ARRAY realloc.) Secondary: two first-touches
  of one index double-alloc + give two mappers different frames (coherence break). Fix: a
  per-shmem_t spinlock held across all of shmem_get_page and all of shmem_resize (plain
  spin_lock -- process-context only; lock order shm->lock > heap/pmm, never nested with
  vma_lock). Code-proof (realloc and read now serialise) + clean boot (0 fault, 49 selftests
  pass, no WD; wayland exercises ftruncate-grow + faults).
- AHCI submit serialization gap -> FIXED (F62, follow-up (h)): s_submit_busy (the single-
  submitter lock that protects the shared slot-0 s_slot_done/s_slot_result at s_nslots=1) was
  taken only in ahci_submit_hhdm. ahci_submit_sg (ext2 user-buffer reads via ahci_read_user)
  and ahci_read_multi (read-ahead) skipped it, so a concurrent kernel-buffer + user-buffer
  read collide on slot 0's completion state -> spurious retry + PRDT rebuild misdirects
  in-flight DMA (data corruption / wrong-buffer read). The _sg path bypasses the bcache, so
  no higher lock serializes it; s_submit_busy's existence proves there is no higher serializer.
  Fix (DRY): extract submit_busy_acquire/release and bracket the slot lifecycle in all 3
  submit paths (read s_slot_result under the lock; read_multi brackets the whole batch). No
  new deadlock (IRQ completion never takes s_submit_busy; lock order s_submit_busy > s_ci_lock
  unchanged). Code-proof + clean boot (0 fault, 49 selftests, no WD; boot does disk I/O via
  all 3 paths). First verify boot hit the flaky chaselev-class stall; re-boot reached DHCP.
- io_uring fixed-files unlocked read -> FIXED (F63): sqe_fdget read fixed_files_nr/[idx] + vfs_tryget
  with no fixed_files_lock, on the io_wq worker kthread, racing IORING_UNREGISTER_FILES (swaps
  NULL under the lock, then vfs_close's files + kfree's the array outside it) -> freed-array read /
  freed-file tryget (freed-fnptr via vtable) / torn nr-vs-array. Fix: hold fixed_files_lock across
  the (nr,array[idx]) read + tryget so the read is consistent and the file is pinned before
  unregister's vfs_close. Code-proof + clean boot (io_uring selftests run, 0 fault, no WD).
  RECORDED follow-ups (verified-real, NOT yet fixed): (i) signalfd OWNER UAF -- raw task_t* owner
  outlives the owner via fork/SCM_RIGHTS; task_free_rcu frees the task but never NULLs s->owner ->
  child read/poll/close derefs freed task (signalfd.c:79/114/131/145). Fix: signalfd_disown_all in
  task_free_rcu (NULL owner under s_sfd_lock) + read owner under s_sfd_lock in claim/poll/close +
  EOF on owner-gone. (j) AF_UNIX backlog client UAF -- connect enqueues pend->client with no
  unix_get; client closed-while-CONNECTING is freed but left on the listener backlog; accept derefs
  freed pend->client (unix_sock.c:851-863). Fix: unix_get at enqueue, unix_put + tryget/state-check
  at accept/drain (mirror F58).
- signalfd owner UAF -> FIXED (F64, follow-up (i)): signalfd_state_t.owner is a raw
  non-refcounted task_t*; an inherited (fork vfs_dup) or SCM_RIGHTS-passed signalfd outlives
  the owner, and task_free_rcu frees the task without NULLing s->owner -> child read/poll/close
  derefs the freed task (signalfd.c:79/114/131/145), deterministic on fork+parent-exit+child-
  read. Fix: signalfd_disown_all(t) in task_free_rcu (NULL owner + clear head + wake waitqs
  under s_sfd_lock, before kfree(t)); read every owner deref into a local UNDER s_sfd_lock in
  claim/poll/close (TOCTOU + lifetime: disown needs the same lock, so a reader keeps the owner
  alive); owner-gone -> read EOF / poll POLLHUP (no hang). Deterministic selftest (Case 6:
  disown then read=0 EOF + poll POLLHUP) + code-proof + clean boot (49 selftests, 0 fault, no WD).
- AF_UNIX listen/accept backlog client UAF -> FIXED (F65, follow-up (j)): unix_sock_connect
  (STREAM) enqueued a backlog unix_pending_t with pend->client = s taking NO ref, then blocked;
  a client that bailed while CONNECTING (WAIT_EVENT_HOOK -EINTR on a signal) and close()d was
  RCU-freed but never unlinked from the listener backlog, so a later accept() dereferenced the
  freed pend->client (client->state/peer writes + unix_wake) -> UAF write, and could publish a
  dangling server->peer the raw poll/send/recv peer derefs would chase. The simple owned-ref
  alone is insufficient (peer derefs are mixed raw, so pairing a DEAD client dangles server->peer
  = a new UAF). Fix (owned ref + atomic claim): connect unix_get(s) before pend->client = s;
  accept restructured into a for(;;) that CAS-claims client CONNECTING->CONNECTED (win = client
  provably still blocked, safe to pair+wake; lose = bailed/closed, unix_put-reap and try next);
  connect's -EINTR hook CASes CONNECTING->DISCONNECTED and returns -EINTR only if it wins; the
  free_rcu backlog drain unix_put()s each client; unix_sock_close CASes a still-CONNECTING closing
  socket dead + wakes it (multi-threaded close-wins -> accept skips it). Refcount: one unix_get
  matched by exactly one unix_put across four mutually-exclusive paths. Deterministic selftest
  (unix_stream_accept_selftest: connector kthread + main accept, asserts owned-ref enqueue +
  atomic-claim pairing + drained backlog + balanced teardown) + code-proof + clean boot (51
  selftests, all 5 AF_UNIX tests PASS, 0 fault, no WD, login prompt renders). RESIDUAL follow-up
  (k): the multi-threaded accept-WINS window (accept links server->peer then a concurrent close
  on another thread clears the symmetric back-pointer) needs a per-listener pairing lock; rarer
  than the sequential case fixed here.
- fatal-signal terminate path zombifies a user THREAD -> per-thread task/pid LEAK -> FIXED (F66,
  follow-up (e)): signal_deliver_pending's SIG_DFL/fatal-hw terminate block unconditionally did
  state=TASK_ZOMBIE + sched_add_zombie + SIGCHLD(parent) with no thread-vs-leader check. Correct
  for a process leader (parent waitpid-reaps it), but a TASK_FLAG_THREAD is futex-joined (CLEARTID),
  never wait()ed -- so zombifying it means nothing ever reaps it: the task_t + pid leak forever, and
  the spurious SIGCHLD wrongly wakes the process parent's child reaper. sys_exit ALREADY had the
  right branch (thread -> TASK_DEAD self-reap, reaped at the next do_switch; leader -> zombie), so a
  thread that exit()s was fine but a thread that died by SIGSEGV/SIGKILL leaked -- a drift between
  two paths deciding the same thing. Fix (DRY, one shared mechanism): extracted
  task_set_exit_state(task_t*, exit_code) (+ pure task_exit_self_reaps(flags)) and routed BOTH
  sys_exit and the signal terminate path through it, so they cannot drift again; the leader branch is
  byte-identical to before, only the thread case in the signal path changes. Deterministic selftest
  (task_exit_state_selftest: the decision for every flag combo + a constructed thread task lands in
  TASK_DEAD with no side effects) + code-proof + clean boot (52 selftests, the leader zombify/reap
  path is exercised constantly by login/svcmgr/sway exits, 0 fault, login prompt renders). Separate
  out-of-scope NOTE: MakaOS kills only the faulting thread on a fatal signal, not the whole thread
  group (a POSIX semantic/feature gap, not this leak).
- write(2) raw-deref of an unmapped-but-in-range buffer -> kernel #PF PANIC (DoS) -> FIXED (F67,
  follow-up (c)): sys_write validated buf with _access_ok ONLY (range/LPE guard), then vfs_write ->
  the write_op reads buf with a raw memcpy. A buf in valid user range with NO VMA (p=mmap; munmap(p);
  write(1,p,16)) takes a kernel-mode #PF; isr14_page_fault treats kernel-mode user-address faults as
  legit (vmm.c:669) but on no-VMA does `goto kernel_panic` (vmm.c:791) -> halts the whole kernel. A
  trivial unprivileged DoS. The recorded (c) (absent-but-VALID page) is benign -- the handler
  demand-pages it correctly; the no-VMA case is the reachable bug. F52 used _access_ok-only here on
  purpose (user_buf_prefault maps+ZEROS absent pages, corrupting a file-backed read source -- login's
  .rodata strings -> blank/hang), so the existing prefault could not close the gap. Fix (new
  no-map primitive): mm_range_has_vmas(mm, addr, len) checks every page has a VMA WITHOUT mapping (so
  a valid-but-absent page still demand-faults its real content); user_buf_readable_ok = _access_ok +
  mm_range_has_vmas; sys_write uses it, returning -EFAULT for an unmapped buf before any write_op lock
  (no panic). Deterministic selftest (mm_range_has_vmas_selftest: two VMAs + a gap, fully-inside /
  straddle / in-gap / mid-page / len0 / null-mm / wrap) + code-proof + clean boot (53 selftests; the
  login prompt is a write() of a file-backed .rodata string and renders correctly, proving valid +
  file-backed writes still work and are not zero-filled). RESIDUAL follow-up (l): a concurrent munmap
  between the check and the raw read still panics; the robust fix is a fault-fixup/exception table so
  any kernel-mode fault on a user address returns -EFAULT (would also let copy_*_user/user_buf_check
  drop their file-corrupting prefault for read sources).
- directory rename across parents left ".." and links_count stale -> fs corruption / path-escape ->
  FIXED (F68, follow-up (b)): ext2_rename moved a dir (dir_add_entry dst + dir_remove_entry src) but
  for a directory never repointed its ".." (in block 0) at the new parent -- so /b/d/.. still resolved
  to /a after `mv /a/d /b/d` (path escape) -- and never adjusted links_count: the old parent stayed
  too high (un-rmdir-able), the new parent too low (an over-decrement could free it while this dir's
  ".." still references it -> cross-link onto a freed inode). mkdir already sets ".." + increments the
  parent's links_count, the convention this mirrors. Fix: dirent_repoint_dotdot (no-I/O, rewrites only
  the ".." inode under the ext2_dirent_in_block bound) + dir_set_dotdot (inode-locked block RMW) +,
  in ext2_rename for is_dir && src_parent != dst_parent, repoint ".." and RMW src_parent-- /
  dst_parent++ under each parent's inode lock. Deterministic selftest (ext2_dotdot_repoint_selftest:
  ".." repointed, siblings + rec_len/name_len intact, no-".." -> not-found) + code-proof (links_count
  mirrors mkdir) + clean boot (54 selftests). NOTE: the verify boots hit the flaky F55 userspace-start
  stall (A/B-confirmed pre-existing, 2/3 reach DHCP), and the selftest's 4KiB stack buffer was moved to
  kmalloc (the kstack is only 8KiB). RESIDUAL follow-up (m): ext2_rename does not reject renaming a dir
  into its own subtree (`mv /a /a/b`, POSIX EINVAL) -- creates a detached cycle; needs a normalized-path
  ancestor check.
- rename() of overlapping src/dst corrupts the fs -> FIXED (F69, follow-up (m)): two cases. (1) dir
  into its own subtree (`mv /a /a/b`): with F68 repointing ".." this detaches /a's subtree into an
  unreachable cycle, leaking inodes and looping path walks; POSIX EINVAL. (2) rename onto self or a
  hard-link sibling (`mv /a/f /a/f`, dst_ino == src_ino): ext2_rename's dst-exists branch frees the
  inode + its blocks, then re-adds the dirent pointing at the freed inode -> data loss + dangling
  entry. Fix: pure path_dst_under_src(src,dst) (prefix walk, trailing-'/' boundary, equal != under)
  gates sys_rename -> -EINVAL for the subtree case; ext2_rename returns success-noop when
  dst_ino == src_ino BEFORE the destructive unlink (POSIX same-file no-op, covers hard links). The
  two checks are disjoint (subtree has dst_ino==0). Deterministic selftest (rename_under_selftest:
  child/grandchild -> under; equal/sibling/ancestor/unrelated -> not) + code-proof + clean boot (55
  selftests). Completes the F68 dir-rename hardening.
- AF_UNIX accept()/close() peer-link race -> dangling server->peer UAF -> FIXED (F70, follow-up (k)):
  F65's CAS-claim closed the SEQUENTIAL backlog-client UAF but not a DIFFERENT thread closing the
  shared client fd (connect uses fd_to_file, no vfs ref). accept CAS-claims CONNECTED, THEN allocs the
  server and sets server->peer/client->peer; a concurrent close() that reads client->peer == NULL
  before the link never clears server->peer, so once the client's refs drop it is RCU-freed while
  server->peer still points at it -> later send/recv/poll unix_pin_peer's freed slab. Root flaw: the
  state-claim and the peer-link were not atomic, so "CONNECTED" did not imply "linked". Fix: one leaf
  spinlock s_unix_pair_lock serializes the whole pairing state machine -- accept's [claim+link] as one
  unit, close's CONNECTING-bail + CONNECTED peer-clear, connect's -EINTR bail, the listener-close
  drain -- so a close observing CONNECTED observes a fully-linked pair (and clears it), and a close
  that wins sees CONNECTING and bails (accept's locked claim then fails+reaps). The CAS became a plain
  locked check-and-set (mixing lockless CAS with a locked claim reopens the window); server alloc and
  all wakes/puts stay OUTSIDE the lock (true leaf, no nesting -> no deadlock). Code-proof + clean boot
  (5 AF_UNIX selftests incl. the two-kthread unix_stream_accept rendezvous PASS under the new lock;
  wayland's heavy concurrent connect/accept/close at userspace-start would hang on a deadlock, and 2/3
  boots reach DHCP -- the 1 stall is the flaky F55 one, re-boot-confirmed).
- execve/spawn raw-deref user argv/envp arrays + element strings -> kernel panic + kernel-mem leak ->
  FIXED (F71): sys_exec/sys_spawn read uargv[i] and s[l]/memcpy(ks,s,l) on fully attacker-controlled
  pointers with no _access_ok/copy_from_user. A KADDR argv (or argv[i]) copies kernel memory into the
  child's argv (unprivileged info-leak/LPE); an unmapped-in-range argv panics the kernel (#PF no-VMA,
  F67 class, DoS). spawn also raw-deref'd stdio_ptr and _access_ok+memcpy'd spawn_attr. Fix (DRY, one
  shared primitive): copy_user_strv reads each array slot via copy_from_user and each string via
  copy_path_from_user (bad ptr -> -EFAULT, never a raw deref), kstack-off scratch (F68 lesson); both
  syscalls route argv+envp through it, stdio+attr use copy_from_user. Deterministic selftest
  (copy_user_strv_selftest: kernel/non-canonical/past-ceiling vector ptr -> -EFAULT; NULL -> empty) +
  code-proof + clean boot (56 selftests; every boot exec/spawn copies a real argv through it).
- RECORDED (read-only fan-out, HIGH confidence, NOT yet self-verified -- verify before fixing): (n)
  sys_brk heap-shrink frees frames before the cross-CPU TLB shootdown (vs munmap's flush-then-free) ->
  sibling-thread stale-TLB UAF write; (o) pty pair lifetime (master_open/slave_open_count/s_pty_head)
  unlocked -> concurrent master+slave close double-frees the pty_t; (p) AF_INET TCP child PCB's raw
  ->listener backpointer not cleared on listener free -> a late handshake ACK writes through the freed
  listener (remote-timed heap UAF); (q) sys_poll/sys_select register a waiter on f->waitq without
  pinning f (epoll pins, F60) -> a sibling close frees the embedded waitq -> wq_remove UAF. Same
  lifetime/ordering classes as F57-F70, in un-swept subsystems.
- sys_brk heap-shrink frees frames before the cross-CPU TLB shootdown -> stale-TLB UAF write -> FIXED
  (F72, follow-up (n)): the shrink loop did vmm_page_unmap (local invlpg only) + pmm_ref_dec (immediate
  buddy free) per page, with tlb_flush_range only AFTER the loop. A sibling THREAD_SHARE_MM thread on
  another CPU keeps a stale writable TLB entry and writes into the freed+reused frame (cross-owner
  corruption / LPE) -- the exact hazard mm_unmap_range_flush_free was built to forbid and which munmap/
  MAP_FIXED already use, but brk open-coded free-then-flush. Fix (DRY): route brk shrink through
  mm_unmap_range_flush_free (unmap -> cross-CPU flush -> free), and shrink the VMA FIRST so a sibling
  fault in the torn-down range SIGSEGVs instead of repopulating a frame about to be freed. Code-proof
  (documented flush-before-free primitive) + clean boot (brk exercised by every heap alloc; 56
  selftests, DHCP, login renders). Closes one of the four fan-out findings.
- sys_poll/sys_select register a waiter on a polled file's embedded waitq without pinning the file ->
  sibling-close UAF -> FIXED (F73, fan-out finding (q)): both read fd_table[fd] raw and
  wait_group_add(f->waitq) across sched_sleep + wait_group_cleanup; a THREAD_SHARE_FILES sibling
  close() kfree's the eventfd/timerfd/pipe state (incl. the embedded waitq) synchronously, so the
  woken poller's wq_remove writes/walks freed memory. epoll pins w->file (F60); poll/select did not.
  Fix: fdget-pin every referenced fd into a per-call array at the top of each iteration, use the pinned
  file for the scan + registration + recheck, fdput after cleanup (mirrors epoll). Code-proof (pin
  spans register->sleep->cleanup) + clean boot (2/2 DHCP; wayland polls constantly; copy_user_selftest
  still passes). Three of four fan-out findings now fixed (n, q); remaining: (o) pty double-free,
  (p) TCP child->listener UAF.
- PTY pair lifetime unlocked -> concurrent master+slave close double-frees the pty_t -> FIXED (F74,
  fan-out finding (o)): pty_master_close/pty_slave_close each did flip-flag/count + test-peer + maybe
  kfree(pty) with no lock, so two CPUs closing the two ends (CLONE_FILES) both free; the unlocked
  s_pty_head insert/unlink/walk could also deref a node being unlinked. Fix (one global s_pty_lock,
  mirrors F70/F25): both close ops flip + wake + decide + unlink under the lock, free after; exactly
  one closer frees (the second to lock), and the wakes under the lock complete before the freer frees
  (closes the wake-vs-free race the stale RCU comment flagged). pty_free_locked split into
  unlink-locked + free-struct; pty_alloc links the pair LAST (built first); open-by-index and a new
  locked pty_find_ctty_slave (replacing tty_get_ctty's open-coded walk) are locked. Deterministic
  selftest (pty_lifetime_selftest: claim, close master -> not freed, close slave -> freed+unlinked) +
  code-proof (lock makes the free atomic) + clean boot (57 selftests; PTYs not boot-exercised, console
  ctty path intact). All four fan-out findings (n, o, q, ...) now fixed except (p) TCP child->listener.
  Recorded follow-up (r): the pty master ring (m_head/m_tail) is mutated unlocked producer-vs-consumer
  -> data race / lost wakeup (masked indices keep it in-bounds, not OOB), lower severity.
- TCP child PCB raw ->listener backpointer not cleared on listener free -> late-ACK heap UAF -> FIXED
  (F75, fan-out finding (p)): a SYN_RCVD child stores child->listener = listener (raw); tcp_pcb_free of
  the listener unlinks + RCU-frees it but never touches the children, so a later handshake ACK in
  tcp_recv's SYN_RCVD case (lst = child->listener; tcp_pcb_lock(lst); accept_q_push(lst); pcb_wake(lst))
  writes through the freed listener -- RCU does not help (the backpointer is read in a later, separate
  reader section, after the listener's grace period). Same class as F65. Fix (universal free chokepoint,
  mirrors F64): in tcp_pcb_free, under the held s_pcb_wlock and before unlinking, NULL the ->listener of
  every pcb pointing at the one being freed; the SYN_RCVD ACK path's `if (lst)` then skips the dead
  listener. No-op for non-listeners; a concurrent reader is safe via the RCU-deferred free. Deterministic
  selftest (tcp_listener_orphan_selftest: listener free clears child->listener, unrelated free does not) +
  code-proof + clean boot (58 selftests; TCP listeners not boot-exercised). ALL FOUR fan-out findings
  (n, o, p, q) now fixed. Minor: the walk makes tcp_pcb_free O(live PCBs) -- fine at MakaOS scale.
- io_uring dispatch double-fetches sqe->addr/len from the user-writable SQE ring -> TOCTOU arbitrary
  kernel R/W -> FIXED (F76, 2nd fan-out): uring->sqes[] is mapped VMA_R|W|USER; the sync and SQPOLL
  dispatch paths pass dispatch_exec a LIVE pointer; for OP_READ/WRITE it validates sqe->addr/len with
  user_buf_check then RE-READS them for vfs_read/write, so a concurrent user write (trivial via SQPOLL)
  redirects the buffer to a kernel address between check and use = arbitrary kernel read/write (LPE).
  The async io_wq path was safe because it snapshots (w->sqe). Fix: snapshot the SQE once at the top of
  dispatch_exec (io_sqe_t snap = *sqe; sqe = &snap) so every read is from the immutable copy -- the
  validate and the use are the same value. Code-proof + io_uring_selftest (NOP dispatch through
  io_uring_enter_impl) still passes + clean boot. Found by a fresh fan-out (futex/scheduler/fork-CoW
  came back SAFE). RECORDED follow-ups from the same fan-out (verify-before-fix): (s) HIGH -- socket
  syscalls (accept/connect/bind/listen/sendmsg/recvmsg + SCM_RIGHTS) use the un-refcounted fd_to_file
  across a BLOCKING op -> a THREAD_SHARE_FILES sibling close() RCU-frees the socket under the blocked
  caller = UAF (same class as F73; fix = fdget/fdput per site, like sys_read). (t) LOW -- sys_thread's
  private-fd-table clone path copies fd_table[] but not fd_flags[], dropping FD_CLOEXEC (correctness).
- socket syscalls deref the un-refcounted fd_to_file across a (blocking) op -> sibling-close UAF -> FIXED
  (F77, fan-out finding (s), partial): bare fd_to_file(fd) holds no ref and no rcu pin; accept/connect/
  recvfrom additionally BLOCK in the socket op, derefing the file across sched_sleep, so a THREAD_SHARE_FILES
  sibling close(fd) RCU-frees it under the caller = UAF (F73 class). Fix (F73 pattern, one mechanism): fdget
  pin + fdput on every return path via single-exit goto out, at sys_bind/listen/accept/connect/sendto/
  recvfrom/setsockopt/getpeerpid/shutdown. DHCP exercises sendto/recvfrom (UDP) so the lease proves those
  balance; socketpair/scm_rights/unix_refcount selftests cover the pin primitive; rest code-proof. 58 PASSED,
  clean boot. REMAINING (s2, recorded): w_sys_sendmsg/recvmsg + the SCM_RIGHTS tf loop (~13 returns each +
  unix_sock_sendfd ownership semantics + a bounce[4096] kstack hazard) and epoll/timerfd/signalfd fd_to_file
  -- a dedicated effort, not boot-validatable pre-login.
- sendmsg/recvmsg + the SCM_RIGHTS fd loop deref the un-refcounted fd_to_file across a blocking op -> sibling-
  close UAF -> FIXED (F78, completes finding (s)): same class as F77 on the two msghdr syscalls. Verified the
  SCM entanglement first: unix_sock_sendfd does vfs_dup(file) (vfs.h: bumps refcount only if already > 0, same
  pointer) into the peer ancillary, and vfs_dup's contract requires the caller to hold a ref (else the dup
  races a last-close) -- so the bare tf = fd_to_file(fds_buf[i]) fed to vfs_dup is itself a UAF. Fix: fdget/
  fdput the socket on every return path (single-exit goto out) in both functions, AND fdget/fdput each SCM fd
  around the non-blocking sendfd (the pin makes vfs_dup's precondition true; the ancillary's dup ref is
  independent). Code-proof (not boot-exercised pre-login) + scm_rights/unix_refcount selftests + clean boot.
  Next: the bounce[4096] and fds_buf/inst_fds[253] (~1KB) buffers in these two functions are still on the
  8KiB kstack (F68 class).
- sendmsg/recvmsg put ~5KiB of buffers on the 8KiB kstack before a blocking call -> kstack-overflow hazard
  -> FIXED (F79, F68 class): bounce[4096] + fds_buf[253]/inst_fds[253] (1012 bytes each, SCM_MAX_FD=253) all
  moved to kmalloc, freed at the single goto-out chokepoint. Named MSG_BOUNCE_SZ; every sizeof(bounce) (would
  silently become 8 on a pointer) replaced. bounce alloc'd once per call on first iov chunk; fds_buf once on
  first SCM cmsg; inst_fds LAZILY on the first arriving fd so the common no-fd recvmsg pays zero alloc. All
  pointers declared NULL before the first goto out (so out's kfree never reads garbage); kmalloc-fail paths
  leave the pointer NULL and -- for inst_fds -- vfs_close the dequeued payload. Per-CPU bounce rejected (held
  across the blocking sleep). Code-proof + no-jump-crosses-init compile + clean boot (58 PASSED).
- epoll_wait/epoll_ctl deref the un-refcounted fd_to_file(epfd) across an op -> sibling-close UAF -> FIXED
  (F80, recorded fd_to_file follow-up): epoll_close kfrees state/self with NO RCU; epoll_wait sleeps on
  state->wq (the sharp sleep-UAF: post-wake task_we_remove + the next spin_lock deref freed state), epoll_ctl
  holds state across spin_lock+kmalloc+register (narrower SMP-concurrent free). Fix: fdget(epfd)/fdput on
  every return path (single-exit goto out, all locals before the first goto so no jump-crosses-init, inline
  fdput for the not-an-epoll case). Watched-fd polling already safe (pinned w->file, F60). Code-proof + clean
  compile + clean boot (58 PASSED incl. epoll_pin). RECORDS (u): epoll_ctl ADD/MOD read the WATCHED fd raw
  (fd_table[tfd]) then vfs_tryget it in register -- a sibling close(tfd) racing the gap can free+reuse wf
  before the tryget; mitigated (tryget) and narrower, but real; fix = fdget(tfd) across register, fdput after.
- timerfd_settime/gettime + signalfd mask-update deref the un-refcounted fd_to_file across the op -> sibling-
  close UAF -> FIXED (F81, closes the fd_to_file class): timerfd_close_op/signalfd_close_op kfree the state +
  file IMMEDIATELY (no RCU), so a sibling close(fd) frees the backing state in the window between the bare read
  and the deref (narrower SMP variant -- these ops do not block; the blocking read goes via sys_read, fdget-safe
  since F73). Fix: fdget/fdput on every path (timerfd via single-exit goto out with locals before the goto;
  signalfd straight-line). Code-proof + timerfd/signalfd selftests pass + clean boot (re-boot reached DHCP after
  a flaky F55 stall). CAPSTONE: fd_to_file then had ZERO callers -- the unsafe file-static compat primitive was
  REMOVED (decl + def), eliminating the footgun (safe-primitives). The whole fd_to_file-across-an-op UAF sweep
  is closed: F73 poll/select, F77 socket syscalls, F78 sendmsg/recvmsg, F80 epoll, F81 timerfd/signalfd.
- epoll_ctl ADD/MOD read the WATCHED fd raw (fd_table[tfd]) then vfs_tryget in register -> free+reuse UAF ->
  FIXED (F82, follow-up (u), found during F80): the raw read + register's tryget is NOT under rcu_read_lock,
  so a sibling close(tfd) on another CPU can free + reuse the slab slot (ADD: across ep_grow/kmalloc) before
  the tryget bumps it. Mitigated (tryget) and narrower than the epfd bug, but real. Fix: fdget(tfd) (read +
  tryget under rcu, cannot be freed/reused) instead of the raw read, fdput after register on every path (ADD:
  all 3 exits; MOD: inside if(wf)). The pin makes register's tryget reliably succeed (no inert watch); ADD on
  a concurrently-dying fd now returns -EBADF instead of inserting an inert watch (more correct, race-only).
  Code-proof + epoll_pin selftest passes + clean boot (DHCP, 58 PASSED). Completes epoll fd safety (F80 epfd
  + F82 watched fd).
- sys_thread private-fd-table clone drops FD_CLOEXEC -> CLOEXEC fd survives exec -> FIXED (F83, follow-up (t),
  LOW correctness): the non-shared branch (syscall.c:1491) copied fd_table[i] via vfs_dup but not fd_flags[i],
  unlike task_fork (process.c:558-561, copies both) and fd_table_grow (copies both). FD_CLOEXEC lives in the
  parallel fd_flags[] (process.h:106) and exec closes fds with it set (syscall.c:1034-1043), so a spawn'd task
  (private fd table) that holds a CLOEXEC fd then execs leaks it. Fix: add dst->fd_flags[i] = src->fd_flags[i]
  to the clone loop, mirroring task_fork exactly. Code-proof + clean boot (spawn path is boot-exercised; 58
  PASSED).
- PTY master ring (m_head/m_tail) mutated unlocked by producer vs consumer on different CPUs -> data race /
  lost wakeup -> FIXED (F84, follow-up (r)): pty_slave_write_char/write_buf (producer, slave write() path, no
  lock -- tty.c forbids tty->lock there) and pty_master_read (consumer, no lock) raced the plain uint32_t
  head/tail; masked indices keep it in-bounds (correctness, not OOB). The slave INPUT ring is correctly
  serialised by tty->lock (tty_input_char + tty_ldisc_drain); the master OUTPUT ring just lacked the symmetric
  lock. Multi-producer/consumer-capable, so a lock (not SPSC atomics) is needed. Fix: a per-pty master_lock
  around the head/tail mutations only (write_char push, write_buf Phase-1, extracted pty_master_drain mirroring
  tty_ldisc_drain), released before every wake and the blocking WAIT_EVENT (no nest, no sleep-under-lock). Drain
  copies under the lock safely (sys_read prefaults the user buffer). Added pty_master_ring_selftest (round-trips
  "hithere" through the locked push/drain). Clean boot 59 PASSED incl. pty_mring + pty_life.
- AHCI LBA-vs-capacity bound (follow-up (d)) -> ASSESSED SAFE, NOT a memory-safety bug, no fix (2026-06-26):
  the AHCI read/write paths do not reject an LBA+count past the device's sector capacity. Verified skeptically:
  (1) the MEMORY side of every submit path is bounded by count, INDEPENDENT of lba -- ahci_submit_hhdm/sg/
  read_user all call xfer_bytes_ok(count,512,..) (the F47 wrap+PRDT guard) and cap the PRDT (130/248 entries),
  so an out-of-range lba cannot enlarge or misdirect the memory transfer (lba selects disk sectors; the PRDT
  built from buf/count fixes the memory region); (2) the lba is NOT attacker-controlled -- there is NO raw
  user block-device path (no /dev/sd, blkdev, or AHCI ioctl; grep-confirmed), the only callers are the trusted
  ext2 fs (ext2_blk_to_lba = part_lba + blk*spb, F46-wrap-guarded) and the boot stress harness; (3) AHCI does
  not even store a device capacity from IDENTIFY, so adding lba+count<=capacity is a new FEATURE, not a bound
  that was dropped; and the ATA drive itself aborts an out-of-range LBA (command abort, no transfer), so the
  worst case is a failed read/write the fs handles -- never an OOB memory access. A partition-end bound (reject
  lba outside [part_lba, part_lba+part_size)) would be defense-in-depth against a corrupt/malicious fs image,
  but that is a trusted-fs/feature concern, not a reachable memory-safety bug. Recorded SAFE; not fixed.
- raw-deref-vs-munmap TOCTOU (follow-up (l)) -> RECORDED, design noted, DEFERRED (big mechanism), no fix this
  pass (2026-06-26): a syscall that user_buf_check/prefaults a user buffer then dereferences it RAW (not via
  copy_*_user) has a TOCTOU window -- a THREAD_SHARE_MM sibling munmap()/mprotect() between the check and the
  raw deref unmaps the page, so the kernel deref takes a #PF in kernel mode. The robust fix is a kernel
  exception-fixup table (extable): tag the raw user-deref instructions so the page-fault handler, on a fault
  at a tagged RIP, jumps to a fixup that returns -EFAULT instead of panicking (the Linux __ex_table model).
  copy_from_user/copy_to_user already have this fault handling (copyuser_test passes), and the DMA paths
  already PIN the user frames (ahci_read_user -> vmm_get_user_pages pins, closing the recycle window), so the
  exposure is the remaining non-pinned prefault-then-raw-deref sites. No clean per-site sub-fix exists short of
  routing each through copy_*_user (which the performance-sensitive sites avoid by design) or building the
  extable; the extable is a cross-cutting mechanism too large for one audit step. Deferred as a dedicated
  effort; design recorded here so it is known debt, not a buried landmine.
- 2026-06-26 FAN-OUT (read-only audit over un-swept subsystems; reported by Explore agents, HIGH confidence
  each, NOT yet independently verified -- VERIFY against the real code before fixing). Two subsystems came back
  SAFE: virtio-gpu/virtio-input virtqueues (every device-writable used-ring id/idx/len and every userspace
  ioctl index is masked to the ring size or bounds-checked; sizes via mul_within_u32/drm_dumb_size; rings
  serialised by per-queue IRQ-safe locks or a single kthread) and the ELF loader / initial user stack
  (argc/envc/strlen capped at the syscall entry, pages > VMM_USER_STACK_PAGES rejected, rsp < ubase backstop,
  N_AUXV matches the writes, every PT_LOAD range bounded; the eager-path file_off+nbytes sum is dead code,
  backing_file always non-NULL). Three CANDIDATE bugs:
  (v) HIGH, directly unprivileged-reachable -- evdev evdev_client_t USE-AFTER-FREE: input_device_emit
  (kernel/drivers/input/evdev.c ~164-182) walks d->clients and derefs each client (ring_push + sched_wake +
  wait_queue_wake_all) with NO lock, while evdev_vfs_close (~261-281) unlinks + kfree's the client; a keyboard/
  mouse producer on one CPU can write into / wake a client a sibling close() just freed (producer reaches c via
  d->clients, holds no fd ref, so the fd refcount does not protect it). Same class as F84/F70/F65; subsumes a
  torn-index race on c->head/c->tail (plain uint32_t, no lock). Reachable: open /dev/input/event0 (0660),
  read() in one thread, close() in a sibling, input flowing. Fix = one per-device lock over the client-list
  lifetime AND the per-client ring head/tail (mirror F84), free RCU-deferred or wakes outside the lock.
  -> VERIFIED + FIXED (F85): independently confirmed against the real code, incl. that the MOUSE producer runs
  in IRQ context (i8042 ISR -> mouse_isr_packet -> input_device_emit), so the lock is IRQ-safe. Added
  input_device_t.lock (spin_lock_irqsave) held across the emit fan-out (walk + ring_push + wakes; wakes under
  the lock are safe -- acyclic order, non-sleeping), the close unlink (free after unlink), the read
  empty-check + ring_pop (dropped around sched_sleep; wake_pending covers the gap), EVIOCGRAB, and the open
  insert; poll/FIONREAD keep lockless advisory reads. Deterministic evdev_ring_selftest (locked emit/drain
  round-trip + client unlink). Clean boot 60 PASSED incl. evdev_ring, no hang on the boot-exercised keyboard.
  (x) HIGH, needs a crafted/corrupt ext2 image -- ext2 block/inode bitmap OOB: s_blocks_per_grp /
  s_inodes_per_grp from the untrusted superblock are validated only != 0 (ext2.c ~877), never clamped to the
  one-block bitmap capacity s_block_size*8; the bitmap scratch is a fixed 4096 bytes, so bit = rel %
  s_blocks_per_grp (free_block ~1783) or bitmap_find_free(buf, s_blocks_per_grp) (alloc_block ~1756) with the
  field > 32768 indexes buf[bit>>3] past 4096 = heap OOB write/read; reached via truncate/write ->
  free_inode_blocks -> free_block. Same for s_inodes_per_grp (alloc_inode ~1716 / free_inode_num ~1807). The 3
  named angles (indirect traversal, symlink, truncate double-free) are SAFE (disk block numbers go through
  ext2_block_valid/ext2_run_valid; symlinks unimplemented -- readlink EINVAL / symlink EPERM; free_block(0) is
  a no-op; F56 covers the overwrite race). Fix = clamp both fields <= s_block_size*8 at mount (~877), mirroring
  ext2_block_size_checked / ext2_inode_size_valid; add a pure ext2_group_geom_valid helper + selftest.
  -> VERIFIED + FIXED (F86): all premises confirmed (fixed 4096-byte scratch, bitmap helpers unbounded, mount
  guarded only != 0, bit = rel % blocks_per_grp unclamped, group-check is not a bit-check). Added pure
  ext2_group_geom_valid (both fields in [1, block_size*8]) gating the mount at ext2.c:877; pure
  ext2_group_geom_selftest (legit maxima 8192@1KiB / 32768@4KiB pass, +1 over cap rejected, zero rejected).
  Clean boot 61 PASSED incl. ext2_geom, and the real root fs mounting through the gate proves the bound is not
  too strict. Trust model: crafted/corrupt ext2 image only.
  (w) MED-LOW, malicious-controller only, NOT reachable on QEMU (DSTRD=0 fits all 64 QIDs in 0x2000) -- NVMe
  doorbell BAR0 OOB: nvme.c:677 maps BAR0 at a fixed 0x2000, but the doorbell offset 0x1000+(2*qid+1)*stride
  scales with device CAP.DSTRD (stride = 4<<dstrd, never validated, ~687) and qid up to nr_queues (capped
  MAX_CPUS=64), so DSTRD>=3 drives an MMIO store past the mapping. The completion data-path is SAFE (cid
  bounded by nvme_cid_valid on the only indexing consume; phase-gated; cid-reuse UAF-free, req[] static; F48
  covers the PRP wrap). Fix = size/cap the BAR0 mapping from the real doorbell extent after CAP + nr_queues are
  known. PRIORITY next: (v) evdev UAF first (sharpest, no crafted image), then (x) ext2 bitmap, then (w) nvme.
  -> VERIFIED + FIXED (F87): all premises confirmed (fixed 0x2000 map, stride = 4<<dstrd unvalidated, doorbell
  = s_regs+0x1000+(2*qid+1)*stride, qid up to MAX_CPUS). After reading CAP, the driver now computes
  db_extent = round_up(0x1000 + (2*MAX_CPUS+1)*stride + 4, page), refuses the controller above a 1 MiB ceiling
  (absurd DSTRD), and remaps BAR0 over db_extent before the qid-0 doorbells are touched; the DSTRD=0 case rounds
  to 0x2000 and is left unchanged (no remap). Code-proof (extent covers the highest doorbell + the write; QEMU
  path unchanged) -- NOT boot-exercised (QEMU attaches no NVMe device, nvme_init bails at pci_find), so the
  change is provably inert at boot. Clean boot (3rd try past the flaky F55 stall): DHCP, 61 PASSED, no faults.
  All three fan-out candidates (v/x/w) resolved.
- 2026-06-26 FAN-OUT #3 (5 read-only Explore agents over un-swept subsystems; reported HIGH confidence each,
  NOT yet independently verified -- VERIFY against the real code before fixing). Two SAFE: futex + the
  cleartid-join path (MakaOS has NO robust-list -- only a single-word cleartid join accessed via fault-safe
  copy_to_user; no unbounded user-list walk to cap; F59 waiter lifetime intact; no FUTEX_REQUEUE) and shmem /
  POSIX shm (two-level refcount = namespace + fd + per-VMA all balanced; the #PF page index is bounded < npages
  under shm->lock; teardown RCU-deferred behind shmem_tryget; beyond F61; minor non-safety notes: a left-edge
  VMA trim does not adjust shmem_pgoff = aliasing-not-OOB, and a mmap-vs-concurrent-remove ref LEAK -- neither
  memory-unsafe). Three CANDIDATE bugs (all HIGH, all unprivileged-reachable concurrency UAFs):
  (y) ksec unlocked single-slot rendezvous (kernel/security/ksec.c ~25-30 s_pending, used in ksec_request
  ~67-98 and ksec_reader_thread ~120-146): s_pending is ONE global slot (the comment admits "Single-CPU... 
  Multi-CPU extension: replace with a hash-table keyed on seq") but the scheduler is SMP; two processes racing
  setuid/seteuid/setuid-exec overwrite s_pending.waiter (a raw task_t* with no lock, no refcount) and .seq, so
  (a) the reader's sched_wake(s_pending.waiter) can fire on a task that already returned + exited + was freed =
  UAF write through a dangling task_t*, and (b) a verdict (ALLOW) issued for one caller is delivered to a
  different caller = verdict cross-talk -> PRIVILEGE ESCALATION in the privilege-gating daemon. The userland
  policy parser is bounded (path/value copies capped at 127, POLICY_MAX checked) = SAFE. Fix = a spinlock over
  every s_pending access with the waiter cleared-under-lock-before-wake (closes the UAF), and a per-seq
  hash/array so concurrent callers do not share one slot (closes the cross-talk + the starvation hang) -- the
  exact "hash-table keyed on seq" the code comment prescribes; the task ref must be lifetime-safe.
  -> VERIFIED + FIXED (F90): confirmed the single unlocked slot + the non-interruptible loop. Also found two
  more SMP bugs: ksec_next_seq did a non-atomic ++ (seq collision) and wrapped to 0 (the free marker). Fix:
  per-seq s_slots[64] under s_ksec_lock; ksec_request claims its own slot (fail-closed -> -EPERM if full),
  drops the lock around sched_sleep, is signal-interruptible (frees slot + returns -EINTR on kill -> no
  dangling-waiter UAF + no hang), and frees under the lock after waking; reader matches by seq + wakes under
  the lock (so the requester cannot free/return before the wake -> no UAF); ksec_next_seq atomic, skips 0.
  Deterministic ksec_slot_selftest + code-proof. Provably inert at boot (no daemon -> ksec_request -EAGAIN
  early). 3rd re-boot reached DHCP; 62 PASSED incl. ksec_slot + ksec_setuid. All fan-out #3 candidates resolved.
  (z) /proc files_shared UAF (kernel/fs/proc.c: proc_fd_open ~287-297, proc_fd_readdir ~300-318, proc_stat fd
  branch ~591-603): proc_open rcu_read_lock + sched_find_pid pins the TASK struct (RCU-freed in task_free_rcu),
  but the proc fd handlers deref t->files_shared (the target's task_files_t + fdtable_t), which sys_exit frees
  SYNCHRONOUSLY via task_files_release -> kfree (NO call_rcu, unlike mm_shared which is freed in task_free_rcu),
  and NULLs files_shared with a plain store. So a reader in /proc/<B>/fd/* while B exits derefs a kfree'd
  fdtable_t (then vfs_tryget on a freed slot pointer) = cross-process UAF read, reachable by any same-uid
  process (parent reading /proc/<child>/fd/, virtfs grants mode 0400 owner=target uid). Distinct from the fdget
  pattern (which only touches g_current's OWN table). Fix = RCU-defer the task_files_t free (mirror mm_shared:
  release/free in task_free_rcu, or call_rcu the final-drop free), so the existing rcu_read_lock in proc_open
  genuinely pins it; also make the files_shared store/loads atomic.
  -> VERIFIED + FIXED (F88, option b): confirmed task_files_release vfs_close's the fds synchronously (the
  load-bearing exit_files EOF semantics, syscall.c:678) AND kfree's the struct synchronously. Fix keeps the
  synchronous close but call_rcu_expedited's the STRUCT free (task_files_free_rcu); sys_exit unpublishes
  files_shared with a release store BEFORE task_files_release (correct unpublish->call_rcu RCU order); the 3
  proc readers snapshot files_shared once with an acquire load (also closing a 2nd-order double-read NULL-deref
  race). The fd_table[n] file pointers are covered by the existing tryget-under-rcu (like fdget). Code-proof +
  clean boot (exit path + /proc heavily boot-exercised; task_exit_state/tgleader selftests pass; 61 PASSED).
  (aa) io_uring SQPOLL-spawns-worker-after-close UAF (kernel/io/io_uring.c: io_uring_close_file ~121-134 vs
  io_sqp_kthread_entry ~587 -> io_wq_ensure_worker ~771): close stops/joins the io_wq worker BEFORE SQPOLL,
  gating on a plain read of uring->worker; if no async op has run yet, worker==NULL so close skips the worker
  stop+join, but the still-running SQPOLL kthread then hits an IOSQE_ASYNC SQE, calls io_wq_ensure_worker which
  sets uring->worker=t (plain store) + sched_add -- a live worker; close then joins only SQPOLL and frees
  uring/backing (pmm_buddy_free + kfree), and the orphaned worker UAFs the freed uring/backing (posts CQEs into
  reused pages). Unprivileged: SQPOLL setup + IOSQE_ASYNC SQE + a sibling close(ring_fd). Distinct from F63
  (fixed-files, the ONLY fixed-index consumer, bounded) and F76 (SQE snapshot); CQ overflow + re-register are
  SAFE. Fix = stop + JOIN SQPOLL FIRST, then read/stop/join the worker (only after sqp_done can no new worker
  be spawned); harden the worker publish to a release store + acquire load. PRIORITY: (z) /proc first (most
  reachable, clearest root cause), then (aa) io_uring (cleanest fix, a reorder), then (y) ksec (biggest fix).
  -> VERIFIED + FIXED (F89): confirmed the join order + the plain publish, AND that the enter spawn-site is
  ref-serialised (sys_io_uring_enter holds an fdget ref so close cannot run during it) -- so SQPOLL is the only
  spawn site that races close. Fix: io_uring_close_file now stops+JOINS SQPOLL FIRST, then reads/stops/joins
  the worker; the worker publish is a release store and every uring->worker read (close join + ensure_worker
  fast-path) an acquire load. io_uring_test still PASSES + clean boot (DHCP, 61 PASSED).
- 2026-06-26 FAN-OUT #4 (5 read-only Explore agents over un-swept subsystems; reported HIGH confidence each,
  NOT yet independently verified -- VERIFY against the real code before fixing). One SAFE: AHCI NCQ completion
  (the device-written completed-slot set is done = active_mask & ~sact/~ci, so the device can only CLEAR bits,
  never inject an out-of-range slot index; slots bounded to MAX_NCQ_SLOTS=32; the completer atomically claims
  done out of active_mask before any state write; result published release / read acquire; slot reuse + waiter
  wake safe; beyond F47/F62). Four CANDIDATE bugs:
  (bb) HIGH, unprivileged SMP -- dcache_install resurrect-a-DYING-dentry UAF: dcache_install's `if (existing)`
  branch (kernel/fs/dcache.c ~398-406) drops g_dcache_wlock at ~399 then bumps existing->refcount with a BARE
  __atomic_fetch_add at ~405, OUTSIDE the lock. bucket_find_locked matches by key, so a refcount-0 dentry still
  on the LRU is a normal find; meanwhile the shrinker kthread (dcache_shrink) can CAS that dentry refcount
  0 -> DCACHE_REF_DYING, unlink it, and call_rcu its free. The bare fetch_add then turns DYING (0xFFFFFFFF) ->
  0, returns the dentry to ext2_lookup_path as live, and (no rcu_read_lock held across this) the grace period
  frees the slot under the returned pointer = UAF + refcount wrap + (via SLAB_TYPESAFE_BY_RCU reuse) a stale
  child_ino -> wrong inode resolved. The lookup path (audit fix T1, dcache.c ~244-254) already closed this with
  a DYING-aware CAS under the lock; the install-hits-existing path was left bare. Reachable by any open/stat/
  exec on SMP + the concurrent shrinker. Fix = acquire existing's ref with the SAME DYING-refusing CAS, UNDER
  g_dcache_wlock (the shrinker also holds it), and on DYING fall through to install the fresh candidate instead.
  -> VERIFIED + FIXED (F91): confirmed the bare fetch_add at ~405 runs AFTER the spin_unlock at ~399, and that
  the lookup-path T1 CAS (dcache.c ~244-254) is the DYING-aware acquire. KEY REFINEMENT of the prescription
  (no-assumptions): the shrinker (dcache_shrink ~475-498) CAS-claims 0 -> DYING (~488) AND
  dentry_unlink_and_free_locked (~493) BOTH under ONE g_dcache_wlock hold -- so a DYING dentry is unlinked from
  the hash bucket before the lock is released, meaning a still-linked entry returned by bucket_find_locked UNDER
  the lock is NEVER DYING. So the correct fix is simply to take the reference UNDER g_dcache_wlock (before the
  unlock): the lock -- which the shrinker also holds for its claim+unlink -- is the serialization, making the
  bump and the shrinker's claim mutually exclusive. A DYING-aware CAS here would be redundant (its DYING branch
  is unreachable under the lock) and would misleadingly imply the lock-free concurrency that the lock prevents;
  the CAS is the lock-FREE dcache_lookup's mechanism (lookup holds no lock, so it CAN race the shrinker). Fix:
  moved the `__atomic_fetch_add(&existing->refcount, 1u, __ATOMIC_ACQ_REL)` to BEFORE the spin_unlock_irqrestore;
  candidate-d disposal (kfree name_ext, parent ref dec, pmm_slab_free) stays after the unlock (d was never
  visible). dcache_test + dcache_race (2 lookers + shrinker, 50000 iters, 0 child_ino corruptions) STILL PASS +
  clean boot (DHCP, all selftests PASSED, login renders).
  (dd) HIGH, unprivileged -- timerfd concurrent-settime per-CPU-list corruption UAF: timerfd_settime
  (kernel/io/timerfd.c ~256-310) detaches the node from its old home-CPU list under old_pc->lock, drops the
  lock (no preempt_disable across the gap), then attaches to the current CPU's list under pc->lock, writing
  t->home_cpu -- with NO node-level mutual exclusion, and tfd_list_insert (~87) has NO already-linked guard.
  Two threads sharing the fd table (THREAD_SHARE_FILES) calling settime on the same timerfd on different CPUs
  both see home_cpu==~0, each insert the node into a DIFFERENT per-CPU list -> one node on two lists / t->next
  cross-linked; then timerfd_close_op removes from home_cpu's list and kfrees while the node is still in the
  other CPU's list -> that CPU's timerfd_tick derefs + fires a freed node = UAF. Distinct from F81 (which
  pinned the file but did NOT serialise two settime callers). eventfd (CAS counter), signalfd (s_sfd_lock), and
  timerfd close-vs-tick (pc->lock + preempt_disable) are SAFE. Fix = a per-node spinlock in timerfd_state_t
  held across the WHOLE settime detach+attach (and close remove), so the two per-CPU list ops on one node are
  atomic; the tick still takes only pc->lock.
  -> VERIFIED + FIXED (F92): confirmed the detach (under old_pc->lock ~277-290) and the attach (under the
  calling CPU's pc->lock ~303-308) are TWO separate critical sections with no node-level mutual exclusion, and
  that tfd_list_insert has no already-linked guard. The per-CPU lock cannot serialize two settime callers
  because home_cpu -- which selects WHICH pc->lock "protects" the node -- is what they are racing to change.
  REFINED the recorded close angle (no-assumptions): close (timerfd_close_op) does NOT need the node lock --
  w_sys_timerfd_settime/gettime fdget-PIN the file (F81), so close runs only at the last fd-ref drop, which
  cannot overlap any in-flight settime; close vs the tick is already serialized by pc->lock. The unfixed race
  is settime-vs-settime on a SHARED vfs_file_t (fork copies the fd table but both fds point at the same file;
  threads/SCM_RIGHTS likewise), each caller holding its OWN fdget ref, which does NOT serialize them against
  each other. FIX (do-it-right, minimal correct, one shared mechanism): added a STABLE per-node spinlock
  timerfd_state_t.lock held (irqsave) across the WHOLE settime detach+attach, so the home_cpu transition is
  atomic vs other settimes. Lock order t->lock (outer) -> pc->lock (inner); only one pc->lock held at a time
  (old home unlocked before new home locked); tick/close take only pc->lock, so no cycle. Documented in
  docs/LOCKS.md. VERIFICATION: a cross-CPU UAF, not deterministically reproducible -> a NEW concurrent stress
  selftest (timerfd_race_selftest, mirrors dcache_race): 3 storm kthreads + main hammer settime on 16 SHARED
  timerfds (armed far-future so they never fire and ticks early-return) for 20000 iters, churning home_cpu;
  then walks every per-CPU list under its lock and asserts each timer's node is on EXACTLY ONE list with a
  consistent home_cpu/in_list (a double-link shows as a node counted on two lists or a list cycle) -- PASSED
  ("no node on two per-CPU lists"). The change is provably not boot-DHCP-exercised: no userland net/dhcp app
  uses timerfd and the kernel TCP/IP retransmit path does not either, so timerfd is inert to the net path.
  Clean boot: 2 boots hit the flaky F55 userspace-start stall (net ready, login + /bin/net paged, no lease, no
  fault); the 3rd same-build re-boot reached the DHCP lease (ifconfig IP 10.0.2.15 GW 10.0.2.2), all selftests
  PASSED incl. [timerfd-selftest] and [timerfd_race], 0 FAILED/PANIC, no [WD], `More login:` renders. This
  closes fan-out #4 candidate (dd).
  (ee) MEDIUM, F84/F85 class -- pipe ring data race: pipe head/tail/count (kernel/fs/pipe.h ~22-26, plain
  uint32_t, no lock in pipe_buf_t) are RMW'd by pipe_read (~54-56) and pipe_write (~111-113) on different CPUs
  with no lock/atomics -> torn count (a count--/count++ interleave loses an update), unreliable empty/full
  gating, torn/stale payload bytes. In-bounds (indices masked to the power-of-2 ring) so NOT OOB -- a data
  race / corruption, same class as F84 (pty ring) / F85 (evdev ring). Reachable: pipe() + sibling threads
  (THREAD_SHARE_FILES) read/write concurrently. Close-time lifetime, bounds, refcount/EOF are SAFE. Fix = a
  per-pipe spinlock around the head/tail/count/refs RMWs (mirror pty master_lock / evdev d->lock; drop before
  sched_sleep/wake). NOTE: PT_INTERP does NOT exist (static-only ELF loader), so the interp angle is moot.
  -> VERIFIED + FIXED (F93): confirmed pipe_buf_t has plain non-atomic head/tail/count and NO lock, and that
  pipe_read does count-- + head advance while pipe_write does count++ + tail advance with zero synchronization;
  the shared count is a non-atomic RMW from both sides, so two tasks sharing the pipe (fork DUPS the fd table
  but both fds point at the one pipe_buf_t; threads/SCM_RIGHTS likewise) racing read+write lose count updates
  (and count-- underflow wraps the u32 to ~4e9 = empty pipe looks full). REFINED two points (no-assumptions):
  (1) the recorded "drop before sched_sleep/wake" is necessary but NOT sufficient -- the read and write data
  paths touch the USER buffer, and sys_read PREFAULTS its dst (so draining into it under the lock cannot fault,
  exactly as pty_master_drain documents) but sys_write does NOT prefault its src (a write reads the buffer, so
  absent pages are demand-paged on ACCESS), so the write path must copy the user source into a kernel bounce
  buffer with the lock DROPPED and only then push into the ring under the lock; holding the lock across src[]
  would fault under the spinlock. (2) writer_refs/reader_refs are ALSO RMW'd lock-free (close hooks decrement,
  read/write read them), so I brought those under the same lock for a uniform discipline. FIX (do-it-right, ONE
  shared mechanism mirroring pty master_lock): added pipe_buf_t.lock (plain spinlock, never IRQ context).
  pipe_read drains available bytes into the prefaulted user dst under the lock, dropping it before WAIT_EVENT /
  wake. pipe_write snapshots full/reader under the lock, bounces a <=256B chunk user->kernel with the lock
  dropped, then pushes what fits under the lock (a count<PIPE_BUF_SIZE guard makes overflow impossible even if a
  concurrent writer refilled the ring) and wakes readers per chunk -- which also closes a latent missed-wakeup
  window in the old code (it filled the ring then only woke readers at the very end). close hooks clear
  reader_refs/writer_refs under the lock, dropping it before the peer wake and pipe_destroy (which frees the
  lock). Documented in docs/LOCKS.md. poll/ioctl keep their advisory lockless count read (32-bit aligned reads
  are atomic). VERIFICATION: a cross-CPU data race, not deterministically reproducible -> a NEW concurrent
  selftest (pipe_race_selftest): a writer + reader kthread on a SHARED pipe stream a known sequence (byte at
  offset k == k&0xFF); the reader verifies every received byte equals its offset value AND the total received
  equals what was sent; the writer closes the write end so the reader terminates on EOF (cannot hang even if
  bytes are dropped) -- `[pipe_race] SELF-TEST PASSED (in-order, no lost/torn bytes, 65536/65536)`. Pipes ARE
  boot-exercised (bash pipelines), so the clean boot is a real exercise. Clean boot first try (no F55 stall this
  time): DHCP lease (ifconfig IP 10.0.2.15 GW 10.0.2.2), 63 selftests PASSED incl. [pipe_refcount] and
  [pipe_race], 0 FAILED/PANIC, no [WD], `More login:` renders. This closes fan-out #4 candidate (ee).
  (cc) MEDIUM, data corruption not OOB -- file-backed mmap left-trim wrong-page: mm_vma_remove Case 3 (left
  edge overlap, kernel/mm/mm.c ~434-436) advances v->start = end but does NOT advance v->file_off / v->file_len
  (the split Case 4 ~413-415 and the ELF split both DO), so after a front munmap of a file-backed VMA every
  subsequent demand fault computes src_off = file_off + (page - start) shifted back by (end - old_start) and
  installs the WRONG file page. The read stays bounded (vmm.c ~847 clamps pg_off < file_len; ext2 pread clamps
  to fd->file_size) so it is NOT an OOB/UAF -- intra-file disclosure / silent data corruption + a wrong-keyed
  page-cache entry. (Same family as the shmem-pgoff aliasing noted SAFE-ish in fan-out #3, now for the file
  backing.) Fix = in Case 3 compute delta = end - v->start, then v->file_off += delta, v->file_len -= delta
  (clamp), before overwriting v->start (mirror Case 4). PRIORITY: (bb) dcache UAF first (sharpest -- HIGH,
  most reachable, clean fix mirroring T1), then (dd) timerfd UAF, then (ee) pipe ring, then (cc) mmap left-trim.
  -> VERIFIED + FIXED (F94): confirmed mm_vma_remove's front-trim (the `else` branch, mm.c ~435-436, labelled
  "Case 3 left edge overlap") advances v->start = end but leaves the backing offsets stale, while the split
  (Case 4 ~413-415) advances them; confirmed the fault path keys the backing off (page - vma->start) -- file:
  src_off = vma_file_off + (page - vma_start) (vmm.c ~816/847), shmem: pg_idx = (page - vma_start)/PAGE_SIZE +
  vma_shmem_pgoff (vmm.c ~961) -- so a stale offset maps a page (end - old_start) too early; bounded by file_len
  / file_size so intra-file disclosure, not OOB. REFINED the recorded prescription (verify-all-angles): it named
  only file_off/file_len, but Case 4 ALSO advances shmem_pgoff (unconditionally, ~403-404), and the shmem fault
  path keys off shmem_pgoff the same way -- so a front-munmap of a SHMEM-backed VMA has the IDENTICAL bug; the
  fix must advance shmem_pgoff too. Case 2 (right-edge / shrink-end, ~432) is CORRECT as-is: start is unchanged
  so the backing offset stays valid and the smaller end bounds later faults (matching Case 4's left fragment,
  which also leaves file_len untouched on an end-shrink). FIX (do-it-right, ONE shared mechanism, no drift):
  extracted a static helper vma_backing_advance(v, delta) that advances shmem_pgoff (page units) + file_off and
  clamps file_len, used by BOTH the front-trim (Case 3, applied to v) and the split (Case 4, applied to the
  right fragment after copying v's backing into it) -- field-by-field equivalent to the old inline Case-4 math
  (file / anon / shmem all verified), so no Case-4 regression. delta = end - v->start computed before start
  moves. VERIFICATION: an offset-math bug, not concurrency -> a deterministic selftest (mm_vma_trim_selftest):
  helper unit cases (file-backed 2-page front-trim advances file_off+file_len+shmem_pgoff; file_len underflow
  clamps to 0; anonymous keeps file_off/file_len 0 and still advances shmem_pgoff) PLUS a real-path case that
  builds a stack file-backed VMA + synthetic mm, calls mm_vma_remove to front-trim 2 of 4 pages (Case 3, no
  free), and asserts start/end/file_off/file_len all moved in lockstep. `[mm_vma_trim] PASS`. Likely NOT boot-
  exercised (a boot-time front-munmap of a file/shmem mapping then re-fault is not on the path), so the change
  is effectively inert at boot like the NVMe/ksec cases; the selftest is the real proof. Clean boot FIRST try:
  DHCP lease (ifconfig IP 10.0.2.15 GW 10.0.2.2), 64 selftests PASSED incl. [mm_vma_trim], 0 FAILED/PANIC, no
  [WD], `More login:` renders. This closes fan-out #4 candidate (cc) -- and with (bb)/(dd)/(ee)/(cc) all fixed,
  ALL of fan-out #4 is now resolved.
- 2026-06-26 FAN-OUT #5 (5 read-only Explore agents over un-swept subsystems: vmm/page-table teardown, tcp
  reassembly/retransmit, virtio-net rx/tx ring, futex/robust-list, ext2 dir rename/link/unlink; reported HIGH
  confidence each, NOT yet independently verified -- VERIFY against the real code before fixing, the (bb)/(dd)/
  (ee)/(cc) prescriptions ALL needed refinement). One SAFE: futex (kernel/proc/futex.c, 179 lines) -- the
  waiter-UAF class is correctly defended (futex.c:167-170 captures task_t* wtask = w->task and unlinks BEFORE
  the __ATOMIC_RELEASE store to w->woken, never derefs the stack waiter after; every wait exit re-acquires the
  bucket lock and self-unlinks; user addr validated via _access_ok + copy_from_user + 4-byte alignment; bucket
  index masked to a power-of-2 table; bucket lock never taken in IRQ context / always dropped before sleep); NO
  robust-list and NO REQUEUE/CMP_REQUEUE implemented, so those bug classes are absent. FOUR CANDIDATE bugs:
  (ff) HIGH, unprivileged LPE -- sys_exec frees the old PML4 hierarchy while sibling threads still run on it:
  sys_exec (kernel/syscall/syscall.c ~1072-1081) does pml4_phys = new_pml4 / vmm_switch(new_pml4) (current CPU's
  CR3 only) / vmm_free_user_ex(old_pml4) + pmm_buddy_free(old_pml4) with the comment "safe: no CPU walks
  old_pml4 now" -- but a sibling thread created via SYS_THREAD + THREAD_SHARE_MM (syscall.c ~1458, shares the
  same task_mm_t -> same pml4_phys, mm_shared->refs==2) is concurrently running user code on another CPU with
  CR3==old_pml4. exec issues NO sibling-termination and NO tlb_flush_mm/shootdown, so after vmm_free_user_ex the
  sibling's page-table walker walks FREED PT/PD/PDPT pages and its TLB caches WRITABLE entries for freed leaf
  frames the buddy allocator immediately recycles -> cross-domain write / disclosure (the "cross-owner UAF write
  / LPE" class). fork and the THREAD_SHARE_MM=false clone path DO tlb_flush_mm; exec is the unique mm-teardown
  path that frees page tables without quiescing other CPUs. Fix = before the commit block, quiesce + terminate
  every sibling sharing g_current->mm_shared and spin until mm_shared->refs==1 (reusing the refcount-as-
  quiescence contract task_mm_release relies on) BEFORE overwriting pml4_phys / vmm_switch / vmm_free_user_ex;
  tlb_flush_mm as defense-in-depth (but insufficient alone -- the sibling's CR3 still points at the freed PML4,
  so the threads must actually be gone). agent CONFIDENCE HIGH (read sys_exec 911-1138, no refs==1 gate / no
  shootdown / no thread-kill).
  -> VERIFIED + FIXED (F97): confirmed sys_exec's commit block (syscall.c ~1072-1081) does the in-place swap
  (mm_shared->pml4_phys = new_pml4) + vmm_switch (current CPU's CR3 only) + vmm_free_user_ex(old_pml4) +
  pmm_buddy_free(old_pml4) with NO sibling-kill and NO TLB shootdown (the comment reasons only about the
  single-threaded current-CPU case); confirmed sys_thread + THREAD_SHARE_MM (~1458-1460) shares the SAME
  task_mm_t and bumps refs to 2; confirmed the clone-without-share path (~1474) and fork (~550) DO tlb_flush_mm
  (the contrast). KEY REFINEMENT (the recorded "spin until mm_shared->refs == 1" is UNSAFE): a killed sibling
  drops its mm ref only in task_free_rcu (process.c:382) -- AFTER an RCU grace period -- so g_current spinning
  on refs==1 inside the exec syscall could stall that grace period and DEADLOCK. So the fix must NOT wait. FIX
  (do-it-right, additive + zero-risk to the proven common path): keep the sole-owner (refs==1) in-place-swap +
  direct-free path UNCHANGED (every boot exec is single-threaded, so the boot-critical path is untouched); for a
  SHARED mm (refs > 1) detach onto a FRESH task_mm_t (task_mm_alloc(new_pml4,new_mm)), SIGKILL the thread group
  (signal_send_group, then clear g_current's OWN pending SIGKILL since sig_group_visit also targets it), point
  g_current at the fresh mm, vmm_switch, and task_mm_release the old shared mm -- which does NOT free old_pml4
  here (siblings still hold refs) but lets task_mm_release free it when the last SIGKILL'd sibling exits
  (refs -> 0). So old_pml4 stays live while any sibling references it: no UAF, no spin, no deadlock. Reused the
  EXISTING refcount-based free (task_mm_release), the "one shared mechanism" the prime directive wants. RESIDUAL
  (correctness, not safety -- recorded in docs/SCALABILITY_DEBT.md): tgid/leader identity is left unchanged, so
  the rare NON-leader execve leaves a tgid pointing at the reaped old leader instead of doing the full POSIX
  de_thread leader-switch (benign -- no UAF; the surviving thread runs the new image). VERIFICATION: the refs>1
  branch is INERT at boot (all boot execs are single-threaded) so not end-to-end testable there -> a
  DETERMINISTIC threshold selftest (exec_mm_teardown_selftest: only refs==1 frees in place; refs 2/8/0 take the
  detach path) + rigorous code-proof + a clean boot that exercises the UNCHANGED sole-owner path on EVERY
  program launch (init/login/bash/net all execve). `[exec_mm] PASS (only sole-owner frees old pml4 in place)`.
  Clean boot FIRST try: DHCP lease (ifconfig IP 10.0.2.15 GW 10.0.2.2), 67 selftests PASSED incl. [exec_mm] and
  the neighbouring [task_exit_state] / [signal_send_pid], 0 FAILED/PANIC, no [WD], `More login:` renders. (Note:
  the session scratchpad had been wiped, so the boot harness bootclean.sh/shot.py/OVMF_VARS were recreated this
  turn.) This closes fan-out #5 candidate (ff).
  (gg) HIGH, remote-triggerable -- pcb_wake UAF write of the socket's vfs_file_t: pcb_wake (kernel/net/tcp.c
  ~213-215) loads f = pcb->sock_file then wait_queue_wake_all(f->waitq) which does __atomic_exchange_n(&wq->head,
  ...) = a WRITE; it runs from the net RX thread / TCP timer thread on any segment/timer for the pcb. sock_close
  (kernel/net/socket.c ~234 tcp_pcb_set_file(NULL) then ~248 kfree(self)) frees the vfs_file_t with a PLAIN
  synchronous kfree -- only s and the pcb are call_rcu-deferred (tcp_pcb_free_rcu / sock_free_rcu), NOT the
  file. If the RX thread loads pcb->sock_file (non-NULL) just before sock_close's NULL store, it holds a dangling
  f and after kfree(self) writes through f->waitq into freed heap. RCU keeps the PCB alive but NOT the file.
  tcp_pcb_set_file is an unlocked store; pcb_wake takes no lock -- no serialization. Fix = RCU-defer the
  vfs_file_t free too (fold self into an RCU callback / call_rcu) so a reader that already loaded sock_file
  finishes inside its RCU section before the file is reclaimed (pcb_wake already runs inside the tcp_recv/
  tcp_timer_tick RCU reader section); or refcount the file across the pcb backpointer. Same family as F88
  (/proc fdtable freed synchronously while an RCU reader holds it) and F89 (io_uring). agent CONFIDENCE HIGH
  (the four facts -- plain kfree at socket.c:248, unlocked deref+write in pcb_wake, unlocked set_file, wake_all
  writes wq->head -- all confirmed; RX/close are separate SMP threads); ruled out the txbuf/rxbuf ring overread
  (kmalloc(65536) rounds to a 128KiB buddy block, indices masked, copies clamped).
  -> VERIFIED + FIXED (F95): confirmed all four facts AND that pcb_wake (tcp.c:213-215) runs inside the
  tcp_recv (rcu 366-533) / tcp_timer_tick (rcu 544-590) reader sections, so RCU-deferring the FILE free is the
  clean fix. KEY DISCOVERY (verify-all-angles): the UDP path has the SAME class -- sock_poll_wake (socket.c:140)
  writes s->file->waitq from socket_deliver_udp's rcu_read_lock (socket.c:691), and s->file is the same
  vfs_file_t (s->file = f at socket_open:303, f->ctx = s) freed by the same kfree(self) at 248 -- so ONE fix
  covers both tcp and udp. And the fix is literally the EXISTING AF_UNIX discipline: unix_sock_free_rcu
  (unix_sock.c:371) already frees s->file in its grace-period callback for exactly this reason ("peers reach it
  via peer->file for poll wakeups ... freeing it here, not eagerly in unix_sock_close, closes the ->file UAF
  window"); the AF_INET path simply MISSED it. FIX (do-it-right, ONE shared mechanism mirroring AF_UNIX): (1)
  sock_free_rcu now frees s->file (== self) in the SAME grace period; (2) sock_close's main path no longer
  kfree's self synchronously (the !s early-return, which has no RCU reader, still frees synchronously -- safe);
  (3) tcp_pcb_set_file is now a __ATOMIC_RELEASE store and pcb_wake's sock_file load a __ATOMIC_ACQUIRE load, so
  a reader either observes the file (covered by the grace period) or NULL (skips). UDP needs no extra ordering:
  s->file is immutable after open and s is unpublished from the udp_table before the deferred free, so a reader
  holding s under RCU keeps both s and s->file alive. VERIFICATION: a cross-CPU RX-vs-close UAF, not
  deterministically reproducible -> code-proof (the file is now reclaimed ONLY via the grace period that the
  pcb_wake/sock_poll_wake readers run under, same as F88/F89/AF-UNIX) + a deterministic invariant selftest
  (sock_file_lifetime_selftest: opens an inet DGRAM socket, asserts s->file==f / f->ctx==s / type -- the
  backptr the dual-free relies on -- then closes it through the deferred path; `[sock_file_life] PASS`) + the
  close path is HEAVILY boot-exercised (DHCP uses UDP sockets open/close, so sock_close/sock_free_rcu run on the
  boot path). Clean boot FIRST try: DHCP lease (ifconfig IP 10.0.2.15 GW 10.0.2.2), 65 selftests PASSED incl.
  [sock_file_life], [socketpair], [scm_rights], [unix_refcount], [tcp_orphan], 0 FAILED/PANIC, no [WD], `More
  login:` renders. This closes fan-out #5 candidate (gg).
  (hh) HIGH, malicious/buggy device -- virtio-net RX desc_id bounded against the wrong count: virtio_net_rx_poll
  (kernel/net/virtio_net.c ~434) validates the device-supplied used->ring[].id with virtio_desc_id_valid (~303,
  index_ok(id, VIRTQ_SIZE=256)), but only VIRTQ_SIZE/2=128 RX buffers are ever allocated (~583, i<VIRTQ_SIZE/2)
  and posted (~336). A device-written id in [128,255] passes the check, reaches an uninitialized s_rx_bufs[id]
  ({.phys=0,.virt=NULL}) -> raw=NULL, memcpy(dst, NULL+VIRTIO_NET_HDR_LEN, device-controlled eth_len) (~450) =
  NULL-source read of attacker-length, AND re-posts a descriptor with addr=s_rx_bufs[id].phys=0 (~454) letting
  the device DMA-clobber physical frame 0; the desc-table writes also corrupt unused slots 128..255. The
  virtio_descid/nvme_cid hardening pattern was applied with the WRONG bound (ring capacity, not buffer count).
  Fix = bound RX ids against the real buffer count: a single source of truth VIRTQ_NUM_RX_BUFS=VIRTQ_SIZE/2 used
  by the alloc loop, the refill loop, the rx_poll check (index_ok(id, VIRTQ_NUM_RX_BUFS)) and the selftest; or
  allocate+post all VIRTQ_SIZE buffers so the capacity bound becomes correct. TX path (~409) unaffected (uses
  the locally-submitted idx; single TX desc 0 always valid). NOTE: this is a malicious-DEVICE threat so it is
  NOT boot-exercised by QEMU's benign NIC model -- the fix is a hardening/selftest, provably inert at boot.
  agent CONFIDENCE HIGH (grep-confirmed s_rx_bufs written only for [0,127]; validation admits [0,255]).
  -> VERIFIED + FIXED (F98): confirmed virtio_desc_id_valid (virtio_net.c:303) is index_ok(id, VIRTQ_SIZE=256)
  while the RX alloc loop (~583) and refill loop (~336) both run i < VIRTQ_SIZE/2, so s_rx_bufs[128..255] stay
  {phys=0, virt=NULL}; rx_poll (~434) gated on virtio_desc_id_valid (admits [0,255]) then uses s_rx_bufs[desc_id]
  .virt as the memcpy SOURCE (~450) and .phys as a re-posted descriptor addr (~454) -- so a device id in
  [128,255] => raw=NULL memcpy + a descriptor re-posted at physical frame 0. REFINEMENT (the recorded "bound to
  128" must NOT touch the generic validator): checked virtio_desc_id_valid's OTHER callers -- virtq_submit (307)
  and the TX completion (409) operate over the FULL 256-entry desc ring, so the generic 256 bound is CORRECT for
  them; only the RX path is tighter (its id also indexes the 128-populated s_rx_bufs[]). Also nailed down WHY
  desc_id maps to s_rx_bufs[desc_id]: rxq_refill pairs descriptor idx with buffer i, and the sequential free
  list makes the posted RX desc ids exactly 0..127, so a legit completion id equals its buffer index. FIX
  (do-it-right, ONE source of truth): added VIRTQ_NUM_RX_BUFS (= VIRTQ_SIZE/2) in virtio_net.h used by the alloc
  loop, the refill loop, AND a new RX-specific virtio_rx_id_valid(id) = index_ok(id, VIRTQ_NUM_RX_BUFS) that
  rx_poll now uses instead of the generic full-ring guard -- so the validation bound and the populated-buffer
  count can never disagree again. The generic virtio_desc_id_valid stays 256 for TX/submit. Fixed the misleading
  "VIRTQ_SIZE buffers" comments (the code only ever populated VIRTQ_SIZE/2). VERIFICATION: a malicious-DEVICE
  threat NOT triggerable by QEMU's benign NIC -> a DETERMINISTIC bounds selftest (virtio_rx_id_valid_selftest:
  0 and 127 valid; 128 and 255 -- in the desc ring but past the rx buffers -- REJECTED; 256 / garbage rejected)
  + the normal RX path IS boot-exercised (DHCP receives through rx_poll with legit ids 0..127), so a clean boot
  with DHCP proves the bound change did not break normal receive. `[virtio_rxid] SELF-TEST PASSED`. Clean boot
  FIRST try: DHCP lease (ifconfig IP 10.0.2.15 GW 10.0.2.2), 68 selftests PASSED incl. [virtio_rxid] and the
  neighbouring [virtio_descid] / [nvme_cid], 0 FAILED/PANIC, no [WD], `More login:` renders. This closes fan-out
  #5 candidate (hh) -- and with futex SAFE + (gg) F95 / (ii) F96 / (ff) F97 / (hh) F98 all shipped, ALL of
  fan-out #5 is now resolved.
  (ii) HIGH, crafted-image / SMP race -- ext2_rename frees a dst inode ignoring i_links_count: the dst-removal
  branch of ext2_rename (kernel/fs/ext2.c ~3061-3068) removes ONE dirent then UNCONDITIONALLY free_inode_blocks
  + sets i_links_count=0 + free_inode_num(dst_ino), never reading i_links_count -- diverging from ext2_unlink's
  correct count-aware logic (~2980-2991). On a multi-link regular dst (i_links_count>=2) it frees the inode +
  blocks while another name still references it -> the surviving dirent points at a freed/reissued inode (cross-
  link disclosure / on-disk-inode UAF). sys_link returns -EPERM (can't create a 2-link file at runtime), so the
  multi-link state comes from a crafted/corrupt image (the stated threat model). Secondary: ext2_unlink does NOT
  take s_rename_lock, so a concurrent unlink(dst) racing rename(*,dst) on the same single-link file double-frees
  the inode bit + blocks. Fix = mirror ext2_unlink (decrement i_links_count under the dst inode lock, free only
  when it hits 0) and factor the "remove one name + drop a link + maybe free" sequence into ONE helper called by
  both rename and unlink so the decrement-and-conditional-free is atomic and the paths cannot double-free. agent
  CONFIDENCE HIGH (3062-3068 sets links_count=0 with no count read, vs the correct ext2_unlink pattern same file).
  -> VERIFIED + FIXED (F96): confirmed ext2_rename's dst-removal (~3062-3068) does free_inode_blocks +
  i_links_count=0 + free_inode_num UNCONDITIONALLY with no count read, vs ext2_unlink (~2980-2991) which
  decrements then frees ONLY at 0; i_links_count is uint16_t (ext2.h:81); sys_link returns -EPERM (syscall.c
  ~5219) so a multi-link file can only come from a crafted/corrupt image (the threat model); the dir-dst case is
  REJECTED earlier (rename over a directory returns 0 at ~3050-3054), so only a regular-file dst reaches the
  free (no "." / ".." link accounting needed here). CONFIRMED the secondary race: ext2_rename's dir_remove_entry
  at ~3060 was UNCHECKED, while ext2_unlink CHECKS it (~2976) -- so a racing unlink(dst) that already removed
  the name + freed the inode let rename re-lock the freed dst_ino and free it again (double-free of the inode
  bit + blocks, runtime-reachable on a single-link file, no crafted image). FIX (do-it-right, ONE shared
  mechanism so the paths cannot drift): extracted a PURE ext2_link_drop(uint16_t* links) (decrement with a > 0
  underflow guard, returns 1 iff the count hit 0) and a shared ext2_drop_link_locked(leaf, ino) (the count-aware
  free tail, always consumes the inode lock); ext2_unlink now calls it, and ext2_rename's dst-removal calls it
  too BUT gated on dir_remove_entry succeeding -- if a racing unlink already removed the name, rename skips the
  drop (that path owns the free), closing the double-free without taking a coarser lock (the dir_remove_entry
  success is the single ownership token, which ext2_unlink already relied on). The old unconditional
  free_inode_num (which ran even when inode_lock failed) is gone -- on a re-lock failure rename now leaks rather
  than wrongly freeing, matching ext2_unlink's behaviour. VERIFICATION: the primary multi-link case needs a
  crafted image (can't be created at runtime, sys_link EPERM) so it is not end-to-end testable at boot -> a
  DETERMINISTIC unit test of the core decision (ext2_link_drop_selftest: 2->1 keep, 1->0 free, 3->2->1->0,
  underflow drop-at-0 stays 0 + reports free) + code-proof of the wiring + the rename/unlink paths are boot-
  exercised. `[ext2_link_drop] PASS (free only at links==0, no u16 underflow)`. Clean boot FIRST try: DHCP lease
  (ifconfig IP 10.0.2.15 GW 10.0.2.2), 66 selftests PASSED incl. [ext2_link_drop] and [rename_under] /
  [ext2_dotdot] (the neighbouring rename tests, unaffected), 0 FAILED/PANIC, no [WD], `More login:` renders.
  This closes fan-out #5 candidate (ii).
  PRIORITY (to verify+fix, sharpest first): (gg) tcp pcb_wake UAF first -- HIGH, remote-reachable UAF WRITE,
  cleanest root-cause fix (RCU-defer the file free, mirroring F88/F89); then (ii) ext2 rename link-count (clean,
  mirrors ext2_unlink); then (ff) exec-vs-threads PML4 free (most severe/unprivileged but the biggest, most
  careful fix -- deserves its own deep turn); then (hh) virtio-net RX bound (trivial surgical fix but malicious-
  device-only, not boot-exercised).
- 2026-06-26 FAN-OUT #6 (3 read-only Explore agents over un-swept subsystems: AHCI/NVMe DMA scatter-gather,
  shmem/CoW fault path, vfs path resolution/symlinks; reported confidence as noted, NOT yet independently
  verified -- VERIFY against the real code before fixing, EVERY fan-out #4/#5 prescription needed refinement).
  AHCI/NVMe SG came back essentially SAFE for the reachable threat model (every reachable PRDT/PRP builder bounds
  its entry count -- ahci_submit_hhdm/do_rw_direct use nprdt<248 + fail-on-remainder, ahci_submit_sg rejects
  npages>130, nvme_rw rejects >8KiB; sectors*size funneled through xfer_bytes_ok/mul_within_u32; the only device
  index cqe.cid is nvme_cid_valid-gated; slot/DMA-buffer lifetimes serialized) -- with ONE LATENT-only defect.
  THREE findings:
  (jj) HIGH, unprivileged -- CoW break in vmm_get_user_pages does a frame-swap PTE write with only a LOCAL invlpg
  and NO cross-CPU TLB shootdown (kernel/mm/vmm.c ~302-315, the local flush at ~313): all threads of a process
  share one page table (THREAD_SHARE_MM), so `*pte = nf` (swap to the freshly-copied frame) changes the
  translation for every sibling, but only the syscalling CPU's TLB is invalidated. A sibling on another CPU keeps
  a stale (old_phys, RO) TLB entry; after pmm_ref_dec drops old_phys to rc 1 and the CoW co-owner (fork peer)
  later frees it (rc 1->0 -> buddy free + recycle), the sibling reads old_phys through its stale TLB = cross-
  process use-after-free READ / info leak (benign variant: stale pre-DMA bytes = silent corruption). This is the
  GUP/DMA-pin CoW-break site; the OTHER two CoW-break sites DO shoot down -- isr14_page_fault's rc>1 break
  (vmm.c ~750) and fork (tlb_flush_mm, process.c ~551) -- so this one was left unflushed (same sibling-stale-TLB
  class as F97/brk/munmap, at a new site). REACHABLE: any multithreaded process that fork()s (CoW pages) then
  does an AHCI-backed read/pread/write/pwrite whose user buffer overlaps a CoW page while a sibling touches it.
  Fix = after the swap, once lock_mm->vma_lock is RELEASED (never while holding it -- IPI-ack deadlock, see
  vmm.c ~744-747), tlb_flush_range(task_get_mm_shared(g_current), va, va+PAGE_SIZE) (batch all swapped VAs into
  one flush after the loop for scale); the rc==1 RO->RW widen-only branch needs no shootdown. agent CONFIDENCE
  HIGH on the defect (the lone frame-swapping CoW break with only a local invlpg), MEDIUM on worst-case freed-
  frame leak (needs the co-owner to free within the stale window) / HIGH on the stale-read corruption variant.
  -> VERIFIED + FIXED (F99): confirmed vmm_get_user_pages's CoW break (vmm.c ~302-315) does *pte = nf (frame
  swap) + invlpg(va) (LOCAL only) + pmm_ref_dec(phys), all under lock_mm->vma_lock (acquired per-page ~296,
  released ~329), with NO tlb_flush_range; confirmed the contrast -- isr14_page_fault's CoW break (~720-752)
  records flush_page under the lock, UNLOCKS, then tlb_flush_range(task_get_mm_shared(g_current), ...) AFTER the
  unlock with an explicit IPI-ack-deadlock note, and its rc==1 widen-only branch sets no flush_page; confirmed
  REACHABILITY -- vmm_get_user_pages is called by ahci_read_user (ahci.c:893), the AHCI read/pread DMA-pin path,
  so a user read into a fork-CoW page hits the break. FIX (do-it-right, mirror the EXISTING isr14/fork shootdown
  discipline -- one shared mechanism applied to the missed site, BATCHED per the prime directive): set a
  cow_broke flag in the CoW-break branch; converted the function's early returns to a single out: exit;
  restructured to return `ret`; and at out: -- AFTER every per-page vma_lock is released -- if cow_broke do ONE
  tlb_flush_range(task_get_mm_shared(g_current), uaddr&~0xFFF, uaddr&~0xFFF + count*PAGE_SIZE) covering the whole
  pinned span (one IPI round for all swapped pages, vs N). The rc==1 widen-only branch sets no flag (a stale RO
  entry just re-faults harmlessly). The local invlpg(va) stays for immediate local consistency. Verified the OOM
  / !pte / not-present / widen / unpin paths all still behave (the goto out reaches the flush+return on every
  exit; over-flushing non-CoW pages in the span is safe). VERIFICATION: a cross-CPU stale-TLB race, NOT
  deterministically reproducible -> rigorous code-proof (identical to the isr14 shootdown the codebase already
  trusts) + the function is boot-exercised on every AHCI DMA-pin read (file loads), so a clean boot proves the
  restructure did not break the pin/DMA/demand-paging path (the cow_broke flush is gated and mostly inert at
  boot). No bespoke selftest: the flush-range arithmetic is trivial and a true cross-CPU race is not
  deterministic (same honest proof method as F97/F95/F89). Clean boot FIRST try: DHCP lease (ifconfig IP
  10.0.2.15 GW 10.0.2.2), 68 selftests PASSED, 0 FAILED/PANIC, no [WD], `More login:` renders. This closes
  fan-out #6 candidate (jj).
  (kk) MEDIUM, unprivileged sandbox bypass -- sys_readdir is missing the unveil_ok gate (kernel/syscall/syscall.c
  ~1740, the check belongs after fs_lookup ~1775 before the dispatch ~1779): unveil_ok (syscall.c ~328) is the
  single unveil enforcement point and EVERY other path syscall calls it (open 411, exec 961, stat 1860, unlink
  1900, rename 1962, mkdir 2116, access 3935, truncate 5370) but sys_readdir's body (1740-1823) has no unveil
  reference, and fs_lookup/ext2_lookup_path only do ACL checks -- so a process confined with unveil() can
  readdir() any directory outside its unveiled set and enumerate names + inode#/size/is_dir (open/stat on those
  names are still blocked, so it is metadata enumeration, not memory corruption). Distinct from F38 (which fixed
  the prefix math + added the gate to unlink/rename/mkdir/rmdir/truncate/exec -- readdir was simply never added).
  Fix = after `fs_lookup(path, ...)` add `if (!unveil_ok(path, UNVEIL_READ)) { kfree(kbuf); kfree(path); return
  -ENOENT; }` (free both buffers on the deny path), mirroring sys_stat. agent CONFIDENCE HIGH the gate is absent.
  -> VERIFIED + FIXED (F100): confirmed sys_readdir's full body (syscall.c ~1740-1823) has NO unveil_ok call --
  fs_lookup (~1775) only does ACL_PERM_READ|EXEC checks; confirmed sys_stat IS the model (~1859-1860:
  fs_lookup then `if (!unveil_ok(path, UNVEIL_READ)) return -ENOENT` BEFORE the fsr check, to hide); confirmed
  fs_lookup calls normalize_path in place (virtfs.c:383) so `..` cannot bypass the prefix check; and confirmed
  UNVEIL_READ is the RIGHT constant -- unveil.h:27 documents it as "O_RDONLY, stat, readdir". FIX (do-it-right,
  mirror sys_stat exactly, ONE shared mechanism): inserted `if (!unveil_ok(path, UNVEIL_READ)) { kfree(kbuf);
  kfree(path); return -ENOENT; }` immediately after fs_lookup, BEFORE the fsr check and BEFORE the is_virtual
  dispatch, so it covers ext2 AND /proc AND /dev uniformly (matching sys_stat) and hides existence too; both
  kmalloc'd buffers (kbuf ~1769, path ~1747) are freed on the deny path. No-op when no unveil is active
  (unveil_ok allows all until the first unveil() call), so unconfined readdir is unchanged. VERIFICATION: the
  fix wires readdir to the EXISTING, already-tested unveil_ok/unveil_check decision -> no new selftest needed
  (the [unveil] selftest covers prefix + boundary + perm bits incl. UNVEIL_READ, and [virtfs_unveil] covers the
  mount boundary; adding a redundant readdir-specific unveil test would re-test the same mechanism). Code-proof
  (identical to the 8 sibling call sites: open/exec/stat/unlink/rename/mkdir/access/truncate) + clean boot
  (login/bash/ls readdir with no unveil active -> allowed, so the gate must not break unconfined listing).
  Clean boot FIRST try: DHCP lease (ifconfig IP 10.0.2.15 GW 10.0.2.2), 68 selftests PASSED incl. `[unveil]`
  and `[virtfs_unveil]`, 0 FAILED/PANIC, no [WD], `More login:` renders (directory listing works). This closes
  fan-out #6 candidate (kk).
  (ll) LOW, latent/unreachable -- build_prdt (kernel/drivers/storage/ahci.c ~402-418) writes prdt[n++] with NO
  n<248 bound (unlike its three siblings), so a bytes value > 248*page_span would OOB-write past prdt[248] off
  the 4KiB cmd_table_t; but its ONLY caller ahci_read_multi (~946) always passes bytes=PAGE_SIZE (1 entry) and
  ahci_read_multi itself has ZERO callers (dead code), so it is NOT currently reachable. Fix (defense-in-depth,
  mirror the siblings) = `while (bytes > 0 && n < 248u)` + caller fails on unconsumed remainder. agent
  CONFIDENCE HIGH it is a real latent defect AND HIGH it is unreachable today.
  -> VERIFIED + FIXED (F101): confirmed build_prdt (ahci.c) had `while (bytes > 0)` with NO n<248 bound while
  the three siblings (do_rw_direct ~633, ahci_submit_hhdm ~765 use nprdt<248 + fail-on-remainder; build_prdt_pages
  bounded by npages); confirmed prdt[248] is a 4KiB cmd_table_t (ahci.c:131); confirmed the sole caller
  ahci_read_multi (~946) passes PAGE_SIZE (1 entry) and ahci_read_multi has ZERO callers (grep: only the ahci.h
  decl + comments) -> dead code, latent/unreachable. FIX (do-it-right, mirror the siblings' discipline): bounded
  the loop `while (bytes > 0 && n < 248u)` so it can never write past prdt[247], and `if (bytes) return 0` to
  signal overflow (the documented "caller must fail" contract, like ahci_submit_hhdm's fail-on-remainder). Left
  ahci_read_multi as-is (it passes PAGE_SIZE so it always gets 1 and can never trigger 0; wiring a slot-freeing
  overflow path into the dead caller would entangle slot management for an unreachable branch -- the helper's
  return-0 contract is documented for any FUTURE large-bytes caller). VERIFICATION: a DETERMINISTIC unit test
  (ahci_build_prdt_selftest, heap array since 250 entries > 8KiB kstack): 1 page -> 1 entry; exactly 248 pages
  -> 248 entries all consumed; 249 pages -> returns 0 (overflow signalled) AND a canary at prdt[248] is
  UNTOUCHED (proves the bound stopped at entry 247, no OOB). `[ahci_prdt] SELF-TEST PASSED (PRDT bounded to 248,
  overflow signalled)`. The fix is in DEAD code so inert at boot; clean boot just confirms nothing else broke.
  Clean boot FIRST try: DHCP lease (ifconfig IP 10.0.2.15 GW 10.0.2.2), 69 selftests PASSED incl. [ahci_prdt]
  and [ahci_xfer], 0 FAILED/PANIC, no [WD], `More login:` renders. This closes fan-out #6 candidate (ll) -- and
  with AHCI/NVMe SG SAFE + (jj) F99 / (kk) F100 / (ll) F101 all shipped, ALL of fan-out #6 is now resolved.
  PRIORITY (to verify+fix, sharpest first): (jj) CoW GUP missing-shootdown first -- HIGH, reachable, cross-
  process UAF-read, clean fix mirroring the isr14/fork shootdown; then (kk) unveil readdir gap (clean, mirrors
  the sibling syscalls); then (ll) build_prdt bound (trivial defense-in-depth on dead code). NOTE: this fan-out
  ran 3 agents (not the usual ~5); bcache/buffer eviction + tty ldisc deeper remain un-swept for a later round.
- 2026-06-26 FAN-OUT #7 (5 read-only Explore agents: bcache eviction, tty/pty line discipline, /dev node
  lifetime, epoll, io_uring SQ/CQ; reported confidence as noted, NOT yet independently verified -- VERIFY each
  before fixing, EVERY fan-out prescription so far needed refinement). FIVE candidate findings:
  (pp) HIGH, unprivileged LPE -- epoll_close frees epoll_state_t + the epoll vfs_file_t SYNCHRONOUSLY while a
  watched fd's IRQ-context epoll_wake_func still derefs them: each epoll_watch registers a persistent epoll_we_t
  on the WATCHED fd's waitq; epoll_wake_func (kernel/wait.c ~261-274) derefs ewe->p_has_ready (&state->has_ready),
  ewe->epoll_wq (&state->wq), ewe->file_wq (the epoll vfs_file's _waitq) -- pointers that live INSIDE the epoll
  object, not the watch. epoll_close (kernel/syscall/syscall.c ~4917-4920) call_rcu's the watch w (so the entry's
  OWN memory survives) but then SYNCHRONOUSLY kfree's state->slots, state, and self. A device IRQ wake on CPU A
  (already past the dead-flag check inside wait_queue_wake_all's RCU reader) races close(epfd) on CPU B: after the
  free, CPU A does __atomic_store_n(ewe->p_has_ready,1) into freed state + wait_queue_wake_all(&state->wq /
  self->_waitq) walking freed (reallocatable) head/func -> a controllable indirect call / write-what = LPE. The
  code's OWN comment (~4830) promises an epoll_state_free_rcu that was NEVER written (runs straight into
  epoll_read). Distinct from the watched-file pin (reverse direction) and the generic fd UAF. Fix = add
  epoll_state_free_rcu and RCU-defer the state+slots+self free (mirror epoll_watch_free_rcu and F88/F95/F99), so
  the free waits out every in-flight wait_queue_wake_all drainer; the free MUST cover self (file_wq points into
  it). agent CONFIDENCE HIGH (watch is RCU-freed but state/self are not; wake_func provably derefs both; close
  takes no lock/RCU the wake path observes; the orphaned 4830 comment names the missing helper).
  -> VERIFIED + FIXED (F102): confirmed epoll_wake_func (wait.c:262-274) derefs ewe->p_has_ready (a write into
  &state->has_ready), ewe->epoll_wq (drain &state->wq) and ewe->file_wq (drain self's _waitq) and returns
  WQ_KEEP (stays registered, runs from IRQ); confirmed state->file_wq = f->waitq (= &self->_waitq) at
  sys_epoll_create, so the ewe references BOTH state and self; confirmed epoll_close (syscall.c) call_rcu's the
  watch w (epoll_watch_free_rcu, so the ewe MEMORY survives a grace period) but SYNCHRONOUSLY kfree'd
  state->slots + state + self; confirmed epoll_watch_unregister removes the ewe from the watched waitq + drops
  the w->file pin but does NOT wait for an in-flight wait_queue_wake_all (an RCU reader already past the dead
  flag still calls epoll_wake_func) -> the in-flight wake writes/walks freed state/self = cross-CPU UAF +
  controllable indirect call through the freed waitq chain; confirmed the orphaned ~4830 comment trails off
  exactly where epoll_state_free_rcu should be (it was never written -- runs straight into epoll_read). FIX
  (do-it-right, ONE shared mechanism, the exact helper the comment promised, mirroring epoll_watch_free_rcu +
  F88/F95/F99 RCU-defer-the-free): added a back-pointer `struct vfs_file_t* file` to epoll_state_t (set to f in
  sys_epoll_create), wrote epoll_state_free_rcu(state) that kfree's state->slots + state->file (== self) +
  state, and restructured epoll_close to call_rcu_expedited(epoll_state_free_rcu, state) AFTER unregistering all
  watches instead of the three synchronous kfrees (the no-state branch still frees self synchronously -- nothing
  holds its waitq). So state+slots+self are reclaimed in the same grace period as the watch, after every
  in-flight epoll_wake_func drainer has left its RCU reader section: no UAF, no double-free of self (it is freed
  ONLY via the deferred path; epoll_close is the sole close hook). VERIFIED-ALL-ANGLES: no-watches case (defer
  still correct), watched-fd-closed-first (the w->file vfs_tryget pin keeps the watched waitq alive until
  unregister, so no dangling ewe -- unchanged), self freed once, call_rcu_expedited manages its own node (no
  embedded rcu_head needed, like epoll_watch_free_rcu). VERIFICATION: a cross-CPU IRQ-vs-close UAF, NOT
  deterministically reproducible -> code-proof (identical to the established epoll_watch_free_rcu / F88 / F95
  RCU-defer pattern, which the code's own comment already prescribed) + the existing [epoll_pin] selftest + a
  clean boot that exercises epoll create/ctl/wait/close HEAVILY (the sway/swaybar/foot desktop event loops),
  proving the RCU-defer did not break epoll lifetime / leak / double-free (same honest proof as F88/F89/F95).
  Clean boot FIRST try: DHCP lease (ifconfig IP 10.0.2.15 GW 10.0.2.2), 69 selftests PASSED incl. [epoll_pin],
  0 FAILED/PANIC, no [WD], `More login:` renders. This closes fan-out #7 candidate (pp).
  (mm) HIGH, unprivileged -- bcache poisoning: ext2_vfs_read (kernel/fs/ext2.c ~1272-1275) and ext2_vfs_pread
  (~1363-1366) warm the SHARED cross-process block cache by bcache_fill(phys_blk+i, dest+i*bs, bs) where dest is
  the USER buffer when is_user (the is_user tag at ~1263/1357 is NOT applied to the cache-warm loop). The zero-
  copy fast path DMAs disk data into the user pages via ahci_read_user, which UNPINS them before returning
  (ahci.c ~907-914); then the cache-warm loop re-reads from user memory. A sibling thread (shared mm) overwrites
  the user buffer between the DMA and the bcache_fill memcpy -> the shared bcache slot tagged phys_blk holds
  attacker bytes, served to ANY later reader of that on-disk block (cross-process cache poisoning / integrity
  bypass; secondary: a racing munmap faults the memcpy on a user VA in kernel mode). Fix = guard the cache-warm
  loop with `if (!is_user)` (only warm the cache from a trusted kernel source; the demand-paging path uses
  is_user==0 and is unaffected), mirroring the ahci_read (trusted) vs ahci_read_user (untrusted) split. agent
  CONFIDENCE HIGH on the bug/reachability, MEDIUM on max priv-esc impact. (Rest of bcache audited SAFE: lock-free
  per-way Dekker protocol correct, static BSS buffers (no UAF), pin get/put balanced, write-through (no dirty
  writeback), LBA/index bounds-checked.)
  -> VERIFIED + FIXED (F103): confirmed ext2_vfs_read (ext2.c:1263-1275) computes is_user = (dest < HHDM_OFFSET)
  and gates the DMA (ahci_read_user vs ahci_read) on it, but the cache-warm loop (1272-1275) bcache_fill(phys_blk
  +i, dest+i*bs, bs) was UNCONDITIONAL, copying from the user `dest` into the SHARED static cache slot tagged by
  the physical block; ext2_vfs_pread (1356-1366) is identical. So a sibling thread (shared mm) overwriting `dest`
  between the DMA and the bcache_fill memcpy poisons the cross-process cache slot with attacker bytes, served to
  any later bcache_get(phys_blk) reader of that on-disk block (integrity bypass; secondary: a racing munmap
  faults the memcpy on a user VA in kernel mode). The kernel-read path (ELF loader / demand paging) uses
  is_user==0 (a trusted HHDM kmalloc'd staging buffer) and is the path the warm comment's "second exec is a
  cache hit" actually serves. FIX (do-it-right, mirror the codebase's ahci_read (trusted) vs ahci_read_user
  (untrusted) split): guard BOTH cache-warm loops with `if (!is_user)`, so the shared cache is warmed ONLY from a
  trusted kernel source; the user-read path forgoes the opportunistic warm (a later read cold-misses and re-fills
  from disk -- correct by construction) and the returned data on the fast path is unchanged (only the cache
  SIDE-EFFECT is gated). VERIFICATION: a one-line correctness gate (the user-path warm was never a needed side
  effect) -> code-proof + the file-read path (read/pread) and the cache are HEAVILY boot-exercised (login/bash/
  net read files; [dcache_test]/[dcache_race]/[ext2_*] selftests exercise the cache + demand paging), so a clean
  boot proves gating the user warm did not break reads or the kernel-path cache. First boot hit the flaky F55
  userspace-start stall (net ready, login + /bin/net paged, no DHCP lease, NO fault, all 67-69 kernel selftests
  PASSED -- so the change is provably not the cause); the SAME-build re-boot reached the DHCP lease (ifconfig IP
  10.0.2.15 GW 10.0.2.2), 69 selftests PASSED incl. the dcache/ext2 cache tests, 0 FAILED/PANIC, no [WD],
  `More login:` renders. This closes fan-out #7 candidate (mm).
  (oo) HIGH, pty-owner reachable -- pty slave reopen UAF: pty_slave_close (kernel/drivers/tty/pty.c ~417, kfree
  at ~432) frees the shared slave vfs_file_t UNCONDITIONALLY even when the master is still open (do_free=0), but
  never NULLs pty->slave_file nor clears slave_claimed, leaving the pty node in s_pty_head with a dangling
  slave_file. A reopen of /dev/pts/N (pty_open_slave_by_index ~698) passes the ineffective `if (!p->slave_file)`
  guard (~706) and resurrects the freed object with __atomic_fetch_add(&p->slave_file->refcount,1) (~714) on
  freed slab, installs the dangling pointer as a live fd -> every read/write/ioctl/poll on freed memory + a later
  close drives slave_open_count negative (double-free/unlink). Deterministic single-process: open(ptmx),
  open(pts/N), close(slave), open(pts/N). /dev/pts/N is mode 0620 uid 0 so the pty OWNER (any process that
  created the pty) can hit it on its own pty. Fix = in pty_slave_close under s_pty_lock, when !do_free set
  pty->slave_file=NULL + slave_claimed=0; in reopen use vfs_tryget(p->slave_file) (fails if refcount hit 0)
  instead of the raw fetch_add; and on reopen with slave_file==NULL + master alive, build a fresh slave
  vfs_file_t+ctx on demand (decouple the shared vfs_file_t lifetime from the pty -- the real root cause). agent
  CONFIDENCE HIGH (the free is unconditional, slave_file never cleared on this path, reopen derefs+resurrects it
  past the NULL guard; pty_lifetime_selftest never covers close-slave-then-reopen-with-master-alive).
  -> VERIFIED + FIXED (F104): confirmed pty_slave_close (pty.c) does kfree(self) even when do_free==0 (master
  still open) and never NULLed pty->slave_file / slave_claimed on that path, leaving a dangling pointer in the
  s_pty_head node; pty_open_slave_by_index's else branch did a raw __atomic_fetch_add(&p->slave_file->refcount)
  on it (resurrect-on-freed-slab) past the ineffective `if (!p->slave_file)` guard. REFINED the recorded fix
  (no-assumptions): the recorded "build a fresh slave on reopen" requires refactoring the boot-critical pty_alloc
  AND resolving a not-fully-built window (the pty enters s_pty_head before pty_open_master assigns slave_file),
  which is more risk than warranted for a path the normal terminal flow never hits (the shell opens its slave
  ONCE and keeps it; dup/fork share via the refcount; an open-while-already-open succeeds via vfs_tryget -- only
  a reopen AFTER a FULL close is the edge case). The SECURITY bug is the UAF, closed surgically: (1) pty_slave_
  close, under s_pty_lock when !do_free, sets pty->slave_file = NULL + slave_claimed = 0 if it == self (clears
  the dangling pointer so a later open sees NULL); (2) pty_open_slave_by_index's additional-open branch uses
  vfs_tryget(p->slave_file) (fails -> NULL if the file is mid-teardown, closing the last-close-vs-open race)
  instead of the raw refcount bump. So a reopen-after-full-close now returns NULL (a clean failure) rather than
  resurrecting freed memory. The reopen-build-fresh FUNCTIONALITY (Linux devpts reopen) is recorded as a
  follow-up in docs/SCALABILITY_DEBT.md (not a safety gap -- the UAF is fully closed). VERIFICATION: a NEW
  DETERMINISTIC selftest (pty_reopen_selftest) drives the exact scenario -- alloc, claim slave, close slave with
  master open, then assert pty->slave_file==NULL + slave_claimed==0 + slave_open_count==0 (the dangling pointer
  is cleared) and that a reopen returns NULL (no resurrection); plus pty is HEAVILY boot-exercised (the desktop/
  foot/login open ptmx + slave + read/write), so a clean boot proves normal pty open/close still works. `[pty_
  reopen] SELF-TEST PASSED (slave_file cleared on close, no reopen UAF)`. First boot hit the flaky F55 stall
  (net ready, login + /bin/net paged, no DHCP, 0 faults, 70 selftests PASSED incl. [pty_life]/[pty_reopen]/[pty_
  mring]); the SAME-build re-boot reached the DHCP lease (ifconfig IP 10.0.2.15 GW 10.0.2.2), 70 selftests
  PASSED, 0 FAILED/PANIC, no [WD], `More login:` renders. This closes fan-out #7 candidate (oo).
  (qq) HIGH, unprivileged -- io_uring SQ consumer unserialized: io_uring_enter_impl's SQPOLL-skip guard
  (kernel/io/io_uring.c ~1046-1050) only fires for (SQ_WAKEUP && !GETEVENTS), so enter(GETEVENTS) on a SQPOLL
  ring falls through into the unlocked consumer loop (~1058-1119) which races the always-running SQPOLL poller's
  identical loop (~581-602): both read head (plain load), pick the same sqe, dispatch_exec it TWICE (double
  send/open/write), post two CQEs, and non-atomically store head back (lost update -> unbounded re-dispatch).
  Memory-safety escalation: for an async SQE both call io_wq_ensure_worker (~757) whose non-atomic
  load-NULL->create->store (~782) lets two consumers each spawn a worker; the second store orphans the first, and
  io_uring_close_file joins only uring->worker (the second) and frees uring while the orphan still runs -> UAF
  (distinct from F89, which fixed join ORDER of the single known worker). Also the concurrent-enter() variant
  (two threads sharing the ring fd, sys_io_uring_enter takes no ring lock) double-consumes with no SQPOLL. Fix =
  (1) skip SQ submission for ALL SQPOLL enters (goto getevents regardless of flags, poller is the sole consumer);
  (2) a per-ring submit_lock across the head-read->dispatch->head-store for non-SQPOLL concurrent enters (mirrors
  Linux uring_lock); (3) CAS-publish uring->worker so a race cannot orphan a worker. agent CONFIDENCE HIGH on the
  double-execution/double-completion (the loop is provably lock-free and the poller is a 2nd active consumer the
  guard fails to exclude for the common GETEVENTS call), MEDIUM-HIGH on the orphaned-worker UAF.
  -> VERIFIED + FIXED (F105): confirmed io_uring_enter_impl's SQPOLL-skip (io_uring.c ~1046-1051) only fired for
  (SQ_WAKEUP && !GETEVENTS), so enter(GETEVENTS) and enter(0) on a SQPOLL ring fell through into the unlocked
  consumer loop (plain `head` load at ~1059); the SQPOLL poller (io_sqp_kthread_entry ~580-602) runs the same
  loop with NO lock (relying on sole-consumer); io_wq_ensure_worker (~757) did a non-atomic load-NULL ->
  task_create_kthread -> RELEASE-store, so two consumers each spawn + the 2nd store orphans the 1st (close joins
  only uring->worker -> the orphan touches freed uring = UAF). REFINED the fix (no-assumptions): split the
  memory-safety bugs from the correctness bug. The MEMORY-SAFETY bugs are the SQPOLL enter-vs-poller double-
  consume + BOTH cases' orphaned-worker UAF. FIX (do-it-right, low-risk, NO lock held across blocking dispatch
  -- IORING_OP_WRITE is sync-dispatched and blocks on disk I/O, so a submit-lock across dispatch would be sleep-
  under-spinlock): (a) enter NEVER consumes a SQPOLL ring regardless of flags (goto getevents; the poller is the
  sole consumer, closing the SQPOLL double-consume + its orphan, no lock); (b) added a per-ring worker_lock and
  made io_wq_ensure_worker double-checked-locked so AT MOST ONE worker is ever spawned (closes the orphaned-
  worker UAF for EVERY consumer-race path, incl. concurrent non-SQPOLL enters; held only on the first async op,
  no sleep under it). The remaining NON-SQPOLL concurrent-enter DOUBLE-EXECUTION (two threads sharing the ring
  fd both running the consumer -> a duplicate op + CQE; correctness, NOT memory-safety since the orphan UAF is
  closed) is recorded as a follow-up in docs/SCALABILITY_DEBT.md (its proper fix needs copy-SQE-then-dispatch-
  outside-the-lock to avoid holding a spinlock across a blocking WRITE; deferred to keep this surgical). VERIFY-
  ALL-ANGLES: the goto skips only mask/user_tail/head (unused at getevents); submitted=0 for a SQPOLL enter
  (correct -- it submits nothing); worker_lock init'd in the ring ctor + documented in docs/LOCKS.md; close-vs-
  enter-spawn already serialised by the enter fdget ref. VERIFICATION: a cross-CPU race, not deterministically
  reproducible -> code-proof + the existing [io_uring_test] (drives create -> NOP-batch + steady-state enter/
  dispatch/complete -> close) and [io_ring_idx] selftests both PASS (the submit/complete path is unbroken) + a
  clean boot. First boot hit the flaky F55 stall (net ready, login + /bin/net paged, no DHCP, 0 faults, 70
  selftests PASSED incl. [io_uring_test]/[io_ring_idx]); the same-build re-boot reached the DHCP lease (ifconfig
  IP 10.0.2.15 GW 10.0.2.2), 70 selftests PASSED, 0 FAILED/PANIC, no [WD], `More login:` renders. This closes
  fan-out #7 candidate (qq).
  (nn) MEDIUM, unprivileged -- tty termios update race: TCSETS/TCSETSW/TCSETSF copy_from_user straight into the
  live tty->termios with NO tty->lock (kernel/drivers/tty/pty.c ~459-465; the /dev/tty fallback syscall.c
  ~4372-4380), racing the lock-free termios reads in tty_input_char (tty.c ~147-218, reads c_lflag + c_cc[]) and
  tty_ldisc_drain (tty.c ~265-266, reads c_lflag/c_cc[VMIN]) on another CPU -> a reader observes a half-written
  termios (ICANON/ISIG flipped, a c_cc[VINTR/VSUSP/VQUIT] byte torn) -> spurious SIGINT/SIGQUIT/SIGTSTP to the
  fg pgrp or a swallowed ^C or a line drained in the wrong mode. NOT memory corruption (every c_cc index is a
  constant < NCCS=19, the struct has no pointers); the tty.h lock comment deliberately scopes the lock to exclude
  termios, so this is a NEW gap not covered by F84/F85. Fix = copy_from_user into a stack-local termios_t tmp
  OUTSIDE the lock (it can fault), then publish under tty_lock (tty->termios = tmp); snapshot the needed termios
  fields under the lock on the read side (or guard termios with a seqlock to keep the hot input path lock-free).
  agent CONFIDENCE HIGH it is a real SMP data race, severity bounded MEDIUM (no memory-safety consequence).
  PRIORITY (to verify+fix, sharpest first): (pp) epoll UAF first -- HIGH, fully unprivileged, IRQ-context UAF
  with a CONTROLLABLE indirect call (LPE), cleanest fix (the RCU-defer the code's own comment already names,
  mirroring F88/F95/F99); then (mm) bcache poisoning (clean one-line is_user guard); then (oo) pty reopen UAF
  (clean: NULL + tryget + rebuild); then (qq) io_uring SQ serialization (biggest, multi-part fix); then (nn) tty
  termios race (MEDIUM, data race not corruption).
  VERIFIED+FIXED (F106): confirmed against the real code -- all THREE writer sites copy_from_user STRAIGHT into
  the live tty->termios with NO lock (pty_tty_ioctl_common TCSETS/W/F at pty.c:471, shared by master+slave
  ioctl; the /dev/tty fallback TCSETS/W at syscall.c:4374 and TCSETSF at 4377), and the readers
  (tty_input_char's c_lflag/c_cc/c_iflag at tty.c:147/148/225/237; tty_ldisc_drain's c_lflag/c_cc[VMIN] at
  265/266) read lock-free on another CPU; tty->lock is preempt_disable + spinlock (so copy_from_user, which can
  fault, must NOT run under it); VINTR=0/VQUIT=1/VMIN=6/VSUSP=10 are all constant < NCCS=19 with no pointers in
  the struct -> data race, MEDIUM, no memory corruption (confirmed). REFINEMENT (no-assumptions): publishing the
  copy under tty->lock is NOT sufficient on its own -- `tty->termios = tmp` is itself a multi-word copy, so a
  lock-free reader could still catch it mid-assignment; BOTH ends must take the lock. Also surfaced TWO MORE
  whole-struct readers the recorded prescription missed: TCGETS (pty.c:466, syscall.c:4370) copy_to_user the
  live struct, handing userland a torn copy under the same race. FIX (do-it-right, one shared mechanism):
  extracted the tty_set_termios / tty_get_termios primitive (PRIMITIVE, tty.c) -- publish/snapshot the whole
  termios under tty->lock; copy_from_user/copy_to_user run on a stack local OUTSIDE the lock. All three TCSET*
  sites publish through tty_set_termios; tty_input_char snapshots once at entry via tty_get_termios; both TCGETS
  sites snapshot through it; tty_ldisc_drain moved its mode/VMIN reads inside its existing lock (zero added
  cost). Output-path single-field reads (c_oflag OPOST/ONLCR, c_lflag TOSTOP) intentionally stay lock-free
  (atomic single-field loads, no cross-field signal/safety dependency, hot path). New deterministic selftest
  tty_termios_snapshot_selftest (byte-for-byte round-trip + full-overwrite republish + ldisc obeys the published
  mode); clean boot, `[tty_termios] SELF-TEST PASSED`. This closes fan-out #7 candidate (nn).
  *** FAN-OUT #7 FULLY RESOLVED *** -- (pp) F102, (mm) F103, (oo) F104, (qq) F105, (nn) F106 all VERIFIED+FIXED.
  NEXT PHASE: bug-TYPE codebase scans (one bug type at a time), starting with the sync-free-under-reader class
  (the F88/F95/F102 RCU-defer family, recurred 3x).

- 2026-06-26 BUG-TYPE SCAN #1 (sync-free-under-reader): the F88/F95/F102 class -- a SHARED object kfree'd
  synchronously while a concurrent reader on another CPU (RCU walker, IRQ/timer wait_queue wake, poll/epoll
  waiter) can still dereference it -> use-after-free. Swept the WHOLE tree with 4 parallel read-only agents
  (net, fs+vfs, drivers, mm+proc/io). ONE real gap found, FIXED (F107); everything else audited CLEAN.
  REAL GAP (FIXED, F107): kernel/proc/signal.c:443-444 (signal_deliver_pending, fatal SIG_DFL terminate) dropped
    the fd table in the WRONG order -- task_files_release(g_current->files_shared) THEN g_current->files_shared =
    NULL (a plain store). task_files_release -> call_rcu_expedited (rcu.c:245: synchronize_rcu_expedited(); func()
    -- frees INLINE), and a /proc/<pid>/fd reader (proc.c:290-298/567-600) does rcu_read_lock(); tf =
    load_acquire(&t->files_shared); ft = tf->ft; vfs_tryget(ft->fd_table[n]). Because the inline grace period
    elapses while files_shared is STILL published, a reader that opens its rcu_read_lock section just after it
    loads an already-freed task_files_t/fdtable_t and vfs_tryget's a freed vfs_file_t vtable -> cross-process UAF.
    The task is still in pid_ht (task_set_exit_state runs later, signal.c:460), so it is sched_find_pid-able. The
    sibling sys_exit path (syscall.c:727) already does it correctly (unpublish RELEASE-store BEFORE release) with
    a comment naming this exact contract -- the two had DRIFTED. FIX (F107): extracted task_drop_files() (the one
    shared unpublish-before-release mechanism) and routed BOTH paths through it so they cannot drift again.
  AUDITED CLEAN (provably, not sampled -- every shared-object free in scope):
    net/: socket.c sock_free_rcu (call_rcu_expedited + RELEASE/ACQUIRE sock_file), udp_table_free_rcu, tcp.c
      tcp_pcb_free_rcu (+ listener-orphan-under-wlock), unix_sock.c unix_sock_free_rcu (refcount + tryget +
      peer-backpointer-under-lock), ns_table/arp_table free_rcu -- all RCU-deferred or refcount-pinned. (One
      OUT-OF-SCOPE note: AF_UNIX backlog_head link/unlink in connect/accept vs unix_sock_free_rcu's walk is a
      list-mutation race, NOT this UAF class -- logged for a separate look.)
    fs+vfs: every vfs_file_t close is refcount-pinned (sys_close removes the fd under tf->lock, teardown runs
      only on 1->0; readers reach it via fdget->vfs_tryget). timerfd (pc->lock holds the IRQ tick's wake AND
      close's removal), signalfd (s_sfd_lock), pipe (open_ends shared-lifetime + ACQ_REL wake-before-free),
      dcache (RCU + SLAB_TYPESAFE_BY_RCU + DYING-CAS), eventfd, ext2/fat32/proc close (no IRQ/poll waitq reader),
      epoll (F102: tryget-pin + call_rcu_expedited) -- all CLEAN.
    drivers: evdev per-open client+ring vs the IRQ producer is unlink-under-d->lock(irqsave)-then-free (the
      F88/F95 model); pty (F104 NULL-under-lock + tryget); tty per-open ctx (producer touches only global g_tty0);
      drm per-fd event ring (synchronous producer, no async vblank IRQ, io_uring path VFS-refcount-pinned); hda/
      ac97/nvme/ahci/fb/virtio_gpu use static/global slot state, no per-open free -- all CLEAN.
    mm+proc/io: io_uring close joins the SQPOLL+io_wq kthreads before free & pins the GETEVENTS waiter via fdget
      (F105 worker_lock makes it a single joinable worker); exec-vs-siblings (F97 detach + refcount free); task/
      mm teardown (call_rcu_expedited(task_free_rcu) after pid_ht/idx removal); TLB-shootdown IPI (cpu_mask is a
      CR3 shadow cleared in do_switch, IPI derefs only BSS); futex (mm is a compare-only hash key, never deref'd;
      task snapshotted before the RELEASE woken-store); shmem (refcount + tryget + call_rcu); VMA free
      (rcu_assign_pointer + call_rcu_head); slab page return (SLAB_TYPESAFE_BY_RCU call_rcu_head); fdtable grow
      (RELEASE publish + call_rcu_expedited old) -- all CLEAN except the signal.c gap above.
  VERDICT: the codebase is consistently hardened against this class via four mechanisms (refcount-pin+tryget,
  RCU-defer, unlink/unpublish-before-free under the waker's lock, kthread-join). The single drift (signal.c) is
  fixed; SCAN #1 is COMPLETE. Next: BUG-TYPE SCAN #2 (next highest-value type).

- 2026-06-26 BUG-TYPE SCAN #2 (sibling-stale-TLB-after-PTE-change): the F97/F99 class -- a PTE cleared/downgraded
  in an mm live on another CPU, the frame then freed/reused while a sibling writes through the stale TLB entry
  (write-after-free into a recycled frame) or keeps writing a shared page through a stale RW entry (CoW-isolation
  leak). Swept the WHOLE tree with 4 parallel read-only agents (vmm munmap/mprotect, CoW/fault, teardown/reclaim/
  shmem, kernel-side remaps). NO ACTIVE GAP found -- the class is already correctly hardened. Shipped a HARDENING
  of a latent landmine the sweep surfaced (F108).
  TLB API (ground truth): tlb_flush_range(mm,s,e)/tlb_flush_mm(mm) (tlb.c:250/254) IPI exactly mm->cpu_mask
    (a precise CR3 shadow set/cleared in do_switch) + self, spin-wait on ACK, take NO lock, emit a SEQ_CST fence
    (tlb.c:202) to close the Dekker race with switch_mm -- so callers MUST flush with the mm/vma lock DROPPED.
    tlb_flush_kernel_range(s,e) (tlb.c:268) is the all-CPU variant for shared kernel mappings. No global pages
    (PAGE_GLOBAL defined but unused), so mov-to-cr3 is a full local flush.
  AUDITED CLEAN (every PTE clear/downgrade in scope; flush-before-free + all-CPU + outside-lock, OR a no-sibling
  precondition, OR metadata-only, OR refcounted-frame-never-recycled):
    munmap/MAP_FIXED/brk-shrink: mm_unmap_range_flush_free (syscall.c:2392) does unmap -> tlb_flush_range (2410,
      all CPUs, no lock) -> pmm_ref_dec free (2411): flush-before-free, correct.
    CoW break: vmm_get_user_pages (vmm.c:315) batched tlb_flush_range at out: (376, after every vma_lock drop) =
      F99; isr14 #PF CoW (vmm.c:745) single-page tlb_flush_range (770) after vma_lock drop. Old frame rc>1 (not
      freed). Permission-WIDEN (RO->RW, vmm.c:324/754) and fresh not-present->present installs correctly skip the
      shootdown (a too-restrictive stale entry only re-faults; x86 caches no not-present entry).
    fork/sys_thread write-protect: vmm_clone_user_ex downgrades the PARENT RW->RO, then tlb_flush_mm(parent) at
      process.c:612 + syscall.c:1532 (all CPUs, no lock). Frame shared (rc bumped), not freed.
    teardown: vmm_free_user_ex (process.c:39) runs only at refs==0 (cpu_mask empty, RCU grace elapsed -> no live
      sibling); sole-owner exec frees old tables only after vmm_switch on the only CPU; MULTITHREADED exec (F97)
      detaches onto a FRESH mm + SIGKILLs siblings + refcount-frees the old pml4 later -> clears no live PTE.
    shmem/pcache/DRM destroy-while-mapped: drop only the OBJECT's per-frame ref (pmm_ref_dec); never clear a PTE,
      never pmm_buddy_free a mapped frame -- a frame mapped elsewhere is held by that mapper's PTE ref; the
      eventual unmapper runs flush-before-free. io_uring fd teardown: unmap -> tlb_flush_range -> buddy_free.
    kernel-side: kstack recycle (tss.c) uses all-CPU tlb_flush_kernel_range at the NEXT alloc before the slot has
      an owner; MMIO window + kheap demand-map are fresh not-present->present at never-reused VAs; HHDM/kernel
      PML4 are boot-only. No vmalloc/ioremap/iounmap/kmap/fixmap exist (audited-clean by absence).
  HARDENING SHIPPED (F108) -- latent fragility, NOT an active bug: do_switch (sched.c:1495) cleared prev's cpumask
    bit then set next's UNCONDITIONALLY; for a same-mm thread switch (prev->mm_shared == next->mm_shared) that
    nets to a no-op but opens a transient window where this CPU's bit reads 0 while CR3 still points at the mm, so
    a concurrent tlb_flush_mm could SKIP this CPU. It is currently SAFE only because context_switch ALWAYS reloads
    CR3 when pml4_phys != 0 (full flush) right after -- but the sched.c comment FALSELY claimed context_switch
    "skips the CR3 write" for same-mm, inviting a future optimization that would turn the window into a real
    stale-TLB UAF. FIX: update the cpumask ONLY when prev->mm_shared != next->mm_shared (the bit stays set for
    thread switches -> strictly conservative, never under-targets a running mm; removes the window; saves two
    atomics on the hot path) + corrected the comment to state the true mechanism + the invariant the safety rests
    on. Verified by code-proof (strict-conservatism) + a clean SMP boot with the chaselev work-stealing stress
    (20000 ops, 0 missing/0 duplicates) and all 73 selftests, 0 faults.
  OUT-OF-CLASS observations (logged for later scans, NOT this type):
    (a) vmm_clone_user_ex intra-walk RW->RO window: a parent sibling can write through a stale RW entry into an
        already-downgraded, now-child-shared frame during the (long) clone before the batched tlb_flush_mm -- a
        CoW-isolation/data-integrity nuance on a still-live shared frame (refcounts stay correct, no frame freed),
        NOT a UAF. Candidate for a CoW-correctness look.
    (b) s_mmio_next (vmm.c:131/143) is bumped with NO lock -- a data race on the MMIO VA allocator (concurrent
        driver init), not a TLB bug. Candidate for the data-race SCAN.
    (c) vmm_page_free / vmm_page_alloc (vmm.c:383/395) are DEAD CODE (no callers); vmm_page_free does a local-only
        invlpg + free, so if ever revived for a runtime KERNEL VA it would be a real all-CPU-shootdown gap.
        Candidate for a dead-code sweep / guard with a comment.
  VERDICT: the sibling-stale-TLB class is already hardened (flush-before-free + no-sibling preconditions +
  refcounted frames); the shootdown is already primitised (tlb_flush_range/tlb_flush_mm/tlb_flush_kernel_range),
  so nothing new to extract. F108 removed the one latent landmine. SCAN #2 COMPLETE. Next: BUG-TYPE SCAN #3.

- 2026-06-26 BUG-TYPE SCAN #3 (unvalidated-user-pointer / arbitrary-kernel-RW): the highest-severity class -- a
  syscall/ioctl dereferences a user-supplied pointer DIRECTLY (raw *(T*)uptr store/read, memcpy/memset, cast-to-
  struct) instead of routing through copy_from_user/copy_to_user/_access_ok (which validate canonical + user-half
  + prefault, return -EFAULT). A raw deref of a kernel/non-canonical pointer = arbitrary kernel WRITE (LPE),
  arbitrary kernel READ (info leak), or a #PF panic (DoS). Swept the WHOLE user->kernel pointer surface with 4
  parallel read-only agents (driver ioctls; net syscalls; core syscalls; io_uring+ipc). SIX real gaps found, all
  in kernel/syscall/syscall.c, all unprivileged -- FIXED THIS TURN (F109); TWO lower-severity net-layer gaps
  recorded for next turn; driver ioctls + io_uring + the rest audited CLEAN.
  FIXED (F109, all in syscall.c, all routed through copy_*_user):
    sys_wait (1685): raw `*sp = (code&0xFF)<<8` to user status_ptr -> arbitrary kernel WRITE (LPE), every waitpid.
    sys_pipe (2190/2209): raw `fds[0]=rfd; fds[1]=wfd` to user int[2] -> arbitrary kernel WRITE (LPE) + leaked the
      installed fds on a bad pointer; every pipe(). (Flagged independently by the net AND core agents.)
    sys_fb_info (2977): raw 24-byte struct write (incl. a phys addr) to user ptr -> arbitrary kernel WRITE (LPE).
    sys_nanosleep (2889): raw `req->tv_sec/tv_nsec` read -> kernel READ oracle + #PF panic (DoS).
    sys_shm_open (2740) / sys_shm_unlink (2852): raw `__builtin_memcpy(name, uname, namelen)` from user ptr ->
      kernel READ + #PF panic (namelen WAS bounded to SHMEM_NAME_MAX, but the POINTER was unvalidated).
  FIXED (F110, the net-layer pair -- lower severity, #PF-DoS / 1-bit oracle):
    setsockopt optval (kernel/net/socket.c:635, the SO_BROADCAST `int v = *(const int*)optval`): the optval
      pointer was passed from sys_setsockopt (syscall.c) with no validation -> #PF/#GP DoS + a 1-bit "is *KADDR
      nonzero" oracle. FIXED: sys_setsockopt now bounces optval into a 128-byte kernel buffer via copy_from_user
      (rejects vallen > 128 with -EINVAL) and passes the COPY to socket_setsockopt, so no option handler ever
      derefs the raw user pointer.
    AF_UNIX sun_path (sys_bind/sys_connect/sys_sendto cast addr_ptr to a USER sockaddr_un and walked sa->sun_path
      as an unbounded C-string via serial_puts_dbg + unix_sock_bind/connect/sendto -> ns_hash_str/ns_streq; the
      base was user_buf_check-validated for 110 bytes but a 108-non-NUL sun_path read past it into the next,
      possibly-unmapped page -> #PF DoS). FIXED: extracted copy_sockaddr_un_from_user (PRIMITIVE) which
      copy_from_user's the whole sockaddr into a kernel sockaddr_un and FORCE-NUL-caps sun_path[len-1]; all three
      sites now use the kernel copy, so no user memory is walked. ns_hash_str/ns_streq additionally capped at
      UNIX_PATH_MAX (safe-by-construction; identical hash/compare for every real NUL-terminated path). New
      unix_ns_str_selftest (bounded hash/streq, NUL-stop).
  AUDITED CLEAN (provably -- the whole surface):
    driver ioctls: EVERY .ioctl across drivers routes `arg` (and embedded user sub-pointers) through copy_*_user --
      DRM (per-handler copy_from/to_user on the top-level arg AND every nested objs_ptr/props_ptr/modes_ptr/id
      array), evdev (copy_*_user_n wrappers for all EVIOCG*/S*; EVIOCGRAB treats arg as an int value), pty/tty
      (the F-precedent), tty0 fallback incl. TIOCGSERIAL (the other precedent), pipe/unix FIONREAD. fb/virtio_gpu/
      storage/hda/input-cores expose NO ioctl. CLEAN.
    net syscalls: bind/connect/accept/sendto/recvfrom (user_buf_check before any deref; addrlen IN/OUT never
      written raw -- getsockopt/getsockname/getpeername/accept4 are NOT implemented, so those classic addrlen
      attack surfaces do not exist), sendmsg/recvmsg (copy_*_user for msghdr + each cmsg + the iovec ARRAY + every
      iov_base chunk -- the usually-sloppy iovec path is correct), socketpair (copy_to_user with fd rollback --
      the template sys_pipe now matches). CLEAN except the setsockopt/sun_path gaps above.
    core syscalls: read/write (user_buf_prefault/user_buf_readable_ok), stat/fstat/readdir/getcwd, sigaction/
      sigprocmask/sigreturn (+canonical gates + MXCSR sanitize), poll/select/epoll_wait, gettimeofday/timerfd,
      uname/getrusage/times/getgroups, argv/envp (copy_user_strv per element+string), fb_blit (bounded prefault),
      FUTEX (copy_from_user for the initial read AND the post-enqueue re-read; only WAIT/WAKE exist, no user-word
      CAS), set_fs/arch_prctl (a value, never deref'd). CLEAN.
    io_uring + ipc: every SQE-supplied address (READ/WRITE/RECV/SEND/CONNECT/ACCEPT/OPENAT/DRM_COMMIT) validated
      via user_buf_check/copy_*_user at dispatch, in the SUBMITTER's mm (workers adopt owner_task->mm_shared + the
      scheduler unconditionally reloads CR3, so validation and deref hit the same address space); SQE double-fetch
      closed by a snapshot; ring indexing uses kernel-trusted masks not the user ring_mask; eventfd/signalfd/
      timerfd buffers validated by the read/write prefault; epoll_event in/out via copy_*_user; ksec wire uses
      kernel-resident structs (no user pointer on the wire). READV/WRITEV/SENDMSG/RECVMSG are not io_uring opcodes.
      CLEAN.
  VERDICT: the validation primitive (copy_from_user/copy_to_user/_access_ok/user_buf_check) is applied almost
  everywhere; the six syscall.c gaps (F109) + the two net-layer gaps (F110) were the stragglers, now all closed.
  SCAN #3 COMPLETE (8 gaps fixed across F109+F110; the rest of the user->kernel pointer surface audited clean).
  Next: BUG-TYPE SCAN #4.

- 2026-06-26 BUG-TYPE SCAN #4 (data-race on shared mutable state): a field/counter/list/flag mutated or read
  from multiple CPUs (cross-core or process-vs-IRQ) WITHOUT a lock or correct atomic/order -> torn/stale/lost-
  update/list-corruption (NOT UAF, NOT a benign single aligned-word flag load). Swept the whole tree with 4
  parallel read-only agents (net; mm+proc; drivers+io; fs+ipc). Found a CLUSTER in AF_UNIX (one root: no per-
  socket data lock), one UDP-rx-ring gap, one fd_flags lost-update, and the latent s_mmio_next. Fixed the sharpest
  (the AF_UNIX backlog double-free, F111); the rest are recorded below for the next turns.
  ROOT (AF_UNIX): unix_sock_t carried NO per-socket data lock -- unix_sock.h:106 literally says "Protected by
    single-threaded kernel". Four queues race producer-vs-consumer across CPUs:
    (A) backlog_head/tail/count -- connect() push (unix_sock.c ~1187) vs accept() pop (~1052) vs a 2nd connect,
        all under rcu_read_lock only (which keeps the socket alive but does NOT serialize the list) -> torn list,
        lost/over-limit count, and DOUBLE kfree(pend)/unix_put(client). FIXED (F111): added unix_sock_t.lock
        (per-socket spinlock); connect re-checks LISTENING+limit and links under target->lock, accept pops under
        listener->lock (re-check head inside), alloc + wait_queue wakes stay OUTSIDE the lock, never nested with
        s_unix_pair_lock; free_rcu drains post-grace-period single-threaded (RCU-serialized vs connect, accept
        cannot run on a refcount-0 listener) so it needs no lock. Documented in docs/LOCKS.md.
    (B) dgram_head/tail/count -- unix_sock_sendto enqueue vs unix_sock_recv_ex dequeue, no lock -> torn list /
        lost message / count lost-update. FIXED (F112): sendto links under target->lock (the queue owner), recv_ex
        dequeues under self->lock re-checking the head inside; the kmalloc + the user-data memcpy (into the kernel
        msg node, BEFORE the enqueue) and the user copy-out (AFTER the dequeue) + the wakes stay OUTSIDE the lock.
        free_rcu drains post-grace-period single-threaded (no lock).
    (C) stream cbuf buf_head/tail/count -- cbuf_write on the SENDER's CPU mutates the PEER's buffer (buf_count +=)
        while cbuf_read on the owner's CPU does buf_count -= : a non-atomic RMW from two CPUs = permanent fill-
        level corruption + torn stream bytes. FIXED (F113, the kernel-bounce refactor): cbuf_write/cbuf_read became
        cbuf_write_locked/cbuf_read_locked -- they take the buffer-OWNER's lock internally and operate on a KERNEL
        buffer; the send/recv callers bounce the user data through a 512-byte stack chunk OUTSIDE the lock (UNIX_BUF
        _SIZE is 8 KiB, too big for a stack bounce; the chunk also caps lock-hold time) and loop, so the byte copy +
        buf_count + index are now atomic w.r.t. the peer with NO user access under the spinlock. A sender into B
        locks B->lock, a reader of B locks B->lock (same -- serialized); A->B and B->A use different locks (no
        AB-BA). The blocking-when-full / blocking-when-empty WAIT_EVENT conditions stay unlocked (benign re-checked
        reads). EOF/EPIPE/EAGAIN/EINTR + partial-write semantics preserved.
    (D) ancillary (SCM_RIGHTS) files[]/head/tail/count -- sendfd (into the PEER's ancillary) vs recvfd (own) vs
        free_rcu drain, no lock -> count lost-update, a passed fd leaked/double-delivered, free_rcu
        double-vfs_close. FIXED (F112): sendfd enqueues under peer->lock (re-checking the limit inside; the vfs_dup
        is outside, undone with vfs_close on a full re-check), recvfd / recvfd_nb dequeue under self->lock
        re-checking count inside; wakes outside.  Only kernel vfs_file_t* pointers move under the lock (no user copy).
  OTHER GAPS:
    UDP inet rx ring (socket.c udp_rx_head/tail/count): socket_deliver_udp vs socket_recv/recvfrom vs sock_free_rcu
      drain -- rcu keeps the socket alive but does not serialize the queue -> count lost-update + torn head/tail
      (leaked skb / write onto freed skb). FIXED (F114): added socket_t.udp_rx_lock (a plain spinlock -- VERIFIED
      that socket_deliver_udp runs in net_rx_thread, a KTHREAD, not a hard IRQ, so no irqsave needed); the deliver
      enqueue re-checks the full-queue limit and links under the lock, recv/recvfrom dequeue under the lock
      re-checking the head inside + retry the wait on a lost multi-receiver race; the skb free, the user copy-out,
      and the wakes stay OUTSIDE the lock. sock_free_rcu drains post-grace-period at refcount 0 (deliver is
      RCU-serialized, recv cannot run on a closed fd) so it is single-threaded -- no lock. Verified directly by the
      DHCP lease (the UDP rx ring carries the DHCP OFFER/ACK). TCP rings stay on pcb->lock (this lock is UDP-only).
    fd_flags cloexec lost-update (syscall.c, sys_socket_inner): after fd_install returned, `tf->ft->fd_flags[fd] =
      FD_CLOEXEC` was a PLAIN store with no tf->lock, while a sibling thread's fd_table_grow COW-copies fd_flags
      under tf->lock and RCU-publishes a new table -> the FD_CLOEXEC store could land in the old (pending-RCU-free)
      table and be lost (fd survives exec -- the wayland/foot fd-leak class) + a minor write to the freed old table.
      VERIFIED it was the LONE outlier (sys_open / dup2 / F_SETFD / openpty set fd_flags under the lock already).
      FIXED (F115, the chokepoint): added fd_install_cloexec(f, cloexec) which sets fd_table[fd] AND fd_flags[fd] in
      ONE tf->lock critical section; fd_install(f) delegates with 0; sys_socket_inner installs with the cloexec flag
      atomically (the racy post-install store is gone). Also explicitly writes fd_flags[fd] (was relying on the slot
      being pre-cleared), so a reused slot can never carry a stale flag. The other 7 fd_install callers are
      unchanged (they delegate). Boot-proven (every fd-creating syscall flows through fd_install; the timerfd_race
      stress drives 20000 install/close iters).
    s_mmio_next unlocked + unbounded (vmm.c): vmm_map_mmio read-then-bumped the monotonic MMIO VA cursor with NO
      lock/atomic AND no ceiling. CONFIRMED LATENT: every caller is a boot-serial initcall on the BSP (run_dag is
      single-threaded; APs parked) so there is no concurrency yet -- it becomes an active overlap the moment any
      non-serial caller (AP init / hot-plug / deferred probe) appears. HARDENED (F116, like F108's do_switch
      landmine -- latent, not active): reserve a disjoint [base, base+span) via __atomic_fetch_add(&s_mmio_next,
      span, RELAXED) so two concurrent callers can never get overlapping VAs, + a documented MMIO_VIRT_END ceiling
      (1 TiB window, collision-free vs the HHDM below / kernel image above) with an overflow-safe bound (span >
      MMIO_VIRT_END - base) so it can never run past the window into adjacent kernel VA (a 0 return + a diag
      kprintf on exhaustion, which never fires at real device counts). The map is a fresh not-present->present
      install at a never-reused VA, so no TLB shootdown is needed (SCAN #2). Boot-proven: every MMIO driver
      (virtio-net/gpu/input, ahci, nvme, hda, ioapic) maps its BARs through it -- net + framebuffer up.
  DOCUMENTED-KNOWN / lower priority (recorded): TCP snd_nxt / send-snapshot / state / waiter (tcp.c, the T4
    "remaining" note at tcp.c:170-174 -- snd_nxt advanced without pcb->lock, send reads an unlocked snapshot,
    pcb->state torn multi-writer, pcb->waiter a single slot that loses a 2nd blocker's wakeup); ac97_write
    unlocked ring (DEAD -- ac97 has no initcall, /dev/dsp -> hda); timerfd_gettime reads home_cpu outside t->lock
    (stale remaining-time only); dcache_grow_locked plain hash_next splice (benign on x86 TSO); pcache `accessed`
    CLOCK bit touched under two different locks (benign heuristic).
  AUDITED CLEAN (mechanism): TCP ring accounting + accept queue (pcb->lock), virtio tx (s_tx_lock) / rx (single
    consumer + lfence/mfence), ARP/UDP-port/unix-ns/PCB tables (RCU + per-table wlock), pmm refcount (g_pmm_lock)
    + g_free_pages (atomics), PCP/slab per-CPU (cmpxchg16b + remote-free Treiber), VMA list (vma_lock + RCU),
    shmem (refcount + per-object lock + RCU ns), pid pool / pid_ht / pgid-tgid-sid idx (locks + RCU), run/sleep/
    zombie lists + children Treiber stack + task state (rq_lock / CAS / atomics), cpu_mask (atomic or/and + SEQ_CST
    fence -- the SCAN #2 Dekker barrier), futex buckets (b->lock + read-before-publish), signal pending/blocked
    (atomic bit ops), evdev ring+client list (d->lock irqsave), virtio_input/kbd/mouse (SPSC, BSP-pinned IRQ via
    ioapic RDT_DEST(0)), tty/pty rings (tty->lock / master_lock, kthread producer not IRQ), virtio_gpu (synchronous
    under s_ctrl/cursor_lock, no IRQ), drm event ring (SPSC release/acquire) + modeset seqlock, ahci (s_ci_lock
    irqsave + done-release/acquire), nvme (per-CPU queues, MSI-X steered), hda (SPSC + mfence), io_uring (cq_lock
    + SQPOLL sole consumer + overflow_lock), eventfd/timerfd/signalfd (atomics / pc->lock+t->lock / s_sfd_lock),
    dcache (RCU + DYING-CAS + g_dcache_wlock), bcache (Dekker pin/tag seqlock), inode irtree (per-leaf seqlock),
    pipe (p->lock), epoll (state->lock + has_ready release/acquire), fd table reads (RCU + tryget), poll/select
    (fdget + wait_group), wait-queue MPSC (CAS). The codebase is overwhelmingly lock/atomic-correct.
  *** SCAN #4 COMPLETE *** -- AF_UNIX cluster (A/B/C/D, F111-F113), UDP inet rx ring (F114), fd_flags cloexec
    lost-update (F115), s_mmio_next latent hardening (F116). Every active data race the sweep found is fixed; the
    rest of the kernel was audited lock/atomic-correct. Documented-known lower-priority residuals stay parked (the
    TCP T4 snd_nxt/state/waiter set, timerfd_gettime home_cpu, dcache hash_next splice, pcache accessed bit). Next:
    BUG-TYPE SCAN #5.

- 2026-06-27 BUG-TYPE SCAN #5 (double-free / unbalanced-refcount): an object freed more than once, or a refcount
  dropped more times than taken (-> premature/double free), or a free on an error path the caller also frees.
  Distinct from SCAN #1 (sync-free) and SCAN #4 (data-race on the count). Swept the whole tree with 4 parallel
  read-only agents (net; fs+vfs; drivers+io; mm+proc). ONE reachable double-free found + FIXED (F117); a cluster of
  audited-clean; plus recorded LEAK/NULL-deref/convention candidates of OTHER classes.
  FIXED (F117): sys_pipe failure-path double-free (syscall.c). On `goto fail` (fd_table_grow OOM), if the read end
    was ALREADY installed (rfd >= 0 -- the grow can fail while searching for the WRITE slot, after r was installed
    at fd_table[rfd]), the `fail:` path did a bare vfs_close(r), freeing it via the pipe refcount while
    fd_table[rfd] still pointed at the freed r -> a later sys_close / process-exit task_files_release double-frees
    r (heap corruption / UAF). Reachable when the table is one slot from full + grow OOMs; exit guarantees the 2nd
    free. FIX: at fail:, sys_close(rfd) for an INSTALLED read end (clears the slot + drops the ref), vfs_close(r)
    only when never installed; the write end is never installed on this path. Mirrors the socketpair + copyout-fail
    rollback discipline. (The copyout-fail path above already did this; the fail: path was the lone holdout.)
  AUDITED CLEAN (the double-free class, with the mechanism): net -- every skb (single-owner handed downward, freed
    once by the terminal layer), tcp_pcb_free (unlink under wlock + RCU-defer, every caller NULLs its pointer),
    sock_free_rcu (NULL-after-free of s->pcb), unix_get/unix_put + unix_refcount_tryget (free only on 1->0, never
    resurrect from 0; backlog/dgram_dest/pin_peer/owner refs all 1:1), SCM_RIGHTS transfer (one in-flight dup ref,
    dropped exactly once by recipient close / free-rcu drain / ENOBUFS rollback). fs+vfs -- pipe_destroy (open_ends
    ACQ_REL, free on 0 only), eventfd/signalfd/timerfd close (free ctx + NULL self->ctx + free self), dcache
    (unlink-under-wlock + DYING-CAS, parent ref +1/-1 once), epoll (tryget-pin + RCU-defer + fd_install contract),
    fd_install contract (success transfers ownership / failure leaves it with the caller -- every caller honors it,
    installed ends closed via sys_close, un-installed via vfs_close). drivers+io -- pty (do_free under s_pty_lock,
    NULL-after-free of slave_file = F104), io_uring (worker/SQPOLL stop+JOIN before free, fixed_files NULL-after-
    swap under lock, single setup-fail rollback), evdev (unlink under d->lock), drm gem/fb (per-page refcount 1:1,
    idempotent cursor drop, clone resource-id neutered). mm+proc -- pmm_ref_dec (free only on 1->0 under g_pmm_lock
    + rc==0 guard + tripwires), CoW break (per-PTE-lock re-check, one dec), the F107 exit-teardown SIBLINGS all
    single-release (files_shared unpublish-before-release; mm_shared/cwd/cred/unveil freed only in task_free_rcu;
    task_destroy runs once -- do_switch[DEAD] vs sys_wait[ZOMBIE] are state-disjoint), shmem (dec + pages[i]=0
    guard), VMA (rcu unlink before the destroy walk), all alloc-helper error paths kfree directly (not via the
    RCU task_destroy) so no caller double-frees. ALL refcounts are u32 -- no realistic wrap.
  RECORDED for later (OTHER classes the agents surfaced -- NOT double-free, do NOT fix this turn):
    LEAK (net, remotely-triggerable DoS): half-open TCP child PCBs in TCP_SYN_RCVD are never reaped (tcp.c -- ~128
      KiB per SYN that never gets the final ACK; a SYN flood exhausts kernel memory), and listener close orphans
      completed-but-unaccepted children in accept_queue (tcp_pcb_free only NULLs child->listener). Fix: an RTO/
      attempt cap that frees half-open children + free the accept_queue on listener close. Candidate for a LEAK
      scan or a TCP-hardening turn.
    NULL-DEREF: virtio_net_rx_poll can return 1 with *skb_out == NULL (skb_alloc fail) -> net_rx_thread calls
      eth_recv(NULL); fat32_open derefs f/fd without checking kmalloc. Candidate for a NULL-deref scan.
    CONVENTION (drm.c create_dumb kmalloc-fail branch uses pmm_buddy_free instead of the per-page pmm_ref_dec every
      other branch uses -- currently safe (rc==1 just-alloc'd) but leaves the rc array inconsistent; normalize).
    LATENT (mm): pcache_evict_one unconditionally pmm_ref_dec+kfree(cand) even if the hash unlink did not find it
      -- safe today (single reclaim thread; pcache_evict_inode has zero callers) but a real double-free if a 2nd
      evictor is wired up. Fix: gate the dec+free on a found flag.
  SCAN #5 IN PROGRESS: the reachable double-free (sys_pipe) is FIXED (F117); the rest of the class is audited clean.
    No other reachable double-free remains -> SCAN #5 effectively COMPLETE; advance to SCAN #6 next turn.

- 2026-06-27 BUG-TYPE SCAN #6 (resource-leak / exhaustion): an alloc/get never freed/put on a reachable path,
  especially a remotely/unprivileged-triggerable UNBOUNDED leak (DoS). Swept the whole tree with 4 parallel
  read-only agents (net; fs+vfs; drivers+io; mm+proc). Several real leaks found; FIXED the io_uring CQ-overflow
  leak (F118 -- clean, boot-verifiable, in-class). The HIGHEST-severity finding (the remote TCP SYN-flood leak) is
  recorded with its full design below for a DEDICATED careful turn (it has real SMP-synchronization subtleties --
  rushing it would introduce a UAF, worse than the leak).
  FIXED (F118): io_uring CQ-overflow list (kernel/io/io_uring.c) -- TWO sub-bugs, both unprivileged-local: (a)
    UNBOUNDED GROWTH -- io_uring_post_cqe pushes a kmalloc'd node onto overflow_head whenever the CQ ring is full,
    with NO cap; a process that submits ops and never reaps its CQ grows the list without bound (kernel-memory
    DoS); (b) LEAK ON CLOSE -- io_uring_close_file frees the ring but never drains overflow_head (the only freer is
    the drain-on-post in a SUBSEQUENT post_cqe, which never runs after close), so a ring closed with a non-empty
    list leaks every node. FIX: added overflow_count + IO_URING_OVERFLOW_MAX (4096, ~96 KiB/ring) -- past the cap,
    post_cqe drops + bumps the user-visible overflow counter (same as the OOM branch); io_uring_close_file now
    drains the list (kfree each node) before kfree(uring), under overflow_lock (the SQPOLL+worker kthreads are
    joined and the fd is gone, so no poster races). overflow_count is touched only inside post_cqe's cq_lock
    section (cap read + inc/dec under the nested overflow_lock), so no race. Verified by [io_uring_test] (create/
    enter/complete/close) + clean boot.
  FIXED (F119, the headline -- TCP half-open SYN-flood leak, kernel/net/tcp.c -- REMOTE unauth
    UNBOUNDED, ~128 KiB/half-open, the single highest-severity finding of the whole campaign): a TCP_SYN_RCVD child
    (tcp_pcb_alloc + 64 KiB txbuf + 64 KiB rxbuf, published on s_pcb_head) is NEVER freed if the final ACK never
    arrives -- tcp_timer_tick rewinds + resends the SYN-ACK forever with NO retransmit cap / RTO timeout (no
    counter field in struct tcp_pcb), and the ONLY tcp_pcb_free callers are in socket.c (PCBs that own an fd); a
    SYN_RCVD child has sock_file==NULL and no fd, so nothing frees it. Plus tcp_pcb_free of a listener only NULLs
    child->listener (the F-fix for the late-ACK UAF), orphaning completed-but-unaccepted children in accept_queue
    forever. FIX DESIGN (verified, and the SUBTLETIES that make it a careful change, NOT a tight rush): add a
    `uint8_t rexmit` to the pcb; in tcp_timer_tick cap SYN_RCVD-orphan retransmits and reap past the cap. CRITICAL
    SUBTLETIES discovered: (1) tcp_timer_tick walks s_pcb_head UNDER rcu_read_lock, but tcp_pcb_free calls
    call_rcu_expedited -> synchronize_rcu_expedited, which PANICS if called with preempt disabled / under a reader
    section -- so you CANNOT free inside the walk; mark-then-reap-after-rcu_read_unlock is required (or collect via
    a separate link field). (2) tcp_timer_tick (net_tcp_timer_thread) reaping a SYN_RCVD child RACES the rx thread
    (net_rx_thread) completing the handshake (SYN_RCVD->ESTABLISHED + accept_queue push); a reap that does not
    re-check state under a lock (s_pcb_wlock) would free a now-VALID connection (UAF / lost conn). So the reap
    decision + unlink must be atomic with the state transition -- the state machine needs real synchronization.
    HOW F119 FIXED IT (both subtleties handled): added uint8_t rexmit (++ in tcp_timer_tick's SYN_RCVD rto branch,
    timer-thread-only -> no lock) + uint8_t reaped + a transient reap_next link. SUBTLETY (1): the reap runs AFTER
    rcu_read_unlock (a separate s_pcb_wlock pass collects over-cap orphans via reap_next, unlinks them leaving their
    own ->next intact so a concurrent reader is undisturbed), then ONE synchronize_rcu_expedited covers the whole
    batch before the direct tcp_pcb_free_rcu frees -- no synchronize under a reader, and one grace period not N.
    SUBTLETY (2): the establish-vs-reap decision is atomic on s_pcb_wlock -- tcp_recv's SYN_RCVD->ESTABLISHED
    transition now runs under s_pcb_wlock and checks `!reaped && state==SYN_RCVD` (aborts if the reaper claimed it),
    and the reaper sets ->reaped + unlinks under the same lock -- so a child that just established is never reaped
    and a being-reaped child is never queued onto an accept queue (no UAF). accept_q_push stays under the LISTENER's
    lock (separate critical section, no two-lock nesting). VERIFIED by a NEW deterministic selftest tcp_synreap_
    selftest (over-cap SYN_RCVD orphan reaped; owned child kept) + the existing [tcp_orphan]/[tcp_accept] still PASS
    (the establish-serialization did not break the handshake/accept paths) + a clean boot. The cross-thread reap-vs-
    handshake race is closed by reasoning (the s_pcb_wlock establish-or-reap pattern) since TCP is not boot-SMP-
    exercised. SECONDARY (listener-close orphan reaping of completed-but-unaccepted children, + a per-listener
    half-open cap) recorded as a follow-up -- the SYN_RCVD reaper bounds the UNBOUNDED remote DoS (the headline).
  RECORDED (lower-severity leaks, for follow-up): ARP cache no eviction/cap (arp.c -- remote on-link, unbounded,
    ~16 B/entry; add LRU/aging + a cap); AF_UNIX SOCK_DGRAM queue no cap (unix_sock.c unix_sock_sendto -- the lone
    uncapped AF_UNIX queue; add UNIX_DGRAM_QUEUE_MAX like UDP_RX_QUEUE_MAX); task_create_kthread leaks the PML4
    frame on task_mm_alloc OOM (process.c -- error-only, bounded, one frame; mirror task_fork's pmm_buddy_free
    cleanup; task_create_user has the same shape but no live callers); nvme/hda/virtio_input/ahci boot-init
    error-path leaks of already-allocated DMA/MMIO (error-only, one-shot at boot, bounded); nvme idbuf/smoke-test
    page leaked even on success (one page/boot).
  AUDITED CLEAN: net skb/sock/pcb (freed on every path; the leak is the unbounded half-open table, not the skbs);
    fs+vfs (vfs_alloc_file/dentry/eventfd/timerfd/signalfd/pipe/epoll/ext2-scratch all freed on all paths incl.
    error; dcache + irtree caches have eviction caps + shrinkers; fd_install contract honored); drivers (pty/evdev/
    drm per-open ctx freed on close + error; io_uring instance/fixed_files/work-nodes freed on all paths EXCEPT the
    overflow list = F118); mm+proc (task_fork/sys_thread/exec/elf error paths free pml4+mm+files; pid_alloc 1:1
    with pid_free incl. fork allocating the pid LAST; futex waiters stack-resident + always unlinked; VMA/shmem/
    pcache/page-fault refs balanced; no unprivileged pid/frame exhaustion path). All refcounts u32 -- no wrap.
  SCAN #6 SUBSTANTIALLY COMPLETE: io_uring overflow leak FIXED (F118), TCP half-open SYN-flood leak FIXED (F119 --
    the headline remote DoS). Residual lower-severity leaks recorded above (ARP cache cap, AF_UNIX dgram cap,
    kthread PML4-on-OOM, boot-init error-path leaks, the TCP listener-close orphan reaping + per-listener cap).
    Next: BUG-TYPE SCAN #7 (NULL-deref / unchecked-allocation) -- or pick off a residual leak if higher-value.

- 2026-06-27 BUG-TYPE SCAN #7 (NULL-deref / unchecked-allocation): an alloc/lookup result deref'd without a NULL/
  not-found check -> a kernel #PF panic (DoS). Swept the whole tree with 4 parallel read-only agents (net; fs+vfs;
  drivers+io; mm+proc). The ONLY unprivileged/remote-reachable NULL-deref was the headline -- FIXED (F120); the
  rest are OOM-only-kernel-path or dead code, recorded for follow-up.
  FIXED (F120): virtio_net_rx_poll NULL skb -> eth_recv(NULL) #PF (kernel/net/virtio_net.c + net.c). On skb_alloc
    OOM the function set *skb_out=NULL but still returned 1 ("packet ready"), and net_rx_thread fed that NULL
    straight into eth_recv, which derefs skb->len -> kernel #PF; OOM-triggered but on the universal RX dispatch
    path (a remote peer driving RX load under memory pressure makes every received frame hit it). FIX: restructured
    the poll into a loop that drops an OOM-failed (or bogus-desc-id) packet -- recycling its descriptor so the RX
    ring does not leak -- and CONTINUES to the next used entry, so it only ever returns 1 with a VALID skb (the
    "return 1 => valid skb" contract is now sound, so a future caller cannot reintroduce the bug). Verified by the
    DHCP lease (the OFFER/ACK are received through the new loop -> ifconfig IP 10.0.2.15) + code-proof (the
    `if (!skb) continue` drop path).
  RECORDED (OOM-only / boot-init / dead code -- NOT unprivileged-reachable, lower priority):
    vmm_map_mmio 0-on-exhaustion (F116) deref'd without a 0-check at boot-init MMIO sites: ioapic.c:127 s_base,
      nvme.c:682 s_regs + :622 s_msix_table, ahci.c:1023 s_hba + :1102 s_msix_entry, virtio_net.c:712 msix_table --
      the window never exhausts today (1 TiB), so latent; fix = `if (!ptr) return 0/-ENODEV;` after each (mirror
      hda.c:391 / virtio_input.c:331 / virtio_gpu.c:472 which already do).
    task_create_kthread (process.c:292) passes vmm_alloc_pml4() into task_mm_alloc with NO == PMM_INVALID_ADDR
      check -> on OOM pml4_phys = UINT64_MAX -> context_switch loads CR3=UINT64_MAX -> #GP/triple-fault (OOM, kernel
      kthread create only, privileged). Fix: check `p == PMM_INVALID_ADDR` before task_mm_alloc (mirror task_fork).
    task_create_user (process.c:326/327) unchecked vmm_alloc_pml4 + mm_create -> NULL/wild use (OOM, DEAD code --
      no live callers). fat32_open (fat32.c:239-249) derefs f/fd without NULL checks (OOM, DEAD -- fat32 unwired,
      no VFS hookup).
    WRONG SENTINEL (not NULL-deref but a sibling wild-pointer bug, boot-init): several DMA-buffer callers test
      `if (!phys)` for pmm_buddy_alloc OOM, but it returns PMM_INVALID_ADDR=UINT64_MAX (not 0), so the check misses
      and `phys + HHDM_OFFSET` wraps to a wild non-NULL pointer -> a wild write on OOM (hda.c:412/428/461/468/479,
      virtio_input.c:362-363, ahci/ac97/virtio_gpu DMA sites). The correct idiom (== PMM_INVALID_ADDR) is already
      used at io_uring.c:231 / drm.c:526 / nvme.c:722. Candidate for a sentinel-correctness sweep.
  AUDITED CLEAN: net (every skb_alloc/kmalloc/pcb_find/ns_find/udp_table_find NULL-checked or has the miss branch;
    udp/tcp skb_put are safe-by-construction sized-to-fit), fs+vfs (vfs_alloc_file/dentry/eventfd/timerfd/signalfd/
    pipe/proc + epoll/poll/select all check-then-rollback; ext2/dcache lookups have the miss branch; the
    check-then-rollback ext2_open_by_ino is the pattern fat32 fails to follow), drivers (pty/evdev/drm/io_uring/tty
    per-open allocs all checked; drm find_dumb/find_fb/find_prop callers all check; io_uring pmm == PMM_INVALID_ADDR
    + sqe_fdget checked), mm+proc (elf malformed-ELF bounds-gated; page-fault/demand-page mm + g_current guarded;
    every pmm_buddy_alloc checked == PMM_INVALID_ADDR; shmem/mm/signal/futex/sched lookups have the miss branch;
    kstack_alloc PANICS on OOM so its unchecked callers are clean-by-design). The subsystem is overwhelmingly
    check-then-error.
  SCAN #7 SUBSTANTIALLY COMPLETE: the only unprivileged/remote NULL-deref (virtio_net RX) FIXED (F120). Residuals
    (the boot-init vmm_map_mmio 0-checks + the PMM_INVALID_ADDR wrong-sentinel sweep + task_create_kthread) are
    recorded. Next: BUG-TYPE SCAN #8-as-a-type.

- 2026-06-27 BUG-TYPE SCAN #8 (wrong-sentinel / unchecked-or-wrong-checked error return): a call whose failure
  sentinel is tested with the WRONG predicate (or not at all), so the error path never fires and a poison value is
  used as if valid. Headline (queued from SCAN #7): pmm_buddy_alloc / vmm_alloc_pml4 return PMM_INVALID_ADDR ==
  UINT64_MAX on OOM (NOT 0), but many DMA-buffer callers tested `if (!phys)` / `== 0`, which NEVER catches OOM ->
  `phys + HHDM_OFFSET` wraps to a wild kernel pointer (~HHDM_OFFSET-1) -> a wild kernel memset / DMA program on OOM.
  Swept the whole tree; mm/** is uniformly clean (== PMM_INVALID_ADDR). The cluster is the driver DMA-init paths.
  FIXED (F121, ONE commit -- same root): extracted PMM_ALLOC_OK(p) := ((p) != PMM_INVALID_ADDR) into pmm.h and gated
    all 20 pmm_buddy_alloc results across 5 drivers through it, so a `!phys`/`==0` mis-check can never recur:
      ahci.c port_init -- had NO checks at all: s_cmdlist_phys/s_fis_phys (239/240) + each s_ctbl_phys[i] (249) were
        memset through +HHDM_OFFSET AND programmed into the device command-header DMA registers with the raw result;
        on OOM that is a wild memset + a wild DMA base handed to the controller. Added early `return;` guards (241,
        250) so OOM aborts before any wild memset / DMA program.
      hda.c (412/428/461/468/479): corb/rirb/bdl/per-buffer/fifo -- 5 sites converted `if (!x) return 0;` ->
        `if (!PMM_ALLOC_OK(x)) return 0;`.
      virtio_gpu.c (226/231/236/515/520/771): the 3 virtq rings (desc/avail/used) + s_cmd/s_cursor + the framebuffer
        alloc -- 6 sites converted.
      virtio_input.c (362-363): the 4-way disjunction (eventq desc/avail/used + s_evbuf) converted in one guard.
      ac97.c (312/319): the BDL alloc had a uint32 TRUNCATION bug -- `s_bdl_phys = (uint32_t)pmm_buddy_alloc(0)`
        truncated UINT64_MAX to 0xFFFFFFFF (non-zero), so even the `!s_bdl_phys` check missed; restructured to test
        the full-width phys_addr_t before the u32 narrowing; the DMA-buffer loop site also converted. (ac97 is DEAD
        -- no initcall, /dev/dsp routes to hda_write -- so latent, but fixed for uniformity.)
    Verified: a clean boot with ahci/hda/virtio-gpu/virtio-input all initialising proves the non-OOM path is
    unbroken (a valid frame passes PMM_ALLOC_OK exactly as `!phys` passed before); the OOM branch is code-proven (a
    UINT64_MAX result now correctly fails). mm/** already used == PMM_INVALID_ADDR everywhere, so no fix there.
  RECORDED (out-of-type, found by the SCAN #8 sweep -- HIGHER severity than the in-type residuals, queued for
  dedicated turns):
    FIXED (F122) -- spawn SPAWN_ATTR_UNVEIL raw user-pointer deref (a SCAN #3 straggler): sys_spawn's
      SPAWN_ATTR_UNVEIL handling validated the user unveil-rule array with `_access_ok` then did a RAW memcpy from
      the user pointer (syscall.c ~1449) with NO copy_from_user / prefault, so a user passing a pointer to an
      UNMAPPED-but-in-range page faulted IN the kernel -> an unprivileged kernel #PF panic (DoS). FIX: replaced
      `_access_ok` + raw memcpy with copy_from_user (handles the fault -> -EFAULT -> bad_attr, never panics; mirrors
      the spawn_attr copy 50 lines up that was already fixed this way), + kfree(ue) on the new copy-failure path,
      + a SPAWN_UNVEIL_MAX=256 cap bounding the attacker-sized kmalloc. SIBLING bug closed in the same flow: each
      copied ue[i].path is a user-controlled char[256] with no guaranteed NUL, and unveil_add's s_strlen is
      unbounded -> an OOB heap read past the n*257-byte buffer; force-NUL-terminate each entry before unveil_add.
      Verified: copy_from_user's bad-pointer rejection is already covered by copy_user/access_ok selftests, and the
      SUCCESS path is boot-exercised (net.svc `unveil=/etc rwc` spawns /bin/net through this code every boot ->
      DHCP lease proves it). Note: bad_attr fails open (child gets no unveil) -- pre-existing design, not a privesc,
      left unchanged.
    ahci_submit_sg omits the AHCI_READ_POISON sentinel re-check that the sibling AHCI read paths perform after the
      DMA completes, so a poisoned (failed-read) sector can be returned as valid data on an unprivileged file read
      (stale-data-as-valid). Medium; queue for a TOCTOU/sentinel follow-up.
  RECORDED (in-type, lower severity -- the remaining wrong-sentinel / ignored-return residuals):
    socket_bind: port 0 is BOTH a valid "any port" request AND the empty-slot sentinel in the bind table, so the
      0-as-sentinel overload can misclassify; needs an explicit occupied flag rather than port==0.
    Ignored OOM error returns (the caller drops a negative/sentinel return and proceeds): process.c fd_table_init /
      pid_alloc, elf.c:308 mm_vma_add -- all ignore failure and continue (OOM-only, kernel path). Plus the boot-init
      vmm_map_mmio 0-on-exhaustion sites carried over from SCAN #7 (ioapic/nvme/ahci/virtio_net MSI-X maps).
    io_uring DRM_COMMIT path leaks an fdput (an unbalanced get/put on the error branch).
  SCAN #8 COMPLETE: the wild-write-on-OOM DMA cluster fixed at root with a reusable PMM_ALLOC_OK primitive (F121,
    docs/PRIMITIVES.md), and the highest-severity out-of-type finding -- the spawn SPAWN_ATTR_UNVEIL unprivileged
    #PF panic + its sibling OOB heap read -- fixed (F122). Remaining recorded residuals (ahci_submit_sg READ_POISON
    stale-data sentinel gap; socket_bind port-0 overload; fd_table_init/pid_alloc/mm_vma_add ignored OOM returns;
    io_uring DRM_COMMIT fdput leak) carried forward. Next: SCAN #9-as-a-type (int-truncation/sign-confusion -> OOB,
    OR TOCTOU -- the ahci_submit_sg READ_POISON gap is a strong TOCTOU/sentinel candidate -- OR a recorded residual).

- 2026-06-27 BUG-TYPE SCAN #9 (integer-truncation / sign-confusion -> OOB / wrong-bound): a 64/32-bit value narrowed
  to a smaller type before a bound check, or a signed value used unsigned, so a huge value looks small or a negative
  one becomes huge. Distinct from the integer-OVERFLOW a*b/a+b class (checked.h, F46-F54) and the wrong-sentinel
  SCAN #8. Swept the whole tree with 4 parallel read-only agents (net; fs+vfs; drivers+io; mm+proc+syscall). The
  ONLY unprivileged/remote-reachable instance was the headline -- FIXED (F123); every other narrowing is bounded
  downstream (recorded LOW/latent below).
  FIXED (F123): UDP send-length u32->u16 truncation + 16-bit wrap -> unprivileged kernel NULL-deref panic
    (kernel/net/udp.c + socket.c + ipv4.c). send/sendto on a SOCK_DGRAM socket with len in [65528,65535] narrowed
    the user u32 to u16 with no size check; udp_send_ex's `(uint16_t)(UDP_HDR_LEN + len)` then wrapped (e.g.
    65535 -> 7), under-sizing the skb so the first skb_put returned NULL and the header write #PF-panicked. FIX:
    widened udp_send/udp_send_ex to uint32_t len + `if (len > UDP_MAX_PAYLOAD(65507)) return -EMSGSIZE` at the
    single UDP chokepoint; a universal L3 backstop `if (skb->len > 0xFFFF - IPV4_HDR_LEN) return -EMSGSIZE` at
    ipv4_send_ex (guards the IP total_len wrap for ALL L4 protocols); socket callers pass full-width len and surface
    -EMSGSIZE; added EMSGSIZE(90) to errno.h. New udp_send_size_selftest (pure reject-path unit test) + DHCP proves
    the valid path. mm/** uses == PMM_INVALID_ADDR / u64 bounds everywhere; nothing to fix there.
  RECORDED (LOW / latent -- narrowing present but bounded downstream, NOT unprivileged OOB):
    fs: ext2_vfs_pread `(uint32_t)offset` (ext2.c:1330) -- bounded by the u32 i_size, no OOB (wrong-offset only at
      >4 GiB, unsupported). dir_remove_entry `(uint8_t)name_len` match vs full-width kmemeq (ext2.c:2173) -- a
      clamp asymmetry vs dir_lookup, but every caller resolves via dir_lookup first (whose clamp rejects >255-byte
      names), so unreachable; defense-in-depth gap. The `(uint32_t)(len-total)` read/write loop casts
      (ext2.c:1226/1342/1427) -- clamped to remain_blk/remain_file per iteration, no OOB (stuck-loop only at an
      exact 2^32 multiple). fat32.c -- DEAD (unwired), same patterns, unreachable.
    mm/syscall: shmem/mmap page-count u64->u32 narrowings (syscall.c:2476/2679/5569, mm.c:371, vmm.c:1003/859) --
      each saved by shmem_get_page's `pg_idx >= npages` hard bound (alias/SIGSEGV at >16 TiB VA, never kernel OOB).
    drivers: NONE -- every untrusted size/index uses a checked.h/domain helper, a power-of-two ring mask, or u64
      math (ahci xfer_bytes_ok, nvme mul_within_u32 + nvme_cid_valid, virtio_gpu vgpu_fb_bytes + scanout cap, drm
      drm_dumb_size/drm_atomic_count_ok u64, virtio_input/evdev/pty/tty/hda ring masks, io_uring kernel-trusted
      masks + user_buf_check). The ac97 `(uint32_t)phys` is the F121-touched OOM line + a genuine 32-bit-DMA HW
      constraint (needs a <4 GiB DMA zone, not a bound), kernel-internal -- excluded.
    net (sibling, same-class, now backstopped): eth.c:42 `(uint16_t)skb->len` and ipv4.c:118/126 TX `(uint16_t)`
      total_len -- under-send/IP-total wrap, only reachable with skb->len > 65515, which no L4 caller produces after
      the UDP cap, AND now caught by the F123 ipv4_send_ex backstop.
  AUDITED CLEAN: net (RX paths clamp packet length fields to skb->len / ETH_MAX_FRAME; tcp dlen/hlen validated;
    virtio_net device ids index_ok'd; arp/icmp fixed-size or skb->len-consistent; unix_sock backlog clamped), fs+vfs
    (all on-disk ext2 fields routed through the checked.h + ext2 domain validators, mount-gated; dcache/pipe/proc/
    virtfs bounded), drivers (see above -- uniformly bounded), mm+proc+syscall (copy_*_user form ranges in u64 via
    ckd_add_u64; mmap_round_len overflow-safe; ELF u64 bounds-gated; fd numbers u64-vs-u32-cap; signal/argv/iovec/
    cmsg counts range-checked). The kernel is overwhelmingly width-correct or downstream-bounded.
  SCAN #9 SUBSTANTIALLY COMPLETE: the only unprivileged/remote truncation OOB (UDP send wrap) FIXED (F123) with a
    new unit test. Residuals are all bounded-downstream LOW. Next: SCAN #10-as-a-type (TOCTOU/race-window -- the
    ahci_submit_sg READ_POISON re-check gap is a strong candidate -- OR uninitialized-memory/info-leak to userspace,
    OR a recorded residual: socket_bind port-0 overload, the ignored-OOM-return cluster, io_uring DRM_COMMIT fdput).

- 2026-06-27 BUG-TYPE SCAN #10 (uninitialized-memory / kernel-info-leak to userspace): kernel stack/heap bytes that
  were never initialized are copied out to an unprivileged user (struct padding, unset fields, a buffer's unwritten
  tail), disclosing kernel pointers / other tasks' data. Swept the whole tree with 4 parallel read-only agents (net
  [sockaddr out-params, recvmsg]; fs+vfs [stat/dirent/proc/dev]; drivers+io [every ioctl GET, signalfd, io_uring
  CQE]; mm+proc+syscall [uname/sysinfo/rusage/siginfo + every copy_to_user]). Found TWO unprivileged HIGH leaks,
  both the SAME shape (copy-out without zeroing), both in syscall.c -- FIXED as one cluster (F124/F125).
  FIXED (F124): sys_readdir kernel-HEAP leak (kernel/syscall/syscall.c). `kbuf = kmalloc(max_entries *
    sizeof(ext2_entry_t))` (syscall.c:1776) is NOT zeroed; ext2_entry_t (name[256]+u32+u32+u8, NOT packed -> 268 B
    with 3 tail pad bytes) is filled by ext2_readdir/dev_readdir/proc_readdir/the inline /proc+/dev injection with
    only name[0..nlen]+NUL + 3 fields, leaving the name tail + 3 pad bytes stale; copy_to_user ships the full struct
    per entry -> up to ~64 KiB of kmalloc heap to any unprivileged process on any readable directory. Flagged by TWO
    agents independently. FIX: __builtin_memset(kbuf, 0, ...) at the single copy-out owner before any filler runs
    (covers all fillers, no drift).
  FIXED (F125): sys_uname kernel-STACK leak (kernel/syscall/syscall.c:4090). `utsname_t u;` (6 x char[65] = 390 B)
    is an unzeroed stack local; strncpy_k copies only strlen+1 per field (no zero-pad like POSIX strncpy), so the
    full-struct copy_to_user leaks ~349 B of stale stack to any unprivileged uname() caller. FIX:
    __builtin_memset(&u, 0, sizeof u) before the fills (the file's sys_stat/sys_fstat zero-then-fill convention).
  Both verified by the new out_struct_zerofill_selftest (pins sizeof(ext2_entry_t)==268 / sizeof(utsname_t)==390 so
    a future field-add that adds uncovered padding fails the build-boot, and asserts the zero-before-fill leaves the
    tail + padding zero) + boot (svcmgr opendir/readdir of /etc/services exercises sys_readdir every boot).
  RECORDED (not fixed -- dead/unreachable): proc_stat (fs/proc.c:538) fills a partial `struct stat` without zeroing,
    but has NO caller (the live /proc stat path is sys_stat's is_virtual branch, which memsets) -- a latent footgun
    if ever wired up. fat32 readdir/stat fillers -- DEAD (unwired, no VFS hookup).
  RECORDED (informational -- NOT a kernel leak, but worth defensive zeroing): socket_recvfrom (socket.c:625) and
    socket_accept (socket.c:378) leave sin_zero[8] of the sockaddr_in out-param unwritten, BUT the kernel writes the
    address fields DIRECTLY into the validated user pointer (never stages a kernel struct + copies it), so the 8
    unwritten bytes keep the USER's own prior memory -- discloses nothing from the kernel. Would become a real 8-byte
    stack leak if ever refactored to stage-in-kernel-then-copy_to_user; zero-the-struct defensively if so. (Note:
    this kernel has NO getsockname/getpeername/getsockopt syscall, eliminating the usual sockaddr leak vectors.)
  AUDITED CLEAN: net (recv/recvfrom/recvmsg copy exactly the byte count read; SCM_RIGHTS cmsghdr no padding + all
    fields set; sockets/PCBs memset at alloc), fs+vfs (sys_stat/sys_fstat __builtin_memset their out struct incl.
    _pad0; proc content generators set *out_size to the exact written length; pipe/dev reads return only produced
    bytes; urandom fills the full len), drivers+io (drm GET handlers copy_from_user the whole arg IN then write it
    back OUT so untouched padding round-trips the USER's bytes; signalfd_siginfo `{0}` zeros its 46-byte __pad;
    io_uring CQE packed 16 B all-fields-set + ring memset-0; evdev device memset-0 + names always NUL-terminated;
    termios/winsize no padding; eventfd/timerfd copy exactly 8 defined bytes), mm+proc+syscall (getrusage/times/
    gettimeofday/sigaction/epoll_event/poll/select/itimerspec all zero-init or no-padding-all-fields-set; task_t
    memset at alloc; every user-visible mm frame zeroed before content). The kernel overwhelmingly zeroes-then-fills.
  SCAN #10 SUBSTANTIALLY COMPLETE: both unprivileged info-leaks (readdir heap, uname stack) FIXED (F124/F125) with a
    new unit test; the "zero-then-fill out-param" rule recorded in docs/PRIMITIVES.md. Next: SCAN #11-as-a-type
    (TOCTOU/race-window -- the ahci_submit_sg READ_POISON re-check gap -- OR a recorded residual: socket_bind port-0
    overload, the ignored-OOM-return cluster [process.c fd_table_init/pid_alloc, elf.c mm_vma_add], io_uring
    DRM_COMMIT fdput leak).

- 2026-06-27 BUG-TYPE SCAN #11 (TOCTOU / double-fetch / check-then-use race-window): the time-of-check-to-time-of-use
  GAP (distinct from SCAN #4's unsynchronized field access). Swept the whole tree with 4 parallel agents (drivers+io
  [io_uring SQE double-fetch]; mm+proc+syscall [fd-table lookup-then-use, futex, copy_from_user re-read]; net [pcb
  lookup-then-use, revalidate-after-wake]; fs+vfs [path TOCTOU, dcache/bcache lookup-then-use]). The prime surface --
  io_uring SQE -- is CLEAN (dispatch_exec snapshots the whole 64-byte SQE into a kernel local once; all reads use the
  snapshot; the SQ index uses a kernel-trusted mask). Highest-value finding FIXED (F126); others recorded.
  FIXED (F126): the fork/spawn/thread fd-table lock-free vfs_dup use-after-free (3 sites: process.c task_fork,
    syscall.c sys_thread, elf.c resolve_stdio). Copying a SHARED fd table did `vfs_dup(src->fd_table[i])` with no
    lock, so a sibling thread's sys_close() could free the file in the window -> vfs_dup loads f->refcount on freed
    memory (UAF). vfs.h:82-84 says lock-free observers MUST use vfs_tryget, not vfs_dup. FIX: extracted
    fd_table_clone(dst, src) that vfs_dup's every slot UNDER src->lock (a non-NULL slot is then guaranteed live, its
    own ref keeps refcount>=1); task_fork/sys_thread call it (+ now handle the OOM return, fixing a latent
    fd_table_init-ignored NULL-deref); resolve_stdio (which also DOUBLE-FETCHED the slot) reads it once under the
    lock. New fd_table_clone_selftest + heavy boot-exercise (every spawn clones through it; login + DHCP prove it).
  RECORDED (real, queued for follow-up turns):
    FIXED (F127): sys_spawn stdio double-fetch (syscall.c:1471) -- unprivileged kernel #PF panic. After
      copy_from_user'ing stdio[3] into a kernel local (1375, EFAULT on a bad ptr), the job-control branch RE-READ
      `((const int*)stdio_ptr)[0]` raw from user memory; a sibling thread munmap's the page during the wide
      elf_exec_from_ext2 window -> the raw read #PF-panics the kernel (no fault-fixup). FIX: replaced with
      `if (stdio[0] == -1)` (the validated snapshot -- equivalent: -1 default covers the NULL case). Verified:
      code-proof + boot (svcmgr/login spawn through this path; login renders, ctty granted).
    TCP lost-wakeup (tcp.c, 4 sites: tcp_recv_data:865, tcp_send:817, socket_accept:369, socket_connect:449) -- MED
      hang/DoS, VERIFIED REAL but DEFERRED to a dedicated turn (the correct fix is a refactor, not a one-liner).
      Each consumer checks its wait condition, then publishes pcb->waiter (plain store), then sched_sleep WITHOUT
      re-checking; pcb_wake gates the wake on a plain-read pcb->waiter, so a wake landing in the check->publish gap is
      lost -> recv/send/accept/connect hangs with the condition already true. The sched_sleep wake_pending interlock
      CANNOT help: the wake is dropped at pcb_wake BEFORE sched_wake is ever called. A minimal publish-then-recheck is
      INSUFFICIENT: pcb->waiter has no common lock, so consumer `store waiter; load cond` vs producer `store cond;
      load waiter` is a store-load (Dekker) pattern needing explicit seq_cst fences on BOTH sides on x86 TSO -- subtle
      and error-prone. The ROOT fix is to converge all 4 onto the lock-serialized wait_queue + WAIT_EVENT_HOOK
      (wait.h), which is correct-by-construction (rq_lock + the queue serialize publish-vs-wake, no hand-rolled
      fences) and also fixes the pcb->waiter single-waiter limit -- but that refactors the core net blocking paths
      (pcb struct + pcb_wake + 4 sites across tcp.c/socket.c) whose blocking-with-contention is NOT boot-exercised, so
      it needs a DEDICATED turn WITH a real blocking-wait test harness (a regression would HANG TCP, worse than the
      rare race). NEXT after the path-TOCTOU, or whenever a TCP blocking-wait test can be built.
    FIXED (F128): io_uring IORING_OP_DRM_COMMIT fixed-file refcount LEAK (io_uring.c:1044) -- the handler took a ref
      via sqe_fdget but never uring_fdput'd it (unlike READ/WRITE), so each DRM_COMMIT leaks a file ref; a FIXED
      file then never reaches refcount 0 and is never freed -> unbounded kernel-memory leak under a commit loop (an
      unprivileged DoS). FIX: added uring_fdput(f) on the success path (DRM_COMMIT was the lone unbalanced sqe_fdget
      site; SEND/RECV/CONNECT/ACCEPT route through sys_* with their own fdget/fdput). Verified: code-proof (get/put
      now balanced like READ/WRITE) + [io_uring_test] boot-exercises the dispatch/fdput path.
    Path TOCTOU on unlink/rename/mkdir/open+O_CREAT (syscall.c + ext2.c) -- HIGH impact, race-gated, INVESTIGATED in
      depth, DEFERRED to a dedicated turn (correct fix is subtle, NOT a half-landable change). The syscall-level perm
      check resolves+validates the parent, then the ext2 op RE-resolves the path from scratch with no perm re-check;
      a concurrent rename of an intermediate component swaps the target -> a destructive op in a directory whose write
      perm was never validated. PLANNED FIX: move the WRITE|EXEC perm check INTO each ext2 op, on the parent inode the
      op itself resolves, so check + use are on the SAME resolution (pass cred into the op; extract a shared
      ext2_dir_write_ok(parent_ino, cred) helper). KEY FINDINGS from the investigation (so the dedicated turn does not
      re-derive them): (1) the 4 ops are called ONLY from the 4 syscall sites (clean signature change); errno.h +
      vfs_check_perm are available in ext2.c. (2) ext2_unlink/ext2_mkdir/ext2_rename resolve their parent(s) once and
      operate directly -> a perm check on that resolution is clean; ext2_rename resolves BOTH parents (so it also
      fixes the dst-parent gap below). (3) ext2_create is the HARD one: it delegates the actual creation to
      ext2_write_file(path), which RE-resolves the path AND is shared with ext2_truncate (different perm semantics) --
      so the check cannot just gate ext2_create's own parent_ino; it must gate ext2_write_file's create site without
      affecting truncate. (4) errno preservation: keep the syscall checks (EACCES + early reject) and make the
      op-internal check authoritative (least-invasive A2), OR plumb int* err_out through the ops (full move). (5) the
      mutating ops are weakly boot-exercised -> the turn MUST add a create/unlink/mkdir/rename selftest (as root) to
      verify normal access is unbroken. ALSO FIX IN THE SAME CHANGE: sys_rename checks only the SRC parent's write
      perm, NOT the dst parent -- a real permission gap (rename into a no-write dir currently succeeds); moving the
      check into ext2_rename (which resolves both parents) closes it. (Related out-of-class: fs_lookup doesn't check
      final-file r/w perm for ext2 -- a separate permission-model gap.) Also LOW: mmap re-resolves f->path instead of
      f->ino (wrong-size); socket_bind RCU-frees a pcb a blocked sibling holds by raw pointer (latent, hangs not UAFs).
  AUDITED CLEAN: io_uring (SQE snapshot in dispatch_exec; sqe_fdget/uring_fdget hold the lock / RCU+tryget across the
    use; REGISTER_FILES single-fetch under lock), drm/evdev/pty/tty ioctls (copy_from_user the arg into a kernel
    local, use only that), fdget (RCU + vfs_tryget -- the canonical lookup-then-use, refcount-pinned), poll/mmap/
    sigaction/setgroups (single copy_from_user into a kernel local), futex (enqueue-then-recheck under the bucket
    lock, compare-vs-register-val), net (tcp/udp/unix lookups stay under rcu_read_lock or pin via tryget through the
    use; UDP recv re-checks head under the lock after wake; unix peer is RCU+refcount; F119 establish-vs-reap under
    s_pcb_wlock), fs (dcache_lookup returns a refcounted handle held across the use; bcache_get pins; proc uses
    RCU-snapshot+vfs_tryget; existing-file open/exec resolve-once-to-inode-number; copy_path_from_user single-fetch).
  SCAN #11 STATUS: FIXED -- the fd-table lookup-then-use UAF (F126, new mechanism + unit test), the sys_spawn stdio
    double-fetch (F127), and the io_uring DRM_COMMIT fdput leak (F128); io_uring (the prime double-fetch surface)
    verified clean. TWO recorded items remain, BOTH needing a dedicated turn with a TEST HARNESS (their correct fixes
    are subtle refactors of weakly-boot-exercised core paths -- deferred per real-proof, design notes captured above):
    (1) the TCP blocking-wait lost-wakeup (4 sites -- wait_queue/WAIT_EVENT_HOOK convergence + a blocking-wait test),
    and (2) the HIGH path-TOCTOU on the mutating fs ops (move-perm-check-into-the-op + the ext2_create/ext2_write_file
    wrinkle + the rename dst-parent gap + a create/unlink/mkdir/rename selftest). Next: advance to SCAN #12-as-a-type
    (use-after-free distinct from double-free, OR lock-ordering/deadlock, OR uninitialized-padding round 2), OR clear a
    smaller clean residual (socket_bind port-0 overload, the ignored-OOM-return cluster, ahci_submit_sg READ_POISON),
    OR take one of the two deferred dedicated-turn items when a test harness can be built.
