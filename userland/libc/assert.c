// ── assert.c — POSIX/glibc-style assertion failure handler ──────────
//
// The <assert.h> macro expands to `__assert_fail(...)` which we
// implement here.  Prints a line to stderr and exits with 134.

#include <stddef.h>
#include <unistd.h>
#include <makaos/syscall.h>

void __assert_fail(const char* expr, const char* file,
                     unsigned int line, const char* func) {
    (void)line; (void)func;
    const char prefix[] = "assert failed: ";
    const char between[] = " at ";
    const char nl[] = "\n";
    write(2, prefix, sizeof(prefix) - 1);
    if (expr) { size_t n = 0; while (expr[n]) n++; write(2, expr, n); }
    write(2, between, sizeof(between) - 1);
    if (file) { size_t n = 0; while (file[n]) n++; write(2, file, n); }
    write(2, nl, 1);
    syscall1(SYS_EXIT, 134);
    __builtin_unreachable();
}
