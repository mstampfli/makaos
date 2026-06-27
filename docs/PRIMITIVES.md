# MakaOS safe primitives

The canonical, reusable building blocks every new and existing code path should
use instead of re-deriving the same logic. The goal: each recurring concern has
**one** named, provably-safe, documented, tested implementation -- a single
source of truth -- so a whole class of bug cannot reappear and the code reads
the same everywhere.

## The rule (applies to all new code, and to every conversion sweep)

1. **Reuse before reinvent.** Before writing a check, a copy, a ring, a refcount
   dance, etc., look for an existing primitive that covers it without costing
   performance or correctness. Only if none fits, add a new primitive.
2. **Every primitive is safe by construction** against its bug class (overflow,
   OOB, UAF, races) -- not "careful if used right", but hard to misuse.
3. **Every primitive documents its USE CASE** in a header comment: what it is
   for, and explicitly what it is NOT for, so it cannot be applied incorrectly.
4. **Every primitive has a deterministic test.** Any change adds/extends a test.
5. **Catalog it here** with the single canonical implementation location.

## Where primitives live (placement + marking)

There is NO single catch-all primitives header. A primitive lives next to what it
serves, and every primitive is tagged so it is greppable (`grep -rn PRIMITIVE`):

- **Cross-cutting generics with no single domain** (checked arithmetic + bounds:
  `ckd_mul`/`ckd_add`, `index_ok`, `in_range`) live in one small focused header,
  `kernel/include/checked.h` -- fs, drivers, net, mm and syscall all use them, so
  the shared generic layer IS their home. That header is capped to this one family;
  unrelated primitives never get funnelled into it.
- **Domain-specific primitives** (a device-id validator, an on-disk-value check, a
  ring, a copy helper, a locking idiom) live in THAT subsystem's `.c`/`.h`, marked
  with a `// PRIMITIVE ...` comment, and DELEGATE the generic part to category A
  (e.g. `virtio_desc_id_valid` -> `index_ok`). The named domain wrapper documents
  the source of the value; the generic primitive does the actual check, once.
- When a new primitive replaces a hand-rolled helper, the old helper is removed in
  the same commit -- never two ways to do the same thing.

## Forbidden patterns (replace with the primitive)

- A raw `*uptr` / `while (uptr[i])` / `memcpy(.., uptr, ..)` on a user pointer
  -> use the user-memory primitives (copy_from_user / copy_to_user /
  copy_path_from_user / the access_ok predicate).
- A bare `a * b` or `a + b` forming a size/offset/end from an untrusted operand
  -> use `ckd_mul_*` / `ckd_add_*` and reject on overflow.
- A bare `arr[i]` / `i < n` where `i` is untrusted -> use `index_ok(i, count)`.
- An on-disk / device-supplied value used as an index, length, or LBA without a
  bounds check against a trusted limit -> validate with `index_ok` / `in_range`.

## Catalog

### A. Untrusted scalars: checked arithmetic + bounds -- `kernel/include/checked.h`
Pure, inline, zero-cost. Safety delegates to the compiler overflow builtins.
- `in_range_u64(x, lo, hi)` -- inclusive validity window (NOT for array indices).
- `index_ok(i, count)` -- half-open array index check `i < count` (count 0 -> false).
- `ckd_mul_u32/u64(a, b, *out)` -- a*b, returns false on overflow (check before use).
- `ckd_add_u32/u64(a, b, *out)` -- a+b, returns false on overflow.
- `mul_within_u32(a, b, max, *out)` -- a*b formed in u64 (two u32s cannot wrap),
  accept iff `<= max`. The `size = count*elem; if (size > limit) reject` pattern
  done safely (the F47 AHCI / nvme DMA-sizing class). max == UINT32_MAX is a pure
  no-u32-wrap check. Reused by ahci `xfer_bytes_ok` + nvme_rw (single source).
- Test: `checked_selftest` (kernel/main/checked.c), wired in kernel/main.c.

### B. User memory (the syscall boundary) -- `kernel/syscall/syscall.c`
- `copy_from_user(dst, uptr, len)` / `copy_to_user(uptr, src, len)` -- validated
  byte copy (`_access_ok` + VMA-checked prefault; never a raw deref).
