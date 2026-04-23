/* ── Stack walker — DEBUGGING.md §5 implementation ─────────────────
 *
 * Frame-pointer chain walker with symbolization.  Safe from every
 * context — no allocation, no locks, no IRQ fiddling.  Tolerant of
 * corrupted stacks: an unreadable frame pointer or implausible
 * return address prints `???` but doesn't stop the walk (§5.4).
 */

#include "stackwalk.h"
#include "kprintf.h"
#include "common.h"

/* Kernel image extents from the link script.  Used to filter out
 * backtrace frames that clearly aren't code (e.g., stack garbage the
 * walker wandered into when -fomit-frame-pointer code broke the
 * chain). */
extern char __kernel_start[];
extern char __kernel_end[];

/* Plausibility: the address must be inside the kernel image.  A
 * random stack qword masquerading as rbp usually points way outside
 * this range, giving us a clean "stop walking" signal. */
static inline int is_kernel_text(uint64_t addr) {
    return addr >= (uint64_t)__kernel_start && addr < (uint64_t)__kernel_end;
}

/* Read `*p` safely — we can't page-fault recursively in the panic
 * path or we triple-fault.  x86-64 kernel addresses above HHDM are
 * always mapped in the current page tables (HHDM is per-process-
 * shared via the same top-half), so a direct dereference after a
 * canonical-address check is safe.
 *
 * If the address is non-canonical (bit 63 set but bits 62..48 zero,
 * or bit 63 zero but bits 62..48 set), dereferencing would #GP.  Be
 * defensive and bail. */
static int safe_read_u64(uint64_t addr, uint64_t* out) {
    /* Canonical address check: bits [63:47] must all be 0 or all be 1. */
    uint64_t top = addr >> 47;
    if (top != 0 && top != 0x1FFFFull) return 0;
    /* 8-byte alignment for safety — kernel stack qwords are aligned. */
    if (addr & 7u) return 0;
    *out = *(const volatile uint64_t*)addr;
    return 1;
}

/* Binary search in the sorted symbol table for the largest entry
 * whose address is <= rip.  That's the function containing rip. */
const char* ksym_lookup(uint64_t rip, uint64_t* out_offset) {
    if (g_ksyms_count == 0) { if (out_offset) *out_offset = 0; return (const char*)0; }
    size_t lo = 0, hi = g_ksyms_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_ksyms[mid].addr <= rip) lo = mid + 1;
        else                          hi = mid;
    }
    if (lo == 0) { if (out_offset) *out_offset = 0; return (const char*)0; }
    const ksym_entry_t* e = &g_ksyms[lo - 1];
    if (out_offset) *out_offset = rip - e->addr;
    return e->name;
}

/* Emit one frame line.  Uses kprintf directly; caller context decides
 * whether the serial lock is already held. */
static void print_frame(int idx, uint64_t rip) {
    uint64_t off;
    const char* name = ksym_lookup(rip, &off);
    if (name) kprintf_atomic("  #%d 0x%016lx <%s+0x%lx>\n", idx, rip, name, off);
    else      kprintf_atomic("  #%d 0x%016lx <?>\n",         idx, rip);
}

void stackwalk_print_from(uint64_t rbp, uint64_t rip) {
    kprintf_atomic("=== stack backtrace ===\n");
    /* Frame 0 is the captured RIP itself. */
    print_frame(0, rip);

    int depth = 1;
    uint64_t cur = rbp;
    for (; depth < 128; depth++) {
        uint64_t next_rbp, ret_rip;
        if (!cur) break;
        if (!safe_read_u64(cur,     &next_rbp)) {
            kprintf_atomic("  #%d 0x%016lx <??? (bad rbp=0x%lx)>\n", depth, 0UL, cur);
            break;
        }
        if (!safe_read_u64(cur + 8, &ret_rip)) {
            kprintf_atomic("  #%d 0x%016lx <??? (bad ret@rbp+8)>\n", depth, 0UL);
            break;
        }
        /* Zero return address is a terminator (typical on entry-point
         * stacks set up by the scheduler). */
        if (ret_rip == 0) break;
        /* Implausible ret in random garbage means the rbp chain
         * wandered off the stack.  Stop rather than print 100 lies. */
        if (!is_kernel_text(ret_rip)) {
            kprintf_atomic("  #%d 0x%016lx <??? (non-text; chain broken)>\n",
                            depth, ret_rip);
            break;
        }
        print_frame(depth, ret_rip);
        /* Next frame must be higher on the stack than current one —
         * stacks grow down, so older frames have larger rbp. */
        if (next_rbp <= cur) break;
        cur = next_rbp;
    }
    kprintf_atomic("=== end backtrace ===\n");
}

void stackwalk_print(uint64_t rbp) {
    /* Frame 0: the caller of this function.  The caller's rip lives
     * at rbp+8 because our own prologue pushed the saved rbp and the
     * return address onto the stack. */
    uint64_t ret;
    if (!safe_read_u64(rbp + 8, &ret)) ret = 0;
    stackwalk_print_from(rbp, ret);
}
