// glibc_compat.c - symbols that PREBUILT glibc shared objects import.
//
// MakaOS's dlopen rejects a .so with any unresolvable strong-undefined symbol,
// so a prebuilt glibc native library (LWJGL's liblwjgl.so, and others) needs
// every symbol it imports defined here. These are the glibc-ABI names our libc
// did not already export: the *64 large-file aliases (identical to the base
// functions under LP64), the old __xstat/__fxstat stat ABI, __getdelim, and
// aligned_alloc. Follows the wrapper-.c convention: sysroot headers only.

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>

// __snprintf_chk: the _FORTIFY_SOURCE variant of snprintf.  Prebuilt glibc
// .so's built with -D_FORTIFY_SOURCE (jdk.management's libmanagement_ext.so)
// import this instead of snprintf.  vsnprintf already bounds output to `maxlen`,
// so the fortify object-size arg (slen) needs no extra check for correctness --
// forward to vsnprintf.  Without it, dlopen(libmanagement_ext.so) fails with
// "undefined symbol: __snprintf_chk", so PlatformMBeanProviderImpl can't init,
// so ManagementFactory.getRuntimeMXBean() throws and Minecraft exits at startup.
int __snprintf_chk(char* s, size_t maxlen, int flag, size_t slen,
                   const char* fmt, ...) {
    (void)flag; (void)slen;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s, maxlen, fmt, ap);
    va_end(ap);
    return r;
}

// glibc 2.38+ redirects the scanf family to C23 variants (__isoc23_*) in its
// headers, so a .so built against new glibc imports those names.  Forward to
// our existing implementations (fscanf has no FILE-backed parser -> EOF, same
// as our fscanf stub; vsscanf is the real engine).
#include <ctype.h>
extern int vsscanf(const char* str, const char* fmt, va_list ap);
int __isoc23_vsscanf(const char* str, const char* fmt, va_list ap) {
    return vsscanf(str, fmt, ap);
}
int __isoc23_sscanf(const char* str, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}
int __isoc23_fscanf(void* stream, const char* fmt, ...) {
    (void)stream; (void)fmt;
    return -1; /* EOF -- matches our fscanf stub (no FILE scanf backend) */
}

// __ctype_b_loc: glibc's ctype classification-table accessor.  is*() macros in
// glibc headers expand to (*__ctype_b_loc())[c] & _ISxxx, so a prebuilt .so
// needs the table.  Build it once from our own is*() functions.  glibc packs
// the class bits byte-swapped: _ISbit(b) = b<8 ? (1<<b)<<8 : (1<<b)>>8.
static unsigned short s_ctb[384];             // indices -128..255, offset +128
static const unsigned short* s_ctb_ptr = s_ctb + 128;
static int s_ctb_done = 0;
#define _ISbit(b) ((unsigned short)((b) < 8 ? ((1 << (b)) << 8) : ((1 << (b)) >> 8)))
const unsigned short** __ctype_b_loc(void) {
    if (!s_ctb_done) {
        for (int c = 0; c < 256; c++) {
            unsigned short m = 0;
            if (isupper(c))  m |= _ISbit(0);
            if (islower(c))  m |= _ISbit(1);
            if (isalpha(c))  m |= _ISbit(2);
            if (isdigit(c))  m |= _ISbit(3);
            if (isxdigit(c)) m |= _ISbit(4);
            if (isspace(c))  m |= _ISbit(5);
            if (isprint(c))  m |= _ISbit(6);
            if (isgraph(c))  m |= _ISbit(7);
            if (isblank(c))  m |= _ISbit(8);
            if (iscntrl(c))  m |= _ISbit(9);
            if (ispunct(c))  m |= _ISbit(10);
            if (isalnum(c))  m |= _ISbit(11);
            s_ctb[c + 128] = m;
        }
        s_ctb_done = 1;
    }
    return &s_ctb_ptr;
}

// More glibc-ABI names imported by the prebuilt LWJGL natives (liblwjgl*.so,
// liblwjgl_tinyfd/stb/opengl.so):
FILE* fopen(const char*, const char*);
FILE* fopen64(const char* path, const char* mode) { return fopen(path, mode); }  // LP64: off_t already 64-bit
int   getc(FILE*);
int   _IO_getc(FILE* f) { return getc(f); }                                      // glibc-internal getc
extern double sin(double), cos(double);
void sincos(double x, double* s, double* c)  { *s = sin(x);         *c = cos(x); }
void sincosf(float x, float* s, float* c)    { *s = (float)sin(x);  *c = (float)cos(x); }

// __ctype_tolower_loc: glibc's tolower table accessor.  tolower() macro ->
// (*__ctype_tolower_loc())[c].  int[384] indexed -128..255 (EOF passes through).
static int s_ctl[384];
static const int* s_ctl_ptr = s_ctl + 128;
static int s_ctl_done = 0;
const int** __ctype_tolower_loc(void) {
    if (!s_ctl_done) {
        for (int c = -128; c < 256; c++)
            s_ctl[c + 128] = (c >= 0 && c < 256) ? tolower(c) : c;
        s_ctl_done = 1;
    }
    return &s_ctl_ptr;
}

// ── Large-file (*64) aliases -- LP64: off_t is already 64-bit ────────────────
void* mmap64(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
    return mmap(addr, len, prot, flags, fd, off);
}
int open64(const char* path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    return open(path, flags, mode);
}
int creat64(const char* path, mode_t mode) { return creat(path, mode); }

// openat: MakaOS has no SYS_OPENAT. AT_FDCWD or an absolute path routes to open;
// a real dirfd-relative open is unsupported.
int openat(int dirfd, const char* path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    if (dirfd == AT_FDCWD || (path && path[0] == '/')) return open(path, flags, mode);
    errno = ENOSYS; return -1;
}
int openat64(int dirfd, const char* path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    return openat(dirfd, path, flags, mode);
}

