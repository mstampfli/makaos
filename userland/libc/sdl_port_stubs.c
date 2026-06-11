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
#include <sys/socket.h>
extern int fcntl(int fd, int cmd, ...);
int accept4(int fd, struct sockaddr* addr, socklen_t* addrlen, int flags) {
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
// pass-through (SDL3's string code already does this).  WEAK: ports
// that link the real port stub (-liconv from port-libiconv.sh, which
// glib does) get that strong definition instead of these — both
// archives on one link line is otherwise a duplicate-symbol error.
typedef void* iconv_t;
__attribute__((weak)) iconv_t iconv_open(const char* to, const char* from) {
    (void)to; (void)from; errno = EINVAL; return (iconv_t)-1;
}
__attribute__((weak)) size_t iconv(iconv_t cd, char** in, size_t* inleft,
             char** out, size_t* outleft) {
    (void)cd; (void)in; (void)inleft; (void)out; (void)outleft;
    errno = EINVAL; return (size_t)-1;
}
__attribute__((weak)) int iconv_close(iconv_t cd) { (void)cd; return 0; }

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

// ── Wide-char collation ("C" locale) ─────────────────────────────────
// glib's gunicollate.c uses wcsxfrm/wcscoll for locale-aware sort keys.
// MakaOS only ships the C locale, where the transform is the identity:
// copy the string, return its length; collation is code-point compare.
size_t wcsxfrm(wchar_t* dst, const wchar_t* src, size_t n) {
    size_t len = wcslen(src);
    if (n) {
        size_t copy = len < n - 1 ? len : n - 1;
        for (size_t i = 0; i < copy; i++) dst[i] = src[i];
        dst[copy] = L'\0';
    }
    return len;
}

int wcscoll(const wchar_t* a, const wchar_t* b) {
    return wcscmp(a, b);
}

// ── IPv6 well-known addresses + interface naming ─────────────────────
// Declared in <netinet/in.h> / <net/if.h>; gio links them.  MakaOS is
// IPv4-only with a single fixed interface.
#include <netinet/in.h>
#include <net/if.h>
#include <string.h>
const struct in6_addr in6addr_any      = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

unsigned int if_nametoindex(const char* name) {
    return (name && strcmp(name, "eth0") == 0) ? 1u : 0u;
}
char* if_indextoname(unsigned int index, char* name) {
    if (index != 1 || !name) return 0;
    strcpy(name, "eth0");
    return name;
}

// ── getservbyname / getservbyport ────────────────────────────────────
// No /etc/services database; gio falls back to numeric ports on NULL.
#include <netdb.h>
struct servent* getservbyname(const char* name, const char* proto) {
    (void)name; (void)proto;
    return 0;
}
struct servent* getservbyport(int port, const char* proto) {
    (void)port; (void)proto;
    return 0;
}

// inet_aton — legacy IPv4 parser; wraps inet_pton (no hex/octal forms).
#include <arpa/inet.h>
int inet_aton(const char* s, struct in_addr* out) {
    return inet_pton(2 /*AF_INET*/, s, out) == 1;
}

// getsockname / getpeername — the kernel keeps no per-socket local
// address record yet; gio callers degrade to "address unavailable".
int getsockname(int fd, struct sockaddr* addr, socklen_t* len) {
    (void)fd; (void)addr; (void)len;
    errno = ENOTSUP;
    return -1;
}
int getpeername(int fd, struct sockaddr* addr, socklen_t* len) {
    (void)fd; (void)addr; (void)len;
    errno = ENOTSUP;
    return -1;
}

// ── Resolver compat stubs ────────────────────────────────────────────
// res_query (libc/resolv.c) always fails on MakaOS, so gio's DNS
// answer parsing never executes; these exist for the linker.
#include <netdb.h>
#include <resolv.h>
int h_errno = 0;
int dn_expand(const unsigned char* msg, const unsigned char* eom,
              const unsigned char* src, char* dst, int dstsiz) {
    (void)msg; (void)eom; (void)src; (void)dst; (void)dstsiz;
    return -1;
}
int getnameinfo(const struct sockaddr* sa, socklen_t salen,
                char* host, socklen_t hostlen,
                char* serv, socklen_t servlen, int flags) {
    (void)sa; (void)salen; (void)host; (void)hostlen;
    (void)serv; (void)servlen; (void)flags;
    return EAI_FAIL;
}

// ── mntent — empty mount table ───────────────────────────────────────
#include <mntent.h>
FILE* setmntent(const char* filename, const char* type) {
    (void)filename; (void)type;
    return 0;            // no mount table — callers see "no mounts"
}
struct mntent* getmntent(FILE* fp) { (void)fp; return 0; }
struct mntent* getmntent_r(FILE* fp, struct mntent* out,
                           char* buf, int buflen) {
    (void)fp; (void)out; (void)buf; (void)buflen;
    return 0;
}
int addmntent(FILE* fp, const struct mntent* mnt) { (void)fp; (void)mnt; return 1; }
int endmntent(FILE* fp) { (void)fp; return 1; }
char* hasmntopt(const struct mntent* mnt, const char* opt) {
    (void)mnt; (void)opt;
    return 0;
}

// ── grp.h — no /etc/group database ───────────────────────────────────
#include <grp.h>
struct group* getgrnam(const char* name) { (void)name; return 0; }
struct group* getgrgid(gid_t gid)        { (void)gid;  return 0; }
int getgrnam_r(const char* name, struct group* grp,
               char* buf, size_t buflen, struct group** result) {
    (void)name; (void)grp; (void)buf; (void)buflen;
    if (result) *result = 0;
    return 0;   // "not found" — POSIX: 0 with *result NULL
}
int getgrgid_r(gid_t gid, struct group* grp,
               char* buf, size_t buflen, struct group** result) {
    (void)gid; (void)grp; (void)buf; (void)buflen;
    if (result) *result = 0;
    return 0;
}

// ── statvfs — no fs-stats syscall yet ───────────────────────────────
#include <sys/statvfs.h>
int statvfs(const char* path, struct statvfs* buf) {
    (void)path; (void)buf;
    errno = ENOSYS;
    return -1;
}
int fstatvfs(int fd, struct statvfs* buf) {
    (void)fd; (void)buf;
    errno = ENOSYS;
    return -1;
}

// ── raise / atol ─────────────────────────────────────────────────────
extern int kill(int pid, int sig);
extern int getpid(void);
int raise(int sig) {
    return kill(getpid(), sig);
}
extern long strtol(const char* s, char** endptr, int base);
long atol(const char* s) {
    return strtol(s, 0, 10);
}

// ── termios — wrap the tty ioctls ────────────────────────────────────
#include <termios.h>
int tcgetattr(int fd, struct termios* t) {
    return ioctl(fd, 0x5401 /*TCGETS*/, t);
}
int tcsetattr(int fd, int actions, const struct termios* t) {
    // TCSANOW/DRAIN/FLUSH map to TCSETS/TCSETSW/TCSETSF.
    unsigned long req = 0x5402 + (actions > 2 ? 0 : (unsigned long)actions);
    return ioctl(fd, req, t);
}

// ── getaddrinfo (IPv4 only) ──────────────────────────────────────────
// Numeric addresses + the native resolver in dns.c.  One result per
// query; service strings must be numeric (no /etc/services).
extern int gethostbyname_ipv4(const char* name, unsigned int* out_ip_be);
extern void* calloc(unsigned long, unsigned long);
extern void free(void*);
extern int atoi(const char*);
int getaddrinfo(const char* node, const char* svc,
                const struct addrinfo* hints, struct addrinfo** res) {
    if (!res) return EAI_FAIL;
    *res = 0;
    if (!node) return EAI_NONAME;

    unsigned int ip_be = 0;
    struct in_addr parsed;
    if (inet_aton(node, &parsed)) {
        ip_be = parsed.s_addr;
    } else {
        if (hints && (hints->ai_flags & AI_NUMERICHOST)) return EAI_NONAME;
        if (gethostbyname_ipv4(node, &ip_be) != 0) return EAI_NONAME;
    }

    struct addrinfo* ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
    if (!ai) return EAI_MEMORY;
    struct sockaddr_in* sa = (struct sockaddr_in*)(ai + 1);
    sa->sin_family      = 2 /*AF_INET*/;
    sa->sin_port        = htons(svc ? (unsigned short)atoi(svc) : 0);
    sa->sin_addr.s_addr = ip_be;
    ai->ai_family   = 2;
    ai->ai_socktype = hints ? hints->ai_socktype : 0;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen  = sizeof(*sa);
    ai->ai_addr     = (struct sockaddr*)sa;
    *res = ai;
    return 0;
}
void freeaddrinfo(struct addrinfo* ai) {
    while (ai) {
        struct addrinfo* next = ai->ai_next;
        free(ai);   // sockaddr is co-allocated
        ai = next;
    }
}
const char* gai_strerror(int err) {
    switch (err) {
    case EAI_NONAME: return "name or service not known";
    case EAI_AGAIN:  return "temporary failure in name resolution";
    case EAI_MEMORY: return "out of memory";
    default:         return "address resolution error";
    }
}

// ── getpwnam_r / getpwuid_r ──────────────────────────────────────────
// _r variants over the static-buffer getpwnam/getpwuid in libc.c.
// POSIX: "not found" is return 0 with *result = NULL.
#include <pwd.h>
int getpwnam_r(const char* name, struct passwd* pw,
               char* buf, size_t buflen, struct passwd** result) {
    (void)buf; (void)buflen;
    struct passwd* p = getpwnam(name);
    if (result) *result = 0;
    if (!p) return 0;
    *pw = *p;
    if (result) *result = pw;
    return 0;
}
int getpwuid_r(uid_t uid, struct passwd* pw,
               char* buf, size_t buflen, struct passwd** result) {
    (void)buf; (void)buflen;
    struct passwd* p = getpwuid(uid);
    if (result) *result = 0;
    if (!p) return 0;
    *pw = *p;
    if (result) *result = pw;
    return 0;
}

// ── wctomb — UTF-8 encode one wide character ─────────────────────────
int wctomb(char* s, wchar_t wc) {
    if (!s) return 0;            // stateless encoding
    unsigned int c = (unsigned int)wc;
    if (c < 0x80)        { s[0] = (char)c; return 1; }
    if (c < 0x800) {
        s[0] = (char)(0xC0 | (c >> 6));
        s[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        s[0] = (char)(0xE0 | (c >> 12));
        s[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        s[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    if (c < 0x110000) {
        s[0] = (char)(0xF0 | (c >> 18));
        s[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        s[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        s[3] = (char)(0x80 | (c & 0x3F));
        return 4;
    }
    return -1;
}

// ── creat ────────────────────────────────────────────────────────────
int creat(const char* path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

// ── setrlimit — accepted, unenforced (matches the getrlimit stub) ────
struct rlimit;
int setrlimit(int resource, const struct rlimit* rlim) {
    (void)resource; (void)rlim;
    return 0;
}

// ── mktime — civil time (assumed UTC; no timezone db) → epoch ────────
#include <time.h>
static int mk_is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}
time_t mktime(struct tm* tm) {
    static const int mdays[12] =
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (!tm) return (time_t)-1;
    long days = 0;
    int year = tm->tm_year + 1900;
    for (int y = 1970; y < year; y++) days += mk_is_leap(y) ? 366 : 365;
    for (int m = 0; m < tm->tm_mon && m < 12; m++) {
        days += mdays[m];
        if (m == 1 && mk_is_leap(year)) days++;
    }
    days += tm->tm_mday - 1;
    return ((time_t)days * 86400) + tm->tm_hour * 3600
         + tm->tm_min * 60 + tm->tm_sec;
}

// ── ctime / asctime — declared in <time.h>, bodies were missing ──────
extern struct tm* localtime(const time_t* t);
static char s_asctime_buf[26];
char* asctime(const struct tm* tm) {
    static const char wd[7][4]  = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char mo[12][4] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    if (!tm) return 0;
    int wday = (tm->tm_wday >= 0 && tm->tm_wday < 7)  ? tm->tm_wday : 0;
    int mon  = (tm->tm_mon  >= 0 && tm->tm_mon  < 12) ? tm->tm_mon  : 0;
    // "Www Mmm dd hh:mm:ss yyyy\n" — fixed 26-byte POSIX layout.
    char* p = s_asctime_buf;
    for (int i = 0; i < 3; i++) *p++ = wd[wday][i];
    *p++ = ' ';
    for (int i = 0; i < 3; i++) *p++ = mo[mon][i];
    *p++ = ' ';
    *p++ = (char)('0' + tm->tm_mday / 10);
    *p++ = (char)('0' + tm->tm_mday % 10);
    *p++ = ' ';
    *p++ = (char)('0' + tm->tm_hour / 10);
    *p++ = (char)('0' + tm->tm_hour % 10);
    *p++ = ':';
    *p++ = (char)('0' + tm->tm_min / 10);
    *p++ = (char)('0' + tm->tm_min % 10);
    *p++ = ':';
    *p++ = (char)('0' + tm->tm_sec / 10);
    *p++ = (char)('0' + tm->tm_sec % 10);
    *p++ = ' ';
    int y = tm->tm_year + 1900;
    *p++ = (char)('0' + (y / 1000) % 10);
    *p++ = (char)('0' + (y / 100) % 10);
    *p++ = (char)('0' + (y / 10) % 10);
    *p++ = (char)('0' + y % 10);
    *p++ = '\n';
    *p = '\0';
    return s_asctime_buf;
}
char* ctime(const time_t* t) {
    return asctime(localtime(t));
}

// difftime — both time_t values are seconds; the difference is exact.
double difftime(time_t end, time_t start) {
    return (double)(end - start);
}

// ── Unlocked stdio ───────────────────────────────────────────────────
// MakaOS stdio has no internal FILE lock (single-threaded streams);
// the unlocked variants alias the locked ones.
#include <stdio.h>
void flockfile(FILE* f)   { (void)f; }
void funlockfile(FILE* f) { (void)f; }
int  ftrylockfile(FILE* f){ (void)f; return 0; }
extern int fgetc(FILE* f);
int  getc_unlocked(FILE* f) { return fgetc(f); }
extern int fputc(int c, FILE* f);
int  putc_unlocked(int c, FILE* f) { return fputc(c, f); }

// ── getrlimit — unenforced limits, report "unlimited" ────────────────
struct k_rlimit { unsigned long rlim_cur, rlim_max; };
int getrlimit(int resource, struct k_rlimit* rlim) {
    (void)resource;
    if (rlim) {
        rlim->rlim_cur = ~0UL;
        rlim->rlim_max = ~0UL;
    }
    return 0;
}
