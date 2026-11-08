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

// ── atexit / exit ────────────────────────────────────────────────────
// Fixed handler table, LIFO execution per POSIX.  32 slots matches
// glibc's minimum guarantee; glib registers a small handful.
#define ATEXIT_MAX 32
static void (*s_atexit_fns[ATEXIT_MAX])(void);
static int  s_atexit_count = 0;

int atexit(void (*fn)(void)) {
    if (!fn || s_atexit_count >= ATEXIT_MAX) return -1;
    s_atexit_fns[s_atexit_count++] = fn;
    return 0;
}

// ── C++ static/global destructor registry (Itanium C++ ABI) ──────────────
// libstdc++ and every C++ TU with a global/static object register their
// destructors via __cxa_atexit(fn, arg, dso), tagged with &__dso_handle.  A
// static executable is a single "DSO", so __dso_handle is one null-ish handle.
// The list grows on demand; __cxa_finalize(0) runs it in reverse at exit().
void* __dso_handle = 0;

extern void* realloc(void* ptr, size_t new_size);   // declared in libc.h (not included here)

struct __cxa_fn { void (*fn)(void*); void* arg; void* dso; };
static struct __cxa_fn* s_cxa_fns;
static size_t s_cxa_count, s_cxa_cap;

int __cxa_atexit(void (*fn)(void*), void* arg, void* dso) {
    if (!fn) return -1;
    if (s_cxa_count == s_cxa_cap) {
        size_t nc = s_cxa_cap ? s_cxa_cap * 2 : 32;
        struct __cxa_fn* np = realloc(s_cxa_fns, nc * sizeof(*np));
        if (!np) return -1;
        s_cxa_fns = np; s_cxa_cap = nc;
    }
    s_cxa_fns[s_cxa_count].fn  = fn;
    s_cxa_fns[s_cxa_count].arg = arg;
    s_cxa_fns[s_cxa_count].dso = dso;
    s_cxa_count++;
    return 0;
}

// Run registered destructors in reverse order.  dso==NULL => run all (exit);
// otherwise only those tagged for that DSO.  Each runs at most once.
void __cxa_finalize(void* dso) {
    for (size_t i = s_cxa_count; i-- > 0; ) {
        void (*fn)(void*) = s_cxa_fns[i].fn;
        if (fn && (dso == 0 || s_cxa_fns[i].dso == dso)) {
            s_cxa_fns[i].fn = 0;   // clear before calling: no re-entry / double-run
            fn(s_cxa_fns[i].arg);
        }
    }
}

// Flush stdout/stderr before the kernel exit — in-tree apps that write
// through the stdio buffered path (puts/printf) would otherwise lose
// their tail on exit.  _flush_all lives in stdio.c.
extern void _flush_all(void);

extern void __cxa_finalize(void* dso);

__attribute__((noreturn))
void exit(int status) {
    __cxa_finalize(0);           // run C++ global/static destructors first
    while (s_atexit_count > 0)
        s_atexit_fns[--s_atexit_count]();
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

// ── bsearch — binary search over a sorted array ──────────────────────
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size,
              int (*cmp)(const void*, const void*)) {
    const unsigned char* a = (const unsigned char*)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void* elt = a + mid * size;
        int c = cmp(key, elt);
        if (c < 0)       hi = mid;
        else if (c > 0)  lo = mid + 1;
        else             return (void*)elt;
    }
    return (void*)0;
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

// ── memalign ─────────────────────────────────────────────────────────
// Legacy glibc aligned allocator (<malloc.h>).  Shares posix_memalign's
// current alignment<=16 limitation (scalability-debt-ledger-#9).  Returns
// NULL on failure rather than an errno, per the glibc contract.
void* memalign(size_t alignment, size_t size) {
    void* p = 0;
    if (posix_memalign(&p, alignment, size) != 0) return 0;
    return p;
}
