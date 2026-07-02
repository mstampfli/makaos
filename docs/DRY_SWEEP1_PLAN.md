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
- U-checksum [F178 DONE] transport_checksum() was __attribute__((unused)) while its body was copy-pasted 5x (udp recv+send, tcp send-seg+send-rst+recv-validate). Verified 4/5 byte-identical (same inet_pseudo_partial + word-sum + odd-byte fold + ~sum); tcp_send_rst OMITTED the `if(l) sum+=*(u8*)pw` odd-byte fold -- but RST is header-only (TCP_HDR_MIN_LEN=20, EVEN), so `l` is always 0 there and the omission never fired (latent-harmless). Made transport_checksum extern (dropped unused+static), declared in ipv4.h, routed all 5; the rst fix is by-construction (adds the dead-for-RST fold for consistency). NET gate: DHCP + tcp selftests PASS.
- U-virtio-cap [F177 DONE] find_virtio_cap + vcap_t + virtio_pci_common_cfg_t + the generic transport macros (VIRTIO_VENDOR, VIRTIO_PCI_CAP_* 1-5, VIRTIO_STATUS_* x5, VIRTIO_F_VERSION_1) were triplicated across virtio_net/gpu/input -- verified byte-identical (find_virtio_cap same logic, common_cfg field-identical). Extracted to new kernel/drivers/pci/virtio_pci.h (header-only, find_virtio_cap is static inline -> no new .c/build change); removed all 3 local copies. Device-specific constants (VIRTIO_DEV_*, per-device feature bits, GPU cmd opcodes, VTI_CFG_*, DRIVER_FEATURES) stay per-driver. Boot-critical: virtio-net DHCP + virtio-gpu init + virtio-input all exercised at boot = the gate.
- U-vaddr [F175 DONE] VERIFIED no bug: USER_ADDR_MAX (0x7FFF..FF, 2^47-1) and USER_ADDR_CEIL ((1<<47), 2^47) are NOT conflicting -- MAX is always paired with >/<= (inclusive last-valid), CEIL/(1<<47)/0x8000..00 always with >=/< (exclusive first-invalid); every site accepts exactly [0, 2^47). Pure dedup: moved the matched pair to common.h (invariant CEIL == MAX+1) as SSOT, removed the 2 local #defines (syscall.c/signal.c), routed the raw (1ULL<<47) x2 + 0x800000000000 x2 + 1 selftest vector through them. Kept BOTH constants (collapsing to one value would be wrong). Left vmm.c:755 0x8000000000000000 (2^63 top-bit kernel check, different concept). Values/operators unchanged.
- U-access-ok [F176 DONE] VERIFIED sig_user_range_ok is EXACTLY _access_ok (not weaker): both reject addr==0; addr>MAX == addr>=CEIL (CEIL==MAX+1); end-1>MAX == addr+len>CEIL; !ckd_add_u64 == (addr+len<addr) unsigned wrap; len==0 handled identically -- all input classes accept/reject the same set. No security drift. Moved _access_ok to a NEW kernel/include/uaccess.h (checked.h explicitly forbids subsystem/copy primitives), removed the syscall.c local def + fwd-decl, deleted sig_user_range_ok, routed signal.c's caller through _access_ok. uaccess.h is now the home for U-uaccess-hdr (copy_*_user decls) too. Behavior-identical; [access_ok_test]/[user_ptr_test] are the gate.
- U-ahci-const [F174 DONE] AHCI 248 PRDT / 130 page-cap literals -> AHCI_PRDT_ENTRIES/AHCI_MAX_IO_PAGES #defines (struct dim, build_prdt bound, 4 stack-array dims, xfer caps, selftest canary). Token-identical (248u/130u); build+boot verified.
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
- ext2 two path walkers (F174 verified: NOT a bug, NOT a merge). path_to_inode (raw, no perm) is only an internal re-resolver; every userspace path syscall goes through fs_lookup -> ext2_lookup_path(cred) first (checks EXEC on every dir component), and the mutating ops re-check write via ext2_dir_write_ok(parent, cred) on the same inode they modify. open uses ext2_open_ino by the already-resolved inode; ext2_open(raw) only after a perm-aware fs_lookup on the parent. Merging the two would double-check or break path_to_inode's internal callers -> justified separation (DRY is not a religion).
- tcp_send_rst checksum odd-byte omission (folded into U-checksum).
