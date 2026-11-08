#ifndef _MAKAOS_EXTABLE_H
#define _MAKAOS_EXTABLE_H

#include "common.h"    // uint64_t (freestanding kernel typedefs)

// ── Kernel fault-fixup (exception) table ──────────────────────────────────
//
// The kernel occasionally touches user memory directly: copying syscall
// buffers, and building signal frames on the user stack.  A user pointer that
// is bad, or a page whose VMA is unmapped by a racing munmap/exit between the
// access check and the access itself, makes that kernel-mode access #PF at an
// address the page-fault handler cannot resolve.  Without recovery that is a
// "kernel #PF (unrecoverable)" panic -- i.e. userland can take down the whole
// system (a trivial DoS, and the crash that killed signal delivery during
// process teardown).
//
// The fix is the same mechanism Linux uses: every instruction that may fault on
// a user address is paired, at build time, with a "fixup" landing pad in a
// dedicated __ex_table section.  When a kernel-mode fault is otherwise
// unrecoverable, the page-fault handler looks the faulting RIP up here; a hit
// means "this was a sanctioned user access -- resume at the fixup, which
// returns -EFAULT" instead of panicking.  A miss means a genuine kernel bug
// (wild pointer with no fixup) and the panic stands.

typedef struct exentry {
    uint64_t insn;     // faulting instruction address (absolute kernel VA)
    uint64_t fixup;    // recovery landing pad         (absolute kernel VA)
} exentry_t;

// Emit an __ex_table entry pairing a labeled faulting instruction with its
// fixup landing pad.  Used inside an inline-asm template (see __uaccess_copy).
// The section is allocated read-only and folded into .rodata by the linker
// script, so it needs no separate objcopy -j flag.
#define _ASM_EXTABLE(insn_label, fixup_label)          \
    ".pushsection __ex_table, \"a\", @progbits\n\t"     \
    ".balign 8\n\t"                                     \
    ".quad " #insn_label "\n\t"                         \
    ".quad " #fixup_label "\n\t"                        \
    ".popsection\n\t"

// Look up `rip` in the fault-fixup table.  Returns the fixup address to jump
// to, or 0 if `rip` is not a registered user-access instruction (a genuine
// kernel bug -> the caller should panic).
uint64_t extable_fixup(uint64_t rip);

#endif // _MAKAOS_EXTABLE_H