- `copy_path_from_user(dst, uptr, dstsz)` -- NUL-terminated path, page-granular,
  no over-read; returns length / -EFAULT / -ENAMETOOLONG (F39). To be generalized
  to `copy_str_from_user` during the sweep.
- `_access_ok(addr, len)` / `user_buf_check(addr, len)` -- validate a user range
  the kernel will deref DIRECTLY (range-check + prefault), w/o copying. USE this for
  any syscall arg the kernel reads/writes through a raw pointer instead of copy_*_user
  (io_uring OP_READ/WRITE, and now the socket recv/send/accept/connect/bind handlers
  -- F51, which previously passed buf_ptr/addr_ptr raw to tcp_recv_data `dst[i]=` /
  socket_accept / sa->sin_*, an arbitrary-kernel-R/W LPE).
  Direction matters (F52): a READ syscall (kernel WRITES the buffer) prefaults so
  absent pages are backed before the direct memcpy; a WRITE syscall (kernel READS
  the buffer) must only range-check (`_access_ok`) -- prefaulting a read-from-user
  buffer would allocate ZERO pages for absent pages (wrong direction) and wedges
  userspace.  `user_buf_prefault` itself is now overflow-safe (F52): it iterates by
  page COUNT, so a huge len can no longer wrap its bound, skip the loop, and return
  "ok" without validating anything (which defeated sys_read -> read(sockfd, KADDR, ~0)).
  `_access_ok` and `mmap_round_len` now fold their addr+len / len+PAGE_MASK wrap
  guards onto `ckd_add_u64` (category A) -- the F19/F24 overflow class, exactly the
  use ckd_add documents. Direct `access_ok_selftest` covers the wrap branch that
  `mmap_range_selftest` reaches only indirectly. `copy_path_from_user` (F39) is
  already the general bounded NUL-terminated user-string copy; verified there are
  no raw user-string reads bypassing it (the dev[]/comm[] per-char loops read
  post-copy KERNEL buffers, not user pointers), so no copy_str_from_user rename is
  warranted -- it stays the named path wrapper.
