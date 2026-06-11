// ── sdl_port_stubs.c — extern symbols SDL3 + upstream ports want ────
//
// libc.h has most of these as `static inline` for in-tree apps, but
// sysroot-linked consumers (SDL3, wlroots, ports that compile with
// `-nostdinc -isystem $SYSROOT/usr/include`) see only the extern
// prototypes in userland/libc/include/*.h.  We gather the link-
// visible bodies here, compiled with SYSROOT_CFLAGS so every type
// (wchar_t, size_t, …) comes from the same header tree as its
// callers — mixing trees causes wchar.h redefinition storms.

#include <wchar.h>
#include <uchar.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <makaos/syscall.h>

size_t wcslen(const wchar_t* s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

int wcsncmp(const wchar_t* a, const wchar_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        if (!a[i])        return 0;
    }
    return 0;
}

int wcscmp(const wchar_t* a, const wchar_t* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(*a) - (int)(*b);
}

// Case-insensitive wide compare — only ASCII case folds; anything
// above 0x7F compares as-is (same policy as wctype.c).
static inline wchar_t _wc_tolower_ascii(wchar_t c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}
int wcscasecmp(const wchar_t* a, const wchar_t* b) {
    for (;;) {
        wchar_t ca = _wc_tolower_ascii(*a++);
        wchar_t cb = _wc_tolower_ascii(*b++);
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
}
int wcsncasecmp(const wchar_t* a, const wchar_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        wchar_t ca = _wc_tolower_ascii(a[i]);
        wchar_t cb = _wc_tolower_ascii(b[i]);
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
    return 0;
}

wchar_t* wcscat(wchar_t* dst, const wchar_t* src) {
    wchar_t* end = dst;
    while (*end) end++;
    while ((*end++ = *src++));
    return dst;
}
wchar_t* wcsncat(wchar_t* dst, const wchar_t* src, size_t n) {
    wchar_t* end = dst;
    while (*end) end++;
    size_t i = 0;
    while (i < n && src[i]) { *end++ = src[i++]; }
    *end = 0;
    return dst;
}

// strsignal + sysconf live in libc.c; do not duplicate here.

// epoll_pwait — sigmask-aware epoll_wait.  MakaOS has no per-thread
// sigmask yet; silently drop the mask and forward.  Foot's use is
// "block SIGCHLD briefly around the wait", which is harmless when
// the mask is global (we already block signals at sigaction time).
#include <sys/epoll.h>
int epoll_pwait(int epfd, struct epoll_event* evs, int max, int timeout,
                const sigset_t* sigmask) {
    (void)sigmask;
    return epoll_wait(epfd, evs, max, timeout);
}

// posix_openpt + friends — allocate a PTY master.  Forward to our
// /dev/ptmx path.
#include <fcntl.h>
extern int open(const char* path, int flags, ...);
int posix_openpt(int flags) {
    return open("/dev/ptmx", flags);
}
int grantpt(int fd)  { (void)fd; return 0; }       // no setuid helper needed
int unlockpt(int fd) { (void)fd; return 0; }       // always unlocked already
static char s_ptsname_buf[32];
extern int ioctl(int fd, unsigned long request, ...);
#define MAKAOS_TIOCGPTN 0x80045430UL
char* ptsname(int fd) {
    // Ask the kernel for the slave index.  Deriving it from the fd
    // number (the old shortcut) breaks the moment the master fd isn't
    // numerically equal to the pty index — which is always.
    unsigned n = 0;
    if (ioctl(fd, MAKAOS_TIOCGPTN, &n) != 0) return 0;
    int i = 0;
    const char* prefix = "/dev/pts/";
    while (prefix[i]) { s_ptsname_buf[i] = prefix[i]; i++; }
    if (n == 0) s_ptsname_buf[i++] = '0';
    else {
        char digits[16]; int d = 0;
        while (n) { digits[d++] = '0' + (n % 10); n /= 10; }
        while (d--) s_ptsname_buf[i++] = digits[d];
    }
    s_ptsname_buf[i] = '\0';
    return s_ptsname_buf;
}
int ptsname_r(int fd, char* buf, size_t len) {
    char* s = ptsname(fd);
    size_t n = 0;
    while (s[n] && n + 1 < len) { buf[n] = s[n]; n++; }
    buf[n] = '\0';
    return 0;
}

// Wide-character memory ops — foot's sixel renderer uses wmemset.
wchar_t* wmemset(wchar_t* s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) s[i] = c;
    return s;
}
wchar_t* wmemcpy(wchar_t* dst, const wchar_t* src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    return dst;
}
wchar_t* wmemmove(wchar_t* dst, const wchar_t* src, size_t n) {
    if (dst < src) {
        for (size_t i = 0; i < n; i++) dst[i] = src[i];
    } else {
        for (size_t i = n; i-- > 0;) dst[i] = src[i];
    }
    return dst;
}
int wmemcmp(const wchar_t* a, const wchar_t* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// confstr already lives in libc.c; header-only change added the
// _CS_PATH macro + extern decl so sysroot consumers see it.

// accept4 — accept + atomically apply SOCK_NONBLOCK / SOCK_CLOEXEC.
// Our SYS_ACCEPT doesn't carry flags yet; fall back to accept() +
// fcntl for the NONBLOCK bit.  CLOEXEC is a no-op on MakaOS (no
// exec FD inheritance path yet honours O_CLOEXEC).
extern int accept(int fd, void* addr, void* addrlen);
extern int fcntl(int fd, int cmd, ...);
int accept4(int fd, void* addr, void* addrlen, int flags) {
    int s = accept(fd, addr, addrlen);
    if (s < 0) return s;
    if (flags & 0x00800 /*SOCK_NONBLOCK*/) {
        int fl = fcntl(s, 3 /*F_GETFL*/);
        fcntl(s, 4 /*F_SETFL*/, fl | 00004000 /*O_NONBLOCK*/);
    }
    if (flags & 0x80000 /*SOCK_CLOEXEC*/) {
        fcntl(s, 2 /*F_SETFD*/, 1 /*FD_CLOEXEC*/);
    }
    return s;
}

// ── More extern impls for symbols only inline in libc.h ─────────────

// Wide string primitives (extern versions).
wchar_t* wcscpy(wchar_t* dst, const wchar_t* src) {
    wchar_t* d = dst;
    while ((*d++ = *src++));
    return dst;
}
wchar_t* wcsncpy(wchar_t* dst, const wchar_t* src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    while (i < n) { dst[i++] = 0; }
    return dst;
}
wchar_t* wcschr(const wchar_t* s, wchar_t c) {
    for (; *s; s++) if (*s == c) return (wchar_t*)s;
    return c == 0 ? (wchar_t*)s : (wchar_t*)0;
}

extern void* malloc(unsigned long);
wchar_t* wcsdup(const wchar_t* s) {
    size_t n = 0; while (s[n]) n++;
    wchar_t* r = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
    if (!r) return 0;
    for (size_t i = 0; i <= n; i++) r[i] = s[i];
    return r;
}

// UTF-8 ↔ UTF-32 conversion.  MakaOS's mbstate_t is stateless (int);
// we don't track partial sequences across calls (ports call these
// one byte at a time for incremental decode — each call sees the
// full code point's bytes in one go).  Good enough for foot.
size_t mbrtoc32(char32_t* pc32, const char* s, size_t n, mbstate_t* ps) {
    (void)ps;
    if (!s) return 0;
    if (n == 0) return (size_t)-2;
    unsigned char c = (unsigned char)s[0];
    char32_t r;
    size_t used;
    if (c < 0x80) { r = c; used = 1; }
    else if ((c & 0xE0) == 0xC0) {
        if (n < 2) return (size_t)-2;
        r = ((c & 0x1F) << 6) | (s[1] & 0x3F); used = 2;
    } else if ((c & 0xF0) == 0xE0) {
        if (n < 3) return (size_t)-2;
        r = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        used = 3;
    } else if ((c & 0xF8) == 0xF0) {
        if (n < 4) return (size_t)-2;
        r = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
            ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        used = 4;
    } else { errno = EILSEQ; return (size_t)-1; }
    if (pc32) *pc32 = r;
    return r == 0 ? 0 : used;
}
size_t c32rtomb(char* s, char32_t c32, mbstate_t* ps) {
    (void)ps;
    if (!s) return 1;
    if (c32 < 0x80) { s[0] = (char)c32; return 1; }
    if (c32 < 0x800) {
        s[0] = (char)(0xC0 | (c32 >> 6));
        s[1] = (char)(0x80 | (c32 & 0x3F));
        return 2;
    }
    if (c32 < 0x10000) {
        s[0] = (char)(0xE0 | (c32 >> 12));
        s[1] = (char)(0x80 | ((c32 >> 6) & 0x3F));
        s[2] = (char)(0x80 | (c32 & 0x3F));
        return 3;
    }
    s[0] = (char)(0xF0 | (c32 >> 18));
    s[1] = (char)(0x80 | ((c32 >> 12) & 0x3F));
    s[2] = (char)(0x80 | ((c32 >> 6)  & 0x3F));
    s[3] = (char)(0x80 | (c32 & 0x3F));
    return 4;
}

// strtof — thin wrapper around strtod.  Hot path is fontconfig's
// font-weight/size parsing.
extern double strtod(const char* s, char** end);
float strtof(const char* s, char** end) { return (float)strtod(s, end); }

// uname — return fixed MakaOS identity.  foot uses it to populate
// $TERM_PROGRAM_VERSION-like variables; any plausible value works.
struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65];
};
int uname(struct utsname* u) {
    if (!u) { errno = EFAULT; return -1; }
    static const char* val[5] = { "MakaOS", "makaos", "1.0", "MakaOS 1.0", "x86_64" };
    char* dst[5] = { u->sysname, u->nodename, u->release, u->version, u->machine };
    for (int f = 0; f < 5; f++) {
        int i = 0; while (val[f][i] && i < 64) { dst[f][i] = val[f][i]; i++; }
        dst[f][i] = 0;
    }
    return 0;
}

