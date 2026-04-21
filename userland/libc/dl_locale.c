// ── dl_locale.c — external-symbol stubs for <dlfcn.h>, <locale.h>, <langinfo.h>
//
// MakaOS statically links every binary.  The dlopen family and locale
// APIs exist only so ports (mbedTLS, fontconfig, harfbuzz, curl, ICU,
// eventually libwayland) compiled against the Linux-style split
// headers can resolve their imports at link time.  All of these
// return a safe, canonical "C / UTF-8" answer.
//
// libc.h also defines these as `static inline` for legacy in-tree
// apps that include libc.h directly.  Static-inline does not emit an
// external symbol, so these .c definitions win for sysroot consumers
// and both paths coexist without conflict.

#include <stddef.h>
#include <errno.h>
#include <dlfcn.h>
#include <locale.h>
#include <langinfo.h>
#include <makaos/syscall.h>

// ── dlfcn ────────────────────────────────────────────────────────────
// All operations fail predictably: dlopen returns NULL and dlerror
// returns a fixed diagnostic.  Ports that probe via dlopen degrade
// gracefully (e.g. mbedTLS's optional PKCS11, fontconfig's optional
// language probes) because NULL means "not available".

static const char s_dl_msg[] = "dlopen: dynamic loading is not supported on MakaOS";

void* dlopen(const char* file, int flags) {
    (void)file; (void)flags;
    return NULL;
}

void* dlsym(void* handle, const char* name) {
    (void)handle; (void)name;
    return NULL;
}

int dlclose(void* handle) { (void)handle; return 0; }

char* dlerror(void) { return (char*)s_dl_msg; }

int dladdr(const void* addr, Dl_info* info) {
    (void)addr;
    if (info) {
        info->dli_fname = NULL;
        info->dli_fbase = NULL;
        info->dli_sname = NULL;
        info->dli_saddr = NULL;
    }
    return 0;
}

// ── locale ───────────────────────────────────────────────────────────
// Canonical name returned by setlocale — matches what glibc returns
// for the plain C locale with UTF-8 ctype.

static const char s_locale_name[] = "C.UTF-8";

// Conservative struct lconv — numeric fields point to empty strings
// except decimal_point / thousands_sep which are set per POSIX C locale.
static char s_dp[]  = ".";
static char s_ts[]  = "";
static char s_gg[]  = "";
static char s_sgn[] = "";

static struct lconv s_lconv = {
    .decimal_point       = s_dp,
    .thousands_sep       = s_ts,
    .grouping            = s_gg,
    .int_curr_symbol     = s_ts,
    .currency_symbol     = s_ts,
    .mon_decimal_point   = s_ts,
    .mon_thousands_sep   = s_ts,
    .mon_grouping        = s_gg,
    .positive_sign       = s_sgn,
    .negative_sign       = s_sgn,
    .int_frac_digits     = (char)127,
    .frac_digits         = (char)127,
    .p_cs_precedes       = (char)127,
    .p_sep_by_space      = (char)127,
    .n_cs_precedes       = (char)127,
    .n_sep_by_space      = (char)127,
    .p_sign_posn         = (char)127,
    .n_sign_posn         = (char)127,
    .int_p_cs_precedes   = (char)127,
    .int_p_sep_by_space  = (char)127,
    .int_n_cs_precedes   = (char)127,
    .int_n_sep_by_space  = (char)127,
    .int_p_sign_posn     = (char)127,
    .int_n_sign_posn     = (char)127,
};

char* setlocale(int category, const char* locale) {
    (void)category; (void)locale;
    return (char*)s_locale_name;
}

struct lconv* localeconv(void) { return &s_lconv; }

locale_t newlocale(int mask, const char* locale, locale_t base) {
    (void)mask; (void)locale; (void)base;
    return (locale_t)1;   // opaque, non-NULL handle
}

locale_t duplocale(locale_t loc) { return loc ? loc : (locale_t)1; }

void freelocale(locale_t loc) { (void)loc; }

locale_t uselocale(locale_t loc) { (void)loc; return LC_GLOBAL_LOCALE; }

// ── langinfo ─────────────────────────────────────────────────────────
// Minimal but honest: CODESET = UTF-8, everything else = C locale.
static const char s_utf8[]        = "UTF-8";
static const char s_empty[]       = "";
static const char s_yesexpr[]     = "^[yY]";
static const char s_noexpr[]      = "^[nN]";
static const char s_radix_point[] = ".";
static const char s_am[]          = "AM";
static const char s_pm[]          = "PM";
static const char s_d_t_fmt[]     = "%a %b %e %H:%M:%S %Y";
static const char s_d_fmt[]       = "%m/%d/%y";
static const char s_t_fmt[]       = "%H:%M:%S";
static const char s_t_fmt_ampm[]  = "%I:%M:%S %p";

