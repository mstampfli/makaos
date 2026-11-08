// ── Kernel fault-fixup (exception) table lookup ───────────────────────────
//
// See extable.h for the theory of operation.  The table is populated at build
// time by _ASM_EXTABLE entries (one per user-access primitive) and delimited by
// the linker-script symbols below.

#include "extable.h"

// Provided by kernel/link.ld (bracketing the __ex_table input section folded
// into .rodata).
extern exentry_t __start___ex_table[];
extern exentry_t __stop___ex_table[];

// Linear scan.  The table holds one entry per user-access primitive (a
// handful), and extable_fixup runs only on the would-be-panic path -- an
// unresolvable kernel-mode fault, which is rare -- so O(n) over a tiny
// read-only table is the right call; a sorted copy would have to live in
// writable memory for no benefit on this cold path.
uint64_t extable_fixup(uint64_t rip) {
    for (const exentry_t* e = __start___ex_table; e < __stop___ex_table; e++)
        if (e->insn == rip)
            return e->fixup;
    return 0;
}