// chdir / execve / _exit — direct syscall wrappers.  Inline versions
// live in libc.h for in-tree apps; sysroot consumers see only the
// extern declarations, so we provide link-visible bodies here.
// makaos/syscall.h already declares syscall[0-6] with uint64_t args.
int chdir(const char* path) {
    size_t n = 0; while (path && path[n]) n++;
    long r = syscall2(SYS_CHDIR, (uint64_t)path, (uint64_t)n);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}
// execve moved to unistd.c (passes full argv/envp through
// kernel SYS_EXEC which takes all three args).
__attribute__((__noreturn__))
void _exit(int status) {
    syscall1(SYS_EXIT, (uint64_t)status);
    for (;;);
}

// ── fenv stubs ───────────────────────────────────────────────────────
// Default rounding mode is all we ever operate in.  foot's box-
// drawing routines call feclearexcept + fetestexcept defensively;
// reporting "no exceptions" is correct behaviour.
int feclearexcept(int excepts)  { (void)excepts; return 0; }
int feraiseexcept(int excepts)  { (void)excepts; return 0; }
int fetestexcept(int excepts)   { (void)excepts; return 0; }
int fegetround(void)            { return 0 /*FE_TONEAREST*/; }
int fesetround(int round)       { (void)round; return 0; }

// iconv — MakaOS has no locale / charset conversion database yet.
// iconv_open reports unsupported so callers fall back to UTF-8
// pass-through (SDL3's string code already does this).
typedef void* iconv_t;
iconv_t iconv_open(const char* to, const char* from) {
    (void)to; (void)from; errno = EINVAL; return (iconv_t)-1;
}
size_t iconv(iconv_t cd, char** in, size_t* inleft,
             char** out, size_t* outleft) {
    (void)cd; (void)in; (void)inleft; (void)out; (void)outleft;
    errno = EINVAL; return (size_t)-1;
}
int iconv_close(iconv_t cd) { (void)cd; return 0; }