// ── Old glibc stat ABI: a version arg precedes (path/fd, buf) ────────────────
int __xstat64(int ver, const char* path, struct stat* buf)  { (void)ver; return stat(path, buf); }
int __fxstat64(int ver, int fd, struct stat* buf)           { (void)ver; return fstat(fd, buf); }
// Non-*64 forms of the same pre-2.33 ABI (LWJGL's libopenal.so imports __xstat).
// Under LP64 struct stat == struct stat64, so these forward identically.
int __xstat(int ver, const char* path, struct stat* buf)    { (void)ver; return stat(path, buf); }
int __fxstat(int ver, int fd, struct stat* buf)             { (void)ver; return fstat(fd, buf); }
int __lxstat(int ver, const char* path, struct stat* buf)   { (void)ver; return lstat(path, buf); }

// ── cabsf: complex-float magnitude (libopenal's HRTF math).  Passed one XMM
// (real|imag); hypotf gives the overflow-safe magnitude. ─────────────────────
float cabsf(float _Complex z) { return hypotf(__real__ z, __imag__ z); }

// ── gettext: no message catalogs on MakaOS -> identity (return the msgid). ───
char* gettext(const char* msgid) { return (char*)msgid; }

// ── __pthread_key_create: glibc's weak internal alias of pthread_key_create.
// libgcc/libstdc++ reference it weakly to detect a threaded libc; define it so
// TLS-key creation works rather than binding to NULL. ────────────────────────
int __pthread_key_create(pthread_key_t* key, void (*dtor)(void*)) {
    return pthread_key_create(key, dtor);
}

// ── __strdup: glibc's internal alias of strdup (JNA's libjnidispatch imports it)
char* __strdup(const char* s) { return strdup(s); }

// ── glibc-version-specific symbols the HotSpot server libjvm.so imports ───────
// glibc 2.32+ single-threaded fast-path flag; the JVM is always multi-threaded,
// so 0 (never take the lock-elision fast path) is the safe, correct value.
char __libc_single_threaded = 0;
// secure_getenv: getenv that returns NULL under privilege elevation; MakaOS makes
// no such distinction here, so plain getenv.
char* secure_getenv(const char* name) { return getenv(name); }
// C23 strtoul symbol (glibc 2.38 renamed the entry); identical to strtoul.
unsigned long __isoc23_strtoul(const char* nptr, char** endptr, int base) {
    return strtoul(nptr, endptr, base);
}
// C++ ABI: invoked if a pure-virtual call ever slips through; abort loudly.
void __cxa_pure_virtual(void) { abort(); }
// glibc 2.35+ loader helper to locate the object containing an address. MakaOS's
// unwinder does not rely on it; report "not found" so callers use their fallback.
int _dl_find_object(void* address, void* result) { (void)address; (void)result; return -1; }
// DTrace/collector probe hook; no profiling collector on MakaOS.
void collector_func_load(void) { }

// ── aligned_alloc (C11) ─────────────────────────────────────────────────────
void* aligned_alloc(size_t alignment, size_t size) {
    void* p = 0;
    if (posix_memalign(&p, alignment, size) != 0) return 0;
    return p;
}

// ── preadv / pwritev (+ *64): emulate over pread/pwrite ─────────────────────
ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t off) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t r = pread(fd, iov[i].iov_base, iov[i].iov_len, off);
        if (r < 0) return total ? total : -1;
        total += r; off += r;
        if ((size_t)r < iov[i].iov_len) break;   // short read -> stop
    }
    return total;
}
ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t off) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t w = pwrite(fd, iov[i].iov_base, iov[i].iov_len, off);
        if (w < 0) return total ? total : -1;
        total += w; off += w;
        if ((size_t)w < iov[i].iov_len) break;
    }
    return total;
}
ssize_t preadv64(int fd, const struct iovec* iov, int iovcnt, off_t off)  { return preadv(fd, iov, iovcnt, off); }
ssize_t pwritev64(int fd, const struct iovec* iov, int iovcnt, off_t off) { return pwritev(fd, iov, iovcnt, off); }

// ── getdelim / __getdelim ────────────────────────────────────────────────────
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* stream) {
    if (!lineptr || !n || !stream) { errno = EINVAL; return -1; }
    if (!*lineptr || *n == 0) { *n = 128; *lineptr = (char*)malloc(*n); if (!*lineptr) return -1; }
    size_t pos = 0; int c;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t nn = *n * 2;
            char* np = (char*)realloc(*lineptr, nn);
            if (!np) return -1;
            *lineptr = np; *n = nn;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == delim) break;
    }
    if (pos == 0 && c == EOF) return -1;
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}
ssize_t __getdelim(char** lineptr, size_t* n, int delim, FILE* stream) {
    return getdelim(lineptr, n, delim, stream);
}

// ── process_vm_readv/writev: no cross-process memory access ─────────────────
ssize_t process_vm_readv(pid_t pid, const struct iovec* lvec, unsigned long liovcnt,
                         const struct iovec* rvec, unsigned long riovcnt, unsigned long flags) {
    (void)pid; (void)lvec; (void)liovcnt; (void)rvec; (void)riovcnt; (void)flags;
    errno = ENOSYS; return -1;
}
ssize_t process_vm_writev(pid_t pid, const struct iovec* lvec, unsigned long liovcnt,
                          const struct iovec* rvec, unsigned long riovcnt, unsigned long flags) {
    (void)pid; (void)lvec; (void)liovcnt; (void)rvec; (void)riovcnt; (void)flags;
    errno = ENOSYS; return -1;
}
