# SESSION 2026-06-27 -- bughunt campaign (SCANs #13-#20, F135-F158)

Autonomous security-audit-and-fix loop on branch `bughunt`. Each turn: state a
one-line suspicion, verify against the real code (no assumptions), fix at the
root (no stopgaps), verify from every angle, build (`NO_QEMU=1 SELFTESTS=1` then
shipping), boot-verify, add a deterministic selftest where one fits, document,
commit, push. 24 fixes landed; the high-value memory-safety space is now
substantially mined out (the last two scans, #18 double-fetch and #20
signedness, came back clean for their classes).

## Method

Bug-TYPE scans: each scan targets ONE bug class across the whole codebase,
usually via a 3-agent parallel sweep (one per subsystem cluster), then every
candidate is verified by hand before any fix. Findings that fall outside the
scan's class are recorded and fixed on a later turn. The kernel's safe
primitives (`checked.h`: `in_range_u64`/`index_ok`/`ckd_*`/`mul_within_u32`;
`copy_from_user`/`copy_to_user`/`_access_ok`/`user_buf_check`; the tcp ring
helpers; RCU + per-object locks) are the single sources of truth the fixes
reuse rather than re-deriving checks inline.

## Per-scan results

- **SCAN #13 -- lock-ordering / deadlock.** F135: the flaky ext2_perm_op boot
  hang was a per-inode seqlock held across BLOCKING AHCI I/O (write side odd
  while `dir_add_entry`/`inode_writeback` slept in `ahci_write`). Root-caused,
  fixed, 40/40 boot-verified.

- **SCAN #14 -- integer overflow (multiply/add wrap).** F136 DRM SET_CURSOR
  `(uint64_t)w*h*4` u64 wrap defeats the backing-size bound; F137 nvme
  `1u << lbads` UB / under-counted DMA size; F138 hda u8-loop infinite codec
  scan. Routed each through a checked primitive.

- **SCAN #15 -- error-path resource leaks.** F139 vmm_get_user_pages pin leak;
  F140 task-create pml4/mm leak + NULL-deref; F141 tcp established-child-PCB
  leak on a full backlog (remotely triggerable); F142 virtio_gpu/drm init-error
  cluster (3 leaks); F143 vmm isr14 unchecked map leak + re-fault loop; F144
  sys_mmap shmem-attach race leak. Scan fully closed.

- **SCAN #16 -- ignored error returns.** F145 fd_table_init NULL-deref panic;
  F146 io_uring lost-CQE-on-ENOMEM hang (SQE:CQE invariant); F147 ext2_rename
  ignored dir_remove_entry/dir_set_dotdot -> fs corruption; F148 ignored
  vmm_page_map / mm_vma_add cluster (4 sites in mm + elf).

- **SCAN #17 -- OOB / over-read.** F149 tcp tx-ring 64 KiB-wrap heap over-read
  (remote kernel-heap info-leak). Residual cleanup of the #16/#17 backlog:
  F150 ext2_vfs_pread missing-EOF-clamp (info-exposure) + selftest; F151
  sys_readlink/sys_times false-success on a faulted copy_to_user (-EFAULT);
  F152 hda verb_send returned stale RIRB data on a poll timeout; F153
  vmm_map_mmio ignored vmm_page_map -> half-mapped kernel BAR (+ the essential
  callers ioapic/ahci/nvme now handle the NULL).

- **SCAN #18 -- double-fetch / TOCTOU.** SWEPT CLEAN (the kernel snapshots each
  io_uring SQE once, copies msghdr/iovec/drm-atomic once into kernel locals,
  futex re-reads a kernel copy). The sweep surfaced one out-of-class bug:
  F154 sys_unveil raw unvalidated user-pointer deref (unprivileged kernel #PF
  DoS + kernel-memory read) -> routed through copy_from_user. Also F155: the
  MSI-X-table-mapper NULL-deref in nvme/ahci/virtio_net (the F153 follow-up),
  fixed per each driver's IRQ-fallback model.

- **SCAN #19 -- refcount balance / lifetime.** NOT clean (3 real bugs). F156
  (HIGH UAF) fork/spawn error-path pmm_ref_dec of a VMA_MMIO frame shared
  without a matching inc -> UAF of a live GPU-scanned device frame; fixed by
  making the teardown mm-aware. F157 (MED UAF) sys_openpty force-closed
  installed fds via `->close()` (refcount bypass) -> shared-fd-table sibling
  UAF/double-free; fixed to sys_close/vfs_close. F158 (MED DoS) tcp listener
  close leaked queued-but-unaccepted ESTABLISHED children (~128 KiB each,
  unbounded); fixed by draining + RST'ing the accept backlog in tcp_pcb_free,
  with a deterministic `tcp_listener_drain_selftest`.

- **SCAN #20 -- signedness / truncation.** SWEPT CLEAN. Syscall lengths are
  uint64 + `_access_ok`; every packet-parser length-subtraction is guarded
  upstream; ext2 metadata math is fenced by tested primitives; device indices
  are `index_ok`'d and DMA products `mul_within_u32`/`ckd_mul_u64`'d. The only
  truncations are by-design and structurally non-exploitable (see below).

## Accepted LOW / recorded-not-fixed (no memory-safety impact)

- **ext2 32-bit i_size / ext2_vfs_seek `cur_pos = (uint32_t)new_pos`**: a
  >4 GiB file/seek truncates, but reads clamp `cur_pos` to the <=4 GiB
  `file_size` and `ext2_run_valid` range-checks the physical block -- a POSIX
  wrong-result nit, never OOB; and unreachable since ext2 files are <4 GiB.
- **vmm `pg_file_idx = (uint32_t)(off >> PAGE_SHIFT)`**: used ONLY as a pcache
  hash key (the disk read uses the full 64-bit offset); aliasing only above
  16 TiB, which the ext2/AHCI stack cannot reach.
- **sys_read validates `buf` via user_buf_prefault directly** rather than
  user_buf_check -- still defended (no user VMA for a kernel/non-canonical buf;
  addr+len wrap rejected). A uniformity tightening, not a bug.
- **mm_t.refcount** -- a dead field (thread sharing uses task_mm_t.refs);
  deletable, harmless.
- **tcp_timer_tick "T4" two-thread data race** -- the retransmit block mutates
  pcb seq accounting under rcu_read_lock without pcb->lock now that two timer
  threads run; the code documents it. NOT a lifetime/refcount bug (reap lists
  stay disjoint, no double-free) -- a correctness data race, separate concern.

## State

Branch `bughunt`, all 24 fixes committed (conventional, de-branded) and pushed;
`main` untouched. Shipping disk builds clean and boots with 88 selftests
PASSED, 0 faults. The bug-TYPE sweeps for deadlock, overflow, leak,
ignored-return, OOB, double-fetch, refcount, and signedness are complete;
#18 and #20 returned clean, a clear diminishing-returns signal, so the campaign
is wound down here. Future work, if resumed, would target a different axis
(e.g. the recorded "T4" timer data race, or a concurrency/SMP-ordering sweep).