// _Exit — skips atexit handlers and stdio flushes; straight syscall.
__attribute__((noreturn))
void _Exit(int status) {
    syscall1(SYS_EXIT, (uint64_t)status);
    for (;;);
}

// ── OpenBSD-style bounded strings (strlcpy / strlcat) ───────────────
// SDL3's SDL_string.c uses both.  Return the total length of the
// string they *tried* to create, like the BSD originals, so callers
// can detect truncation via `ret >= dstsize`.
size_t strlcpy(char* dst, const char* src, size_t dstsize) {
    size_t slen = 0;
    while (src[slen]) slen++;
    if (dstsize) {
        size_t copy = slen < dstsize - 1 ? slen : dstsize - 1;
        for (size_t i = 0; i < copy; i++) dst[i] = src[i];
        dst[copy] = '\0';
    }
    return slen;
}

size_t strlcat(char* dst, const char* src, size_t dstsize) {
    size_t dlen = 0;
    while (dlen < dstsize && dst[dlen]) dlen++;
    size_t slen = 0;
    while (src[slen]) slen++;
    if (dlen == dstsize) return dstsize + slen;
    size_t avail = dstsize - dlen - 1;
    size_t copy  = slen < avail ? slen : avail;
    for (size_t i = 0; i < copy; i++) dst[dlen + i] = src[i];
    dst[dlen + copy] = '\0';
    return dlen + slen;
}
