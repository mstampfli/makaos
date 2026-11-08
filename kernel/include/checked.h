#pragma once
#include "common.h"   // uint*_t, bool

// ── Checked arithmetic + bounds primitives ─────────────────────────────────
//
// THE canonical, 100%-safe building blocks for any computation on a value that
// is NOT fully trusted: a size, length, count, offset, or index that came from
// userspace, a device, or on-disk metadata.  Use these INSTEAD of a bare
// `a * b`, `a + b`, or `i < n` whenever any operand is attacker-influenceable,
// so an integer overflow or out-of-range index can never silently produce a
// too-small allocation, a wrapped bound, or an out-of-bounds access.
//
// All are PURE and inline -- no state, no allocation, no locks -- so they are
// exactly as fast as the hand-written check they replace.  Safety is by
// construction: the ckd_* functions delegate to the compiler's overflow
// builtins (no UB, no hand-rolled high-bit math to get wrong).
//
// PLACEMENT: these are the cross-cutting generics -- no single domain (fs,
// drivers, net, mm, syscall all use them), so this one small focused header is
// where they belong.  It is capped to the checked-arith + bounds family; a
// primitive that belongs to a subsystem (a device-id validator, a ring, a copy
// helper) lives in THAT subsystem's file, tagged `// PRIMITIVE`, and delegates
// the generic check to one of these.  This is NOT a catch-all header.
//
// PRIMITIVE (checked arithmetic + bounds, category A).
// Catalog + the full rules / forbidden patterns: docs/PRIMITIVES.md.

// in_range_u64(x, lo, hi): is lo <= x <= hi (INCLUSIVE both ends)?
//   USE for "is this scalar inside an inclusive validity window" -- a port, a
//   mode/flag value, a clamped parameter.
//   DO NOT use for an array index: an index is half-open (0..count-1); use
//   index_ok instead, or you will accept i == count.
static inline bool in_range_u64(uint64_t x, uint64_t lo, uint64_t hi) {
    return x >= lo && x <= hi;
}

// index_ok(i, count): is `i` a valid index into an array of `count` elements
// (i.e. i < count)?  count == 0 -> always false (no valid index exists).
//   USE before EVERY `arr[i]` where `i` is untrusted -- a device descriptor id,
//   an fd, a slot, a block/inode number checked against a count, a user-supplied
//   table index.  This is the single source of truth for "bounded index".
//   DO NOT use it as a substitute for a range that has a nonzero lower bound
//   (use in_range_u64), nor when `i` is a byte offset that must be combined with
//   a length (use ckd_add_* to form the end, then index_ok / a size compare).
static inline bool index_ok(uint64_t i, uint64_t count) {
    return i < count;
}

// ckd_mul_u32(a, b, out): set *out = a*b and return true IFF the product fits
// in uint32_t; on overflow return false and leave *out unspecified (callers
// MUST check the return before using *out).
//   USE for any `size = a * b` on untrusted operands -- pixels*bpp, count*elem,
//   sectors*512, nlb*lba_size.  A bare 32-bit multiply WRAPS and under-allocates
//   (the F23 DRM / F30 ext2 class).  Reject (return an error) when this is false.
//   DO NOT ignore the return value, and DO NOT use the 32-bit form when the true
//   product can legitimately exceed 4 GiB -- use ckd_mul_u64 there.
static inline bool ckd_mul_u32(uint32_t a, uint32_t b, uint32_t* out) {
    return !__builtin_mul_overflow(a, b, out);
}
static inline bool ckd_mul_u64(uint64_t a, uint64_t b, uint64_t* out) {
    return !__builtin_mul_overflow(a, b, out);
}

// ckd_add_u32(a, b, out): set *out = a+b and return true IFF the sum fits in
// uint32_t; on overflow return false (and *out is unspecified).
//   USE for `end = off + len`, `addr + size`, `head + n` bounds math on
//   untrusted operands -- the wrap that defeats a naive `if (off + len > MAX)`
//   (the F24 mmap-range class).  Form the end with ckd_add_*, then compare the
//   end against the limit.
//   DO NOT use the 32-bit form for 64-bit addresses (use ckd_add_u64).
static inline bool ckd_add_u32(uint32_t a, uint32_t b, uint32_t* out) {
    return !__builtin_add_overflow(a, b, out);
}
static inline bool ckd_add_u64(uint64_t a, uint64_t b, uint64_t* out) {
    return !__builtin_add_overflow(a, b, out);
}

// mul_within_u32(a, b, max, out): set *out = a*b and return true IFF the product
// is <= max.  The product is formed in 64-bit, so two u32 operands can NEVER wrap
// a u32 -- this defeats the `size = count * elem; if (size > limit) reject` failure
// where the multiply wraps and a huge request masquerades as a small one (the F47
// AHCI / nvme DMA-sizing class).  On false, *out is unspecified (do NOT use it).
//   USE for "size = a * b must fit within a known limit" on untrusted a/b -- a
//   sector count * sector size, a pixel count * bpp, a descriptor count * stride.
//   Passing max == UINT32_MAX reduces this to a pure no-u32-wrap check (a valid use
//   when the real cap is enforced downstream, e.g. an AHCI PRDT fragmentation check).
//   DO NOT use the 32-bit form when the product can legitimately exceed 4 GiB --
//   keep the value 64-bit end to end (use ckd_mul_u64 + a u64 compare) instead.
static inline bool mul_within_u32(uint32_t a, uint32_t b, uint32_t max, uint32_t* out) {
    uint64_t p = (uint64_t)a * b;     // 64-bit: two u32 operands cannot overflow
    if (p > max) return false;
    *out = (uint32_t)p;               // safe: p <= max <= UINT32_MAX
    return true;
}
