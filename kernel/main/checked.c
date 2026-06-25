// Deterministic self-test for the checked-arithmetic + bounds primitives
// (kernel/include/checked.h).  Header-only primitives, so the test lives here.
#include "checked.h"

#ifdef MAKAOS_BOOT_SELFTESTS
void checked_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    uint32_t o32; uint64_t o64;

    // ── ckd_mul_u32: fits vs overflow across the 2^32 boundary ────────────
    if (!ckd_mul_u32(0xFFFFu, 0x10000u, &o32) || o32 != 0xFFFF0000u) {  // = 0xFFFF0000, fits
        kprintf("[checked] FAIL mul32 0xFFFF*0x10000\n"); fails++; }
    if (ckd_mul_u32(0x10000u, 0x10000u, &o32)) {            // = 2^32 -> overflow
        kprintf("[checked] FAIL mul32 2^32 not caught\n"); fails++; }
    if (!ckd_mul_u32(0xFFFFFFFFu, 1u, &o32) || o32 != 0xFFFFFFFFu) {
        kprintf("[checked] FAIL mul32 max*1\n"); fails++; }
    if (ckd_mul_u32(0xFFFFFFFFu, 2u, &o32)) {               // -> overflow
        kprintf("[checked] FAIL mul32 max*2 not caught\n"); fails++; }

    // ── ckd_add_u32 at the wrap boundary ─────────────────────────────────
    if (ckd_add_u32(0xFFFFFFFFu, 1u, &o32)) {               // -> wrap
        kprintf("[checked] FAIL add32 wrap not caught\n"); fails++; }
    if (!ckd_add_u32(0xFFFFFFF0u, 0xFu, &o32) || o32 != 0xFFFFFFFFu) {  // = max exactly
        kprintf("[checked] FAIL add32 exact max\n"); fails++; }

    // ── 64-bit forms ─────────────────────────────────────────────────────
    if (ckd_mul_u64(0x100000000ull, 0x100000000ull, &o64)) {  // = 2^64 -> overflow
        kprintf("[checked] FAIL mul64 2^64 not caught\n"); fails++; }
    if (!ckd_mul_u64(0x100000000ull, 2ull, &o64) || o64 != 0x200000000ull) {
        kprintf("[checked] FAIL mul64 2^32*2\n"); fails++; }
    if (ckd_add_u64(0xFFFFFFFFFFFFFFFFull, 1ull, &o64)) {      // -> wrap
        kprintf("[checked] FAIL add64 wrap not caught\n"); fails++; }

    // ── index_ok: half-open; count 0 rejects everything ──────────────────
    if (index_ok(0, 0))  { kprintf("[checked] FAIL index 0<0\n"); fails++; }
    if (!index_ok(0, 5)) { kprintf("[checked] FAIL index 0<5\n"); fails++; }
    if (!index_ok(4, 5)) { kprintf("[checked] FAIL index 4<5\n"); fails++; }
    if (index_ok(5, 5))  { kprintf("[checked] FAIL index 5<5\n"); fails++; }   // == count -> invalid
    if (index_ok(0xFFFFFFFFFFFFFFFFull, 5)) { kprintf("[checked] FAIL index huge\n"); fails++; }

    // ── in_range_u64: inclusive both ends ────────────────────────────────
    if (!in_range_u64(5, 1, 10) || !in_range_u64(1, 1, 10) || !in_range_u64(10, 1, 10)) {
        kprintf("[checked] FAIL in_range inside\n"); fails++; }
    if (in_range_u64(0, 1, 10) || in_range_u64(11, 1, 10)) {
        kprintf("[checked] FAIL in_range outside\n"); fails++; }

    kprintf(fails ? "[checked] SELF-TEST FAILED\n"
                  : "[checked] SELF-TEST PASSED (ckd_mul/add overflow + index_ok + in_range)\n");
}
#endif /* MAKAOS_BOOT_SELFTESTS */