char* nl_langinfo(nl_item item) {
    switch (item) {
    case CODESET:    return (char*)s_utf8;
    case RADIXCHAR:  return (char*)s_radix_point;
    case THOUSEP:    return (char*)s_empty;
    case YESEXPR:    return (char*)s_yesexpr;
    case NOEXPR:     return (char*)s_noexpr;
    case AM_STR:     return (char*)s_am;
    case PM_STR:     return (char*)s_pm;
    case D_T_FMT:    return (char*)s_d_t_fmt;
    case D_FMT:      return (char*)s_d_fmt;
    case T_FMT:      return (char*)s_t_fmt;
    case T_FMT_AMPM: return (char*)s_t_fmt_ampm;
    default:         return (char*)s_empty;
    }
}

// ── getenv (real external symbol) ────────────────────────────────────
// libc.h has a static inline that works for in-tree apps; ported
// libraries via the split headers need a linkable symbol.  Shares
// the same `environ` table the loader populates.
extern char** environ;

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

// ── gettimeofday ─────────────────────────────────────────────────────
// POSIX.  expat, libffi, curl, many others call it.  MakaOS has only
// a monotonic nanosecond clock via SYS_CLOCK_NS; we return that as
// both sec + usec.  Matches enough of POSIX semantics for these
// libraries (none depend on wall-clock accuracy).
struct __k_timeval { long tv_sec; long tv_usec; };

int gettimeofday(struct __k_timeval* tv, void* tz) {
    (void)tz;
    if (!tv) { errno = EINVAL; return -1; }
    uint64_t ns = syscall0(SYS_CLOCK_NS);
    tv->tv_sec  = (long)(ns / 1000000000ull);
    tv->tv_usec = (long)((ns % 1000000000ull) / 1000ull);
    return 0;
}

// ── __assert_fail (glibc-style assert stub) ──────────────────────────
// expat + many others call this when compiled with NDEBUG undefined.
// We print via write(2) to stderr + exit — matches glibc behaviour.
extern ssize_t write(int fd, const void* buf, size_t n);

void __assert_fail(const char* expr, const char* file,
                     unsigned int line, const char* func) {
    (void)line; (void)func;
    const char prefix[] = "assert failed: ";
    const char nl[]     = "\n";
    write(2, prefix, sizeof(prefix) - 1);
    if (expr) { size_t n = 0; while (expr[n]) n++; write(2, expr, n); }
    write(2, " at ", 4);
    if (file) { size_t n = 0; while (file[n]) n++; write(2, file, n); }
    write(2, nl, 1);
    // Direct SYS_EXIT — libc.h's static-inline exit isn't visible here.
    syscall1(SYS_EXIT, 134);
    __builtin_unreachable();
}

// Real extern exit() for sysroot consumers.  abort() and _exit()
// already exist elsewhere in libc.c — don't redefine here.
__attribute__((noreturn))
void exit(int status) {
    syscall1(SYS_EXIT, (uint64_t)status);
    __builtin_unreachable();
}

// ── Port-support stubs (libdrm, libinput, friends) ───────────────────

int getpagesize(void) { return 4096; }

int mknod(const char* path, unsigned mode, unsigned dev) {
    (void)path; (void)mode; (void)dev;
    errno = ENOSYS;
    return -1;
}

// Symlink-free filesystem: canonical path == input path.  Callers
// typically pass resolved_path as NULL asking for malloc'd result,
// or as a PATH_MAX buffer.  Honour both.
extern void* malloc(size_t);
extern size_t strlen(const char*);
extern void*  memcpy(void*, const void*, size_t);

char* realpath(const char* path, char* resolved_path) {
    if (!path) { errno = EINVAL; return (char*)0; }
    size_t n = strlen(path);
    char* out = resolved_path ? resolved_path : (char*)malloc(n + 1);
    if (!out) { errno = ENOMEM; return (char*)0; }
    memcpy(out, path, n + 1);
    return out;
}

// aligned_alloc-style — our malloc is 16-byte aligned always, so we
// just forward.  For alignment > 16 callers would need a bigger
// buffer + manual alignment; libdrm/libinput request 8/16 so OK.
int posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (!memptr) return EINVAL;
    if (alignment & (alignment - 1)) return EINVAL;   // must be pow2
    if (alignment == 0)              return EINVAL;
    *memptr = malloc(size);
    return *memptr ? 0 : ENOMEM;
}

// open_memstream + fmemopen: not implemented (need an in-memory
// FILE* backend).  libdrm uses open_memstream for debug-log
// buffering; harfbuzz/others use fmemopen for in-memory font
// loading.  Return NULL → callers fall back.
#include <stdio.h>
FILE* open_memstream(char** bufp, size_t* sizep) {
    (void)bufp; (void)sizep;
    errno = ENOSYS;
    return (FILE*)0;
}
FILE* fmemopen(void* buf, size_t size, const char* mode) {
    (void)buf; (void)size; (void)mode;
    errno = ENOSYS;
    return (FILE*)0;
}

// asprintf stub — returns -1, leaves *strp unset.  Real impl would
// need a pre-probe vsnprintf(NULL) pass.  Not needed by our current
// ports' happy paths.
int asprintf(char** strp, const char* fmt, ...) {
    (void)strp; (void)fmt;
    return -1;
}
