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

### C. Rings (producer/consumer accounting)
- `tcp_ring_consume(head, used, n, mask)` -- clamped drain, never underflows (F18).
- `tcp_ring_reserve(tail, used, want, size, mask, *start)` -- clamped reserve (F44).
- (`kernel/include/spsc.h` is the existing lock-free SPSC ring.) The sweep will
  reconcile tcp_ring_* / the tty rb_* helpers / `io_ring_index` into one generic,
  tested ring primitive used under the owning lock.

### D. On-disk / device values
Validate every value read from disk or a device against a trusted bound before
using it as an index/length/LBA, via the category-A primitives. Examples already
in tree: `ext2_block_valid` (F36), `virtio_desc_id_valid` (F22), `nvme_cid_valid`
(F26), `drm_atomic_count_ok` (F40). The sweep folds these onto `index_ok`/`in_range`.

## Status

Phase 2 (the primitive-extraction phase): category A landed (this commit).
Remaining: generalize B (copy_str_from_user), unify C into one ring primitive,
fold D's per-domain validators onto A, then sweep every call site -- one
subsystem per commit, each with a test. Beyond data, extract any other recurring
pattern (locking idioms, alloc/free pairs, retry loops) the same way.
