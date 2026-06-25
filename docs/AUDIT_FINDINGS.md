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
`txbuf_used` wedges the sender (the `while txbuf_used >= TCP_TXBUF_SIZE` spin
never clears) or underflows. Fix: a per-PCB lock around the send/ack/retransmit
critical sections. STILL OPEN -- needs a dedicated iteration WITH a TCP-loopback
self-test (boot drives no TCP data, so a locking change is NOT verifiable by
boot alone; a lock-ordering mistake would deadlock only under live concurrent
TCP load).

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
  (`kernel/proc/futex.c:77` futex_wait) -> OPEN. The authoritative post-enqueue
  compare is `cur = *(volatile uint32_t*)uaddr;` done UNDER b->lock; a concurrent
  munmap/mprotect(PROT_NONE) of that page (same address space, another CPU,
  unserialized vs the futex bucket) faults the read -> the page-fault handler
  takes mm->vma_lock and may alloc/sleep while a raw spinlock is held
  (fault-under-spinlock + lock-nest -> panic/DoS, attacker-controlled). The
  "stays mapped, cannot fault" comment is false (no swap != no munmap). Fix:
  do the compare with a fault-safe read (copy_from_user / get_user) OUTSIDE
  b->lock, keeping enqueue-before-recheck for the lost-wake fence; on -EFAULT
  unlink + return -EFAULT (the timeout path already implements the unlink).
  Verify: code-proof + a boot-selftest that FUTEX_WAITs on an unmapped addr and
  asserts -EFAULT instead of panic. Confidence MED-HIGH (race-class). Reachable
  via every pthread mutex. Good NEXT item (clean, high-value).

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
  (`kernel/drivers/storage/nvme.c:484` nvme_irq_handler) -> OPEN. `cqe.cid`
  (uint16, device-written into the completion queue) indexes the 64-entry
  q->req[] with NO bounds check -> OOB write of req->status/->done + a
  wait_queue_wake_all on a fabricated wait_queue_t (walks/writes a wake-list
  through garbage pointers). Exact F22 class for NVMe (the submit side IS bounded
  by a 64-bit bitmap; only the device-echoed completion CID is trusted). AHCI is
  unaffected (slots derived via ctz of the hardware mask into a 32-entry array).
  Fix: pure nvme_cid_valid(cid)=cid<NVME_IOQ_DEPTH, `continue` on a bad cid (the
  CQE is already consumed). Deterministic selftest (verbatim F22 shape).
  Confidence HIGH. Threat model: malicious/buggy NVMe controller or CQ-page DMA
  corruption (the repo already treats this device-echo-index class as a real bug
  worth a guard, per F22).
