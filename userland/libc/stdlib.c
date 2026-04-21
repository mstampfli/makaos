// ── stdlib.c — <stdlib.h> extern symbols for sysroot consumers ──────
//
// libc.h provides static-inline versions for in-tree apps; the sysroot
// split-header world needs real linkable symbols.  malloc/free/realloc,
// setenv/unsetenv, atoi/strtol/snprintf/asprintf/abort live in libc.c.
// exit/getenv/realpath/posix_memalign live here.

#include <stddef.h>
#include <errno.h>
#include <makaos/syscall.h>

extern char** environ;
extern void*  malloc(size_t);
extern size_t strlen(const char*);
extern void*  memcpy(void*, const void*, size_t);

// ── getenv ───────────────────────────────────────────────────────────
// Shares the `environ` table the loader populates.  Returns a pointer
// into one of the "KEY=VALUE" entries — caller must not free.
char* getenv(const char* name) {
    if (!environ || !name) return (char*)0;
    size_t nlen = 0; while (name[nlen]) nlen++;
    for (char** e = environ; *e; e++) {
        size_t klen = 0;
        while ((*e)[klen] && (*e)[klen] != '=') klen++;
        if ((*e)[klen] != '=') continue;
        if (klen != nlen) continue;
        int eq = 1;
        for (size_t i = 0; i < nlen; i++)
            if ((*e)[i] != name[i]) { eq = 0; break; }
        if (eq) return (*e) + nlen + 1;
    }
    return (char*)0;
}

// ── exit ─────────────────────────────────────────────────────────────
// Flush stdout/stderr before the kernel exit — in-tree apps that write
// through the stdio buffered path (puts/printf) would otherwise lose
// their tail on exit.  _flush_all lives in stdio.c.
extern void _flush_all(void);

__attribute__((noreturn))
void exit(int status) {
    _flush_all();
    syscall1(SYS_EXIT, (uint64_t)status);
    __builtin_unreachable();
}

// ── realpath ─────────────────────────────────────────────────────────
// Symlink-free filesystem: canonical path == input path.  If
// resolved_path is NULL, malloc a fresh buffer; otherwise write into
// caller's buffer (assumed PATH_MAX).
char* realpath(const char* path, char* resolved_path) {
    if (!path) { errno = EINVAL; return (char*)0; }
    size_t n = strlen(path);
    char* out = resolved_path ? resolved_path : (char*)malloc(n + 1);
    if (!out) { errno = ENOMEM; return (char*)0; }
    memcpy(out, path, n + 1);
    return out;
}

// ── posix_memalign ───────────────────────────────────────────────────
// libc's malloc returns 16-byte-aligned blocks.  Callers requesting
// alignment ≤ 16 get correct behaviour; > 16 is not supported yet.
// TODO(scalability-debt-ledger-#9): real aligned allocator up to page
// size — needed when the first port requests alignment > 16.
int posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (!memptr) return EINVAL;
    if (alignment == 0 || (alignment & (alignment - 1))) return EINVAL;
    *memptr = malloc(size);
    return *memptr ? 0 : ENOMEM;
}