- `copy_sockaddr_un_from_user(ksa, addr_ptr)` (syscall.c, F110) -- copy a user
  sockaddr_un into a kernel buffer AND force sun_path NUL-termination
  (`sun_path[sizeof-1]='\0'`).  sys_bind/connect/sendto used to cast addr_ptr to a
  user `sockaddr_un_t*` and walk sa->sun_path as a C-string (ns_hash_str/ns_streq +
  debug print); user_buf_check validated only the 110-byte struct, so a 108-non-NUL
  sun_path read past it into the next page -> #PF DoS.  All three AF_UNIX sites now
  go through this one helper, so no user memory is ever walked as a string; the
  downstream ns_hash_str/ns_streq are additionally capped at UNIX_PATH_MAX
  (safe-by-construction).  This is the "copy + NUL-cap a user struct that the kernel
  will then treat as a C-string" idiom -- apply it wherever a user struct carries a
  to-be-walked string field.  `unix_ns_str_selftest` covers the bounded hash/streq.
  The SAME idiom applied to the sys_spawn SPAWN_ATTR_UNVEIL array (F122): the unveil
  entries were copied with `_access_ok` + a RAW memcpy (an unmapped-but-in-range
  pointer #PF-panicked the kernel), and each entry's user-controlled char[256] path
  was fed to unveil_add's UNBOUNDED s_strlen (an OOB heap read past the copy buffer).
  Now: `copy_from_user(ue, a.unveil, n*sizeof)` (faults -> -EFAULT, never panics) +
  a SPAWN_UNVEIL_MAX=256 cap on the attacker-sized count + force-NUL each
  `ue[i].path[255]='\0'` before unveil_add.  This was the last `_access_ok` + raw
  memcpy of a user array in sys_spawn (the spawn_attr_t copy was already converted).
  (Sibling raw-out-param fixes F109: sys_wait/pipe/fb_info copy_to_user a kernel
  local; sys_nanosleep/shm_open/shm_unlink copy_from_user into one; sys_setsockopt
  bounces optval into a kernel buffer so no option handler derefs the raw pointer.)
- ZERO-THEN-FILL every out struct/buffer before copy_to_user (F124/F125, the
  SCAN #10 info-leak class).  A struct/buffer copied to userspace must have EVERY
  byte defined -- not just the fields the handler sets, but struct PADDING (gaps
  between differently-sized fields, tail padding) and any unwritten buffer tail.
  Leaving them stale leaks kernel stack/heap (pointers, other tasks' data) to an
  unprivileged reader.  RULE: `struct X out; __builtin_memset(&out, 0, sizeof out);`
  (or zero a kmalloc'd buffer -- there is NO kzalloc in this tree) BEFORE filling,
  so padding AND future-added fields are always clean.  Zero at the SINGLE copy-out
  owner, not per-filler (F124 zeroes the readdir kbuf once at sys_readdir, covering
  the ext2/proc/dev/injected fillers in 3 files, instead of patching each).
  Established convention: sys_stat/sys_fstat/sys_fb_info/getrusage/select already
  memset (or `= {0}`) their out struct first; F124 (sys_readdir kmalloc heap) and
  F125 (sys_uname stack, strncpy_k does not zero-pad) brought the two stragglers
  onto it.  `out_struct_zerofill_selftest` pins the struct sizes (a future
  field-add that creates uncovered padding fails the build-boot) + asserts the
  tail/padding is zero after the zero-before-fill.  NOTE: a handler that
  copy_from_user's the whole arg IN then writes it back OUT (the DRM GET pattern)
  is already safe -- untouched padding round-trips the USER's own bytes.

### C. Rings (producer/consumer accounting)
- `tcp_ring_consume(head, used, n, mask)` -- clamped drain, never underflows (F18).
- `tcp_ring_reserve(tail, used, want, size, mask, *start)` -- clamped reserve (F44).
- (`kernel/include/spsc.h` is the existing lock-free SPSC ring.) The sweep will
  reconcile tcp_ring_* / the tty rb_* helpers / `io_ring_index` into one generic,
  tested ring primitive used under the owning lock.

### D. On-disk / device values
Validate every value read from disk or a device against a trusted bound before
using it as an index/length/LBA, via the category-A primitives. The per-domain
validators stay (they document the domain + the source of the value) but their
BODY delegates to category A, so the bounds/overflow logic lives in one place.

Folded onto category A (single source of truth for the check):
- `virtio_desc_id_valid(id)` -> `index_ok(id, VIRTQ_SIZE)`  (kernel/net/virtio_net.c)
- `nvme_cid_valid(cid)` -> `index_ok(cid, NVME_IOQ_DEPTH)`  (kernel/drivers/storage/nvme.c)
- `ext2_block_in_range` upper bound -> `index_ok(blk, count)`  (kernel/fs/ext2.c)
- `ext2_run_in_range` overflow-safe `start+run<=count` -> `ckd_add_u32`  (kernel/fs/ext2.c)
Each verified a true behavioral match; covered by the existing
`virtio_desc_id_valid_selftest` / `nvme_cid_valid_selftest` / `ext2_block_valid_selftest`
(the latter exercises the `ckd_add` wrap boundary `{0xFFFFFF00, 0x200}`).

New domain primitive (not a fold -- correct WIDTH, no checked-arith primitive needed):
- `ext2_blk_lba(part_lba, blk, spb)` (kernel/fs/ext2.c, F46) -- block number -> 64-bit
  device LBA, formed in u64 so `blk*spb` cannot wrap a u32 to a wrong sector. Pure;
  `ext2_blk_lba_selftest` drives the `blk*spb == 2^32` wrap boundary. Used at ALL
  four LBA sites via the `ext2_blk_to_lba(blk)` static wrapper: bcache_get,
  write_block, and the two inode_build_run DMA read paths (the ELF-loader /
  bulk-read fast path) -- the latter two were a second pair of the same bug, caught
  on the verify-all-angles "same class elsewhere" sweep after the first F46 commit.
- `xfer_bytes_ok(sectors, sector_size, max_bytes, *out)` (kernel/drivers/storage/ahci.c,
  F47) -- thin domain wrapper over `mul_within_u32`: rejects a zero-sector transfer,
  else forms the byte length in u64 and bounds it, so `sectors*sector_size` cannot wrap
  a u32 and slip past a byte-based DMA guard (the wrap makes a huge request look tiny ->
  PRDT/page bound passes -> HBA moves the huge count -> overrun). Used at all three AHCI
  count*512 sites (ahci_submit_hhdm, ahci_submit_sg, ahci_read_user, replacing the bare
  `count*512` + the now-redundant `npages>130` check). `xfer_bytes_ok_selftest` drives
  the `sectors*512 == 2^32` wrap. (Latent: current callers cap count <= 1024.) The
  SAME-CLASS nvme sibling is now FIXED (F48): nvme_rw `nlb * s_ns_lba_size` (which wraps
  while cdw12 carries a 16-bit-masked sector count -> a 32 MB DMA into a 4 KB PRP) now
  uses `mul_within_u32(nlb, s_ns_lba_size, 8192, &bytes)`; covered by the mul_within
  cases in checked_selftest (nvme itself is not the boot device).
- `nvme_lba_size_ok(lbads, *size)` (kernel/drivers/storage/nvme.c, F137) -- validates the
  device-supplied Identify-Namespace LBA Data Size exponent (LBADS, 8 bits, 0..255) BEFORE
  `1u << lbads`: a shift count >= 32 is UB (x86 masks to & 31 -> a tiny bogus sector size)
  and a sub-512 / oversized size defeats the nvme_rw mul_within DMA guard above (the guard
  under-counts -> OOB DMA past the 2-PRP region). Accepts only lbads 9..12 (512..4096-byte
  sectors), computes the shift with no UB, and nvme_init refuses the namespace otherwise.
  This is the upstream sibling of the mul_within guard: it keeps s_ns_lba_size itself sane
  so the guard's bound is meaningful. `nvme_lba_size_ok_selftest` drives the sub-512 /
  oversized / UB-shift (32, 255) rejects. The forbidden raw `1u << <untrusted>` is gone.
- `vgpu_fb_bytes(w, h, *out)` (kernel/drivers/video/virtio_gpu.c, F49) -- the w*h*4
  bytes a w*h B8G8R8X8 scanout needs, via `mul_within_u32` (u64, no wrap) capped at
  VGPU_MAX_FB_BYTES (256 MiB). w/h are the device-reported scanout mode (untrusted);
  the old `uint32_t bytes = w*h*4` wrapped for a large/crafted mode -> undersized
  backing while the GPU + paint loop access the full w*h -> host OOB. Rejects a 0
  dimension / over-cap mode (vgpu_setup_scanout_buffer then returns 0; both callers
  handle it). `vgpu_fb_bytes_selftest` drives the 2^32 wrap + over-cap rejects. Most
  drm.c size math (drm_dumb_size F23, addfb/atomic) already uses u64; the one gap was
  SET_CURSOR -- see drm_cursor_bytes below.
- `drm_cursor_bytes(width, height, backing_size, *bytes)` (kernel/drivers/video/drm.c,
  F136) -- the width*height*4 bytes a 32bpp ARGB cursor needs, which must fit the dumb
  buffer that backs it. width*height is a u64 product of two u32 (cannot overflow u64)
  and the *4 is guarded by `ckd_mul_u64`. The old SET_CURSOR guard `(uint64_t)w*h*4 >
  d->size` (and the matching pin-page count `(w*h*4 + 4095)/4096`) WRAPPED u64 once w*h
  >= 2^62 (w=h=0x80000000 -> *4 == 2^64 -> 0): `0 > d->size` is false, so user-supplied
  absurd cursor dimensions slipped past the bound into resource_create/resource_transfer
  (host-side OOB on the small backing) with a wrapped-to-zero pin count. w/h come
  straight from the DRM_IOCTL_MODE_CURSOR struct (untrusted, uncapped). Reused for the
  pin-page count so both share one overflow-safe value. `drm_cursor_bytes_selftest`
  drives the 2^64 *4 wrap + over-backing + zero-dim rejects.
- `ext2_inode_size_valid(inode_size, block_size)` (kernel/fs/ext2.c, F50) -- gates
  the untrusted superblock s_inode_size at mount: power of two, >= sizeof(ext2_inode_t),
  <= block_size. That makes inode_size divide block_size, so an inode tiles a block
  without straddling -> (local*inode_size) % block_size + sizeof(inode) <= block_size,
  closing the OOB bcache-slot read (inode_load_into) + scratch write (inode_disk_write)
  a crafted s_inode_size (only `> 0`-checked before) caused. Sibling of ext2_block_size_checked.
  `ext2_inode_size_valid_selftest` drives the non-pow2 / too-small / too-big / straddle rejects.
- `ext2_dirent_in_block(off, rec_len, name_len, blk_bytes)` (kernel/fs/ext2.c, F54) --
  validates an on-disk directory entry's untrusted rec_len/name_len: the entry stays
  in the block (off + rec_len <= blk_bytes) and holds its aligned header+name
  (align4(8+name_len) <= rec_len). Guards both write-side walks (dir_add_entry slack
  split, dir_remove_entry name compare); without it a corrupt rec_len/name_len
  underflowed `rec_len - actual_len` and split-wrote past the 4096-byte heap block
  (heap OOB write). Mirrors the read-side ext2_dirent_namelen_clamp.
  `ext2_dirent_in_block_selftest` drives the underflow / past-block / rec<8 rejects.

### F. Allocation-result sentinels (test the RIGHT failure value)
A fallible allocator whose failure value is NOT 0 must be tested against its actual
sentinel, never `!x` / `== 0`. The PMM is exactly this trap: pmm_buddy_alloc /
vmm_alloc_pml4 return `PMM_INVALID_ADDR == UINT64_MAX` on OOM, so a `!phys` test
NEVER fires and `phys + HHDM_OFFSET` then wraps to a wild kernel pointer (a wild
memset / a wild DMA base programmed into a device). One named test closes the class:
- `PMM_ALLOC_OK(p)` := `((p) != PMM_INVALID_ADDR)` (kernel/mm/pmm.h, F121) -- the
  canonical "is this pmm_buddy_alloc / vmm_alloc_pml4 result a VALID frame" test.
  Documented in-header with WHY `!phys`/`==0` is wrong. Used at EVERY driver
  DMA-init alloc site (ahci/hda/ac97/virtio_gpu/virtio_input, 20 sites); mm/** and
  io_uring/drm/nvme use the equivalent explicit `== PMM_INVALID_ADDR`. RULE: any new
  pmm_buddy_alloc / vmm_alloc_pml4 caller gates the result through PMM_ALLOC_OK (or
  the explicit `== PMM_INVALID_ADDR`) -- NEVER `!phys`, and NEVER narrow to a
  uint32 before the check (UINT64_MAX truncates to 0xFFFFFFFF, non-zero -> the
  truncated check silently misses; the F121 ac97 BDL site was exactly this bug).
  No standalone selftest (the macro is a one-line sentinel compare; every boot
  exercises the success branch through ~4 drivers' DMA init).

### G. Fixed-width wire/struct field bounds (cap BEFORE the narrowing)
A value that will be written into a fixed-width on-the-wire or on-disk length
field must be REJECTED if it exceeds that field's range, BEFORE it is narrowed
into the field -- never narrowed first and bound after (the narrowing wraps and
the post-narrow check passes a corrupt small value).  Cap at the protocol/struct
chokepoint, carry the value full-width up to that point, and return a real errno.
- `UDP_MAX_PAYLOAD` (kernel/net/udp.h, F123) = 65535 - IPV4_HDR_LEN - UDP_HDR_LEN
  = 65507: the largest UDP payload that overflows NEITHER the 16-bit UDP length
  field NOR the 16-bit IP total-length field.  `udp_send_ex` (the single sink for
  both send/sendto) takes a full-width `uint32_t len` and rejects `len >
  UDP_MAX_PAYLOAD` with -EMSGSIZE as its FIRST statement, so the later
  `(uint16_t)(UDP_HDR_LEN + len)` cannot wrap.  `udp_send_size_selftest` drives
  the boundary + the old wrap values {65528,65535,...}.
- IP total-length backstop (kernel/net/ipv4.c, F123): `if (skb->len > 0xFFFF -
  IPV4_HDR_LEN) return -EMSGSIZE;` at the top of `ipv4_send_ex` -- the UNIVERSAL
  L3 chokepoint, so the IP total_len field cannot wrap for ANY L4 protocol
  (one shared guard for UDP/TCP/ICMP, not a per-protocol check that can drift).
  RULE: any new code forming a fixed-width length/size field from a wider value
  caps the value at that field's max at the chokepoint, full-width, before the
  narrowing cast -- mirror UDP_MAX_PAYLOAD / the ipv4_send_ex backstop.

### E. Locking idioms (concurrency primitives, not data validation)
The recurring "publish/snapshot a whole multi-field struct under its lock so a
lock-free reader never observes a half-written value" pattern. A plain struct
assignment is a multi-word copy: writing it under the lock is NOT enough -- the
reader must take the SAME lock, or it can catch the assignment mid-flight.

- `tty_set_termios(tty, *src)` / `tty_get_termios(tty, *dst)` (kernel/drivers/tty/tty.c,
  F106) -- publish / snapshot the whole `termios_t` under `tty->lock`. TCSET* overwrites
  the entire termios; a lock-free line-discipline reader (tty_input_char's ISIG/ICANON
  decision against c_cc[], tty_ldisc_drain's ICANON/VMIN) racing that write could mix a
  new c_lflag with an old c_cc[] byte and swallow a ^C / fire a spurious signal / drain
  in the wrong mode. ALL termios writers publish through tty_set_termios and ALL
  whole-struct readers (the TCSET* sites land copy_from_user in a stack local first;
  tty_input_char snapshots once at entry; both TCGETS sites snapshot then copy_to_user)
  go through this one pair, so copy_from_user/copy_to_user (which can fault) never run
  under the preempt-disabled spinlock and a reader never sees a torn struct. Output-path
  single-field reads (c_oflag OPOST/ONLCR, c_lflag TOSTOP) intentionally stay lock-free:
  atomic single-field loads, no cross-field dependency, hot path. `tty_termios_snapshot_selftest`
  drives byte-for-byte round-trip + full-overwrite republish + ldisc-obeys-published-mode.
  This is the first cataloged member of the "snapshot/publish a shared struct under its
  owning lock" family; apply it wherever a multi-field struct is overwritten wholesale
  while a reader walks its fields lock-free.

- `task_drop_files(task_t* t)` (kernel/proc/process.c, F107) -- drop a task's fd table on
  exit in the ONE correct order: unpublish `t->files_shared` with a RELEASE store, THEN
  `task_files_release`. task_files_release RCU-defers the table-struct free, and a
  /proc/<pid>/fd reader snapshots files_shared (then tf->ft, then a fd) under rcu_read_lock;
  call_rcu_expedited runs the grace period and frees INLINE, so the unpublish MUST precede
  the release or a reader entering its rcu_read_lock section just after it loads a freed
  table (cross-process UAF). Both exit paths -- sys_exit and the fatal-signal SIG_DFL
  terminate -- now go through this, so the order is a single source of truth and cannot
  drift (it had: the signal path freed before unpublishing, which is what F107 fixed).
  `task_files_drop_selftest` proves the unpublish + single-release on a refs==2 table.
  This is the "unpublish-before-RCU-deferred-free" ordering primitive: apply it wherever a
  pointer to an RCU-freed object is cleared around the deferred free.

- `fd_table_clone(dst, src)` (kernel/proc/process.c, F126) -- clone a task's fd table for
  fork / thread-copy / spawn, vfs_dup'ing each open file UNDER `src->lock`.  The three sites
  (task_fork, sys_thread, resolve_stdio) used to read a slot and vfs_dup it LOCK-FREE, which
  is a use-after-free when the table is shared across threads: a sibling sys_close() can free
  the file in the gap and vfs_dup then loads f->refcount on freed memory.  Per vfs.h, a
  lock-free OBSERVED file pointer must use vfs_tryget (CAS-if-nonzero); but the correct fix
  here is to hold the OWNING lock so the observation is not lock-free at all -- under
  src->lock a non-NULL slot is guaranteed live (its own ref keeps refcount >= 1, and close
  cannot NULL+free it without the lock), so the plain vfs_dup is safe AND src->ft is pinned
  against a concurrent grow's RCU free.  This is the "vfs_dup under the owning fd-table lock"
  rule, mirroring sys_close / fd_install which mutate the table under the same lock; it is
  the single source for the full-table clone (resolve_stdio inlines the same lock for its
  one-fd dup, reading the slot ONCE -- it previously double-fetched it).  vfs_dup / the
  fdtable_alloc kmalloc never sleep, so holding the spinlock across them is sound (fd_table_grow
  already allocates under this lock).  RULE: never vfs_dup a file observed from a table you do
  not own without holding that table's lock (or an RCU section + vfs_tryget).
  `fd_table_clone_selftest` pins the clone logic (cap + slot pointers + flags copied, refs bumped).

- `ext2_dir_write_ok(dir_ino, cred)` (kernel/fs/ext2.c, F129) -- the "permission-check on the
  operation's OWN resolution" idiom that closes a path-TOCTOU.  A two-step "resolve path + check
  perm" then "re-resolve path + act" is a TOCTOU: a concurrent rename of an intermediate component
  swaps the target between the check and the act, so the act runs on a parent the check never saw.
  The fix is NOT to re-check at the syscall layer (that resolves a THIRD time) but to perform the
  permission decision INSIDE the mutating op, on the SAME parent inode the op resolved and is about
  to modify -- so check and use are one resolution and cannot diverge.  ext2_unlink/mkdir/rename
  (rename: BOTH parents under s_rename_lock) and ext2_create-via-ext2_write_file's create path each
  call ext2_dir_write_ok on their own resolved parent_ino before mutating; a NULL cred skips it (the
  overwrite/truncate callers that act on an EXISTING file, never create).  The syscall-layer check is
  kept for the EACCES errno + early reject; the op-internal check is the authoritative TOCTOU-closer
  (identical vfs_check_perm, so zero behavior change in the non-racing case).  RULE: when a
  permission/validity check and the operation it guards each independently resolve the same untrusted
  name/handle, move the check INTO the operation, onto the operation's own resolution.
  `ext2_perm_op_selftest` exercises root create/unlink/mkdir/rename through the in-op check.

- `unix_sock_t.lock` + the "lock the queue OWNER, copy user data OUTSIDE the lock" discipline
  (kernel/net/unix_sock.c, F111/F112/F113; full rules in docs/LOCKS.md) -- one per-socket leaf
  spinlock serializes ALL of an AF_UNIX socket's per-socket queues (listener backlog, dgram rx,
  SCM_RIGHTS ancillary, stream cbuf), which had assumed the retired single-CPU model. TWO reusable
  rules: (1) each op locks EXACTLY the per-socket lock of the socket that OWNS the queue/buffer it
  mutates (a send mutates the peer's queue -> locks the PEER's lock), so two connected sockets'
  traffic uses two independent locks and never AB-BA deadlocks; (2) a spinlock is NEVER held across
  a user-memory access -- list/pointer queues keep the user copy before/after the enqueue (dgram),
  and the stream ring bounces user data through a bounded kernel chunk (cbuf_write_locked/
  cbuf_read_locked operate on a KERNEL buffer; the 512-byte bounce caps both stack and lock-hold).
  Apply rule (1) to any cross-CPU producer/consumer queue and rule (2) to any lock that guards a
  buffer the kernel also fills from / drains to userland.

- `fd_install_cloexec(f, cloexec)` (kernel/syscall/syscall.c, F115) -- install an fd AND set its
  FD_CLOEXEC flag in the SAME tf->lock critical section (sets fd_table[fd] and fd_flags[fd] together,
  then unlocks).  fd_install(f) delegates with cloexec=0.  Setting fd_flags as a SEPARATE unlocked
  step after fd_install (the old sys_socket path) raced a sibling fd_table_grow (COW + RCU-publish
  under tf->lock): the flag could land in the freed old table and be lost (the fd then survived exec).
  This is the "set a slot's flags in the SAME lock that installs the slot, never as a follow-up store"
  primitive -- the single chokepoint for fd creation; any future create-with-cloexec (accept4,
  epoll_create1, eventfd) must use it, not a post-install store.
  COROLLARY (fd-creating-syscall ROLLBACK discipline, F117): when a multi-fd syscall (pipe, socketpair)
  fails AFTER installing one or more ends, roll back each end by ITS STATE -- an INSTALLED end via
  `sys_close(fd)` (which clears fd_table[fd] AND drops the ref), an UN-installed end via `vfs_close(f)`.
  A bare vfs_close on an installed end frees it while the fd-table slot still points at it -> a later
  close/exit double-frees (the F117 sys_pipe bug).  socketpair and the pipe copyout-fail path already do
  this; the rule is "never vfs_close a vfs_file_t that is reachable from the fd table -- go through its fd".

- `vmm_map_mmio` MMIO VA bump-reserve (kernel/mm/vmm.c, F116) -- the lock-free-bump-allocator-with-ceiling
  shape: reserve a disjoint span via `base = __atomic_fetch_add(&cursor, span, RELAXED)` (two concurrent
  callers can never get overlapping ranges -- no lock needed) AND bound it against a documented end with the
  overflow-safe test `base >= END || span > END - base` (never compute base+span), failing the allocation
  rather than running past the window.  Apply this to any monotonic VA/ID/offset cursor that hands out
  ranges: the atomic fetch_add makes it concurrency-safe-by-construction and the ceiling turns silent
  exhaustion into an explicit, detectable failure (mirrors the checked.h bounds family for the wrap guard).

- Bounded-resource CAP + timer REAPER (io_uring overflow F118; TCP SYN_RCVD half-opens F119) -- any resource a
  remote/unprivileged peer can create (overflow CQEs, half-open connections, queued datagrams) needs an explicit
  CAP (drop past it) AND, if the resource can get stuck (a half-open whose peer vanishes), a timer-driven REAPER
  that frees it after a deadline.  CAP alone bounds memory but a stuck resource still blocks the slot; the reaper
  drains it.  REAPER RULES (the F119 hazards): (1) if the timer walks an RCU list, the FREE must run AFTER
  rcu_read_unlock (synchronize_rcu_expedited PANICS under a reader) -- collect victims under the list lock via a
  SEPARATE link field (leaving ->next intact for concurrent readers), then one grace period per batch; (2) if a
  second thread can "complete" the resource (a handshake), serialize the complete-vs-reap decision on ONE lock +
  a `reaped` flag (complete commits only if !reaped; reap claims only if not completed) so neither frees/queues a
  resource the other owns (no UAF).

## Status

Phase 2 (the primitive-extraction phase): category A landed (cfbc0f6); D's device
+ on-disk index/run validators folded onto A (a1509ff); B's user-range overflow
guards (`_access_ok` / `mmap_round_len`) folded onto `ckd_add_u64` + direct
`access_ok_selftest` (89c3fb2); F46 added `ext2_blk_lba` (u64 LBA, fixed a real u32
wrap bug) (this commit). Verified NOT foldable / correct-by-construction (kept as-is,
do not retry): `drm_dumb_size` (F23, DRM_MAX_FB_DIM policy cap + u64 math, not a
hand-rolled overflow check); `drm_atomic_count_ok` (F40, inclusive `<= MAX`, index_ok
would be off-by-one); `io_uring compute_layout` (clamps entries to IO_URING_MAX_ENTRIES
before the u64 size multiplies, same cap pattern as drm). Remaining: unify C into one
ring primitive (check spsc.h), then sweep remaining untrusted `a*b`/`a+b` call sites --
one subsystem per commit, each with a test. Beyond data, extract any other recurring
pattern (locking idioms, alloc/free pairs, retry loops) the same way.

PLANNED FINAL phase -- primitive uniformity sweep. Once the bug hunt + the remaining
system / bug-type / security audits are done, scan the WHOLE codebase for WHERE these
primitives should be applied EVEN IN CORRECT, bug-free code, and retrofit them so the
safe handling is uniform and future-proof (new code lands on the safe rails by
default). Robustness + consistency, not bug-finding: each conversion must be a TRUE
behavioral match (skip + record correct-by-construction sites), tagged, tested or
clean-boot-proven, one subsystem per commit.
