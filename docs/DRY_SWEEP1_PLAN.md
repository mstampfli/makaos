# DRY consolidation phase -- sweep-1 plan (2026-07-01)

Source: 14-angle multi-agent DRY sweep (wf_0c566906-d28), each candidate
behavior-preservation + worthwhileness verified by 2 adversarial lenses.
27 confirmed consolidation units + drift-flags. Execute ONE unit/turn,
provably-safe-first, build+boot each. "drift-bug" flags are mostly the
verify lens re-stating candidates -- VERIFY each claimed divergence before
treating it as a bug.

## Shipped
- F172  console tcsetpgrp/TIOCSPGRP signal-hijack (real bug) -> one tty_set_fg_pgrp SSOT.
- F173  preempt_enable_no_resched() SSOT (10 sites).

## Confirmed units (ranked value/risk), pending
HIGH value, LOW risk:
- U-checksum   transport_checksum() helper exists but __attribute__((unused)); its body copy-pasted 5x (udp recv/send, tcp send-seg/recv). tcp_send_rst OMITS the odd-byte fold (latent bug). Route all 5 through the helper (+fix rst). NET-CRITICAL: verify byte-identical, boot DHCP+tcp is the gate.
- U-virtio-cap find_virtio_cap PCI cap-walk + vcap_t + the 3-cap/3-BAR map block + virtio_pci_common_cfg_t/macros triplicated across virtio_net/gpu/input -> shared virtio_pci.{h,c}. F155 proved the dup (same NULL-deref fixed 3x).
- U-vaddr      x86-64 canonical/2^47 user-VA boundary open-coded 30+ sites, 5 spellings, 2 CONFLICTING constant names -> one USER_ADDR_MAX/canonical helper. VERIFY the two constants have the same value (else picking one changes behavior = the real bug).
- U-access-ok  sig_user_range_ok (signal.c) hand-reimplements _access_ok with a manual overflow check vs ckd_add_u64 -> expose _access_ok via a shared uaccess header, route signal.c through it. VERIFY the hand-roll is not weaker (would be a security drift).
- U-ahci-const AHCI 248 PRDT / 130 page-cap bare literals (incl 4 stack-array dims) -> named #defines. Provably safe (rename).
- U-fifo       intrusive singly-linked FIFO enqueue-tail/dequeue-head hand-rolled 4x (socket, io_uring, unix_sock) -> one helper.
- U-strlcpy    4 hand-rolled truncating NUL copies + str_eq/s_streq 3x + str_to_uint dup -> shared string utils.
MED/LOW:
- U-uaccess-hdr copy_from_user/copy_to_user/user_buf_check re-declared by hand in 7 files -> one uaccess.h.
- U-ttybg      tty_is_background(tty) predicate (SIGTTIN/SIGTTOU) x4 (tty.c/pty.c).
- U-cred-id    "id in {r,e,s}" predicate: cred_seteuid vs spawn_cred_allowed -> shared helper (cred_setuid's 2-slot set stays distinct).
- U-msi        MSI-X find-cap+table-map+enable + legacy MSI block dup across ahci/nvme/virtio -> shared msix helper.
- U-align      align_up/align_down redefined per file in mm/.
- U-pgid-twin  sys_getpgid/sys_getsid rcu+find+zombie->ESRCH twin.
- U-chmod      non-root setuid-bit strip dup in sys_chmod/sys_fchmod.
- U-dcache     dcache LRU tail-push inlined 2x (head helper exists, no tail).
- U-vfs-alloc  ~20 heap vfs_file_t inits -> vfs_file_alloc(); anon-fd (eventfd/signalfd/timerfd) -> vfs_anon_fd.
- U-elf-unwind elf.c reimplements process.c task_create_unwind/task_init_common (VERIFY elf's fresh-image case; F156 was fork-only, elf builds fresh so no MMIO -- dedupe only).
- U-dirname    syscall.c dirname split x4 -> route through ext2 path_split (VERIFY the syscall hand-rolls' parent[] cap vs path length).

## Real drift-bugs to VERIFY then fix (not mere dedup)
- ext2 has two path walkers; the old one skips permission checks -- CONFIRM it is dead / not reachable on a perm-gated path, else it is an authz bypass.
- tcp_send_rst checksum odd-byte omission (folded into U-checksum).
