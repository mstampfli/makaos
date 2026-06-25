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
- Test: `checked_selftest` (kernel/main/checked.c), wired in kernel/main.c.

### B. User memory (the syscall boundary) -- `kernel/syscall/syscall.c`
- `copy_from_user(dst, uptr, len)` / `copy_to_user(uptr, src, len)` -- validated
  byte copy (`_access_ok` + VMA-checked prefault; never a raw deref).
- `copy_path_from_user(dst, uptr, dstsz)` -- NUL-terminated path, page-granular,
  no over-read; returns length / -EFAULT / -ENAMETOOLONG (F39). To be generalized
  to `copy_str_from_user` during the sweep.
- `_access_ok(addr, len)` / `user_buf_check` -- validate a user range w/o copying.
  `_access_ok` and `mmap_round_len` now fold their addr+len / len+PAGE_MASK wrap
  guards onto `ckd_add_u64` (category A) -- the F19/F24 overflow class, exactly the
  use ckd_add documents. Direct `access_ok_selftest` covers the wrap branch that
  `mmap_range_selftest` reaches only indirectly. `copy_path_from_user` (F39) is
  already the general bounded NUL-terminated user-string copy; verified there are
  no raw user-string reads bypassing it (the dev[]/comm[] per-char loops read
  post-copy KERNEL buffers, not user pointers), so no copy_str_from_user rename is
  warranted -- it stays the named path wrapper.

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
  `ext2_blk_lba_selftest` drives the `blk*spb == 2^32` wrap boundary. Used by
  bcache_get + write_block via the `ext2_blk_to_lba(blk)` static wrapper.

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
