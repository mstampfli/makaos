#pragma once
#include "common.h"    // uint*_t, USER_ADDR_MAX
#include "checked.h"   // ckd_add_u64: overflow-safe add for the user-range guard

// ── User-address access primitives ─────────────────────────────────────────
//
// The single home for validating and copying to/from userspace pointers.
// Every syscall that touches a user pointer routes through here so the
// kernel/non-canonical/overflow rejection is decided in ONE place and cannot
// drift between call sites (a weaker hand-rolled copy is a security bug).
//
// _access_ok is the range validator; the copy_*_user family (declared by the
// subsystems that define them) builds on it.

// PRIMITIVE (user (addr,len) range validation, category B -> ckd_add_u64 wrap guard).
//
// User pointers may ONLY live in the canonical low half [0, 2^47).  Checking
// against HHDM_OFFSET alone let non-canonical addresses through -- anything in
// the 0x0000_8000_.. .. 0xffff_7fff_.. gap is < HHDM_OFFSET, so e.g. a
// scribbled 0x0053004300530000 passed the check and the subsequent memcpy
// #GP'd the kernel (writing a non-canonical address faults) instead of cleanly
// returning -EFAULT.  An untrusted user pointer must never crash the kernel:
// reject everything at or above the user ceiling (kernel AND the non-canonical
// gap).  USER_ADDR_MAX / USER_ADDR_CEIL (== MAX + 1) live in common.h.
static inline int _access_ok(uint64_t addr, uint64_t len) {
    if (!addr) return 0;
    if (addr > USER_ADDR_MAX) return 0;                      // kernel or non-canonical
    if (len) {
        uint64_t end;
        if (!ckd_add_u64(addr, len, &end)) return 0;         // addr+len wraps u64
        if (end - 1 > USER_ADDR_MAX) return 0;               // end past user ceiling
    }
    return 1;
}
