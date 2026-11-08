// jdk_compat.c - extern symbols required by sysroot C++ consumers (OpenJDK's
// libjvm/libjava/libnio/libnet) that libc otherwise only had as static inlines
// in libc.h, or lacked entirely. MakaOS's dlopen rejects a shared object with
// any unresolvable strong-undefined symbol, so every symbol a JDK .so imports
// must have a linkable definition here (exported by the launcher at load time).
//
// Follows the wrapper-.c convention: sysroot headers only, never libc.h.

#include <makaos/syscall.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>

#ifndef SYS_UMASK
#define SYS_UMASK 48
#endif
#ifndef SYS_GETRUSAGE
#define SYS_GETRUSAGE 72
#endif
#ifndef SYS_CHOWN
#define SYS_CHOWN 67
#endif

// umask - set the file-mode creation mask, return the previous one.
mode_t umask(mode_t mask) {
    return (mode_t)syscall1(SYS_UMASK, (uint64_t)mask);
}

// getrusage - resource usage. Backed by SYS_GETRUSAGE; the kernel zero-fills
// fields it does not track yet.
int getrusage(int who, struct rusage* usage) {
    return (int)__syscall_ret(syscall2(SYS_GETRUSAGE, (uint64_t)who, (uint64_t)usage));
}

// lchown - like chown but never follows a final symlink. MakaOS's symlink
// support is minimal, so route through the same SYS_CHOWN path.
int lchown(const char* path, uid_t owner, gid_t group) {
    size_t n = 0; while (path && path[n]) n++;
    return (int)__syscall_ret(
        syscall4(SYS_CHOWN, (uint64_t)path, n, (uint64_t)owner, (uint64_t)group));
}

// fchdir - change directory via an open fd. No backing syscall yet; report
// ENOSYS so callers fall back to a path-based chdir.
// TODO(scalability-debt-ledger): add SYS_FCHDIR (the kernel already resolves
// fds to inodes for fstat, so the cwd swap is a small addition).
int fchdir(int fd) {
    (void)fd;
    errno = ENOSYS;
    return -1;
}

// mprotect - change protection on a mapping. MakaOS has no SYS_MPROTECT yet;
// the VMM has the perm-tightening + TLB-shootdown infrastructure but no user
// entry point. Report success so consumers (the JVM's guard-page setup) proceed;
// the pages simply keep their original permissions until a real syscall lands.
// TODO(scalability-debt-ledger): wire SYS_MPROTECT into the VMM so guard pages
// and W^X actually take effect (JVM stack-overflow detection depends on it).
int mprotect(void* addr, size_t len, int prot) {
    (void)addr; (void)len; (void)prot;
    return 0;
}

// sigsuspend - atomically install `mask` and wait for a signal, then restore.
// Without a SYS_PAUSE we approximate: swap the mask, sleep interruptibly (a
// delivered signal wakes nanosleep with EINTR), restore, and report EINTR.
// TODO(scalability-debt-ledger): a real SYS_PAUSE removes the wakeup latency
// and the tiny set-mask/sleep race.
int sigsuspend(const sigset_t* mask) {
    sigset_t old;
    sigprocmask(SIG_SETMASK, mask, &old);
    struct timespec ts = { 100000, 0 };   // long, cut short by any delivery
    nanosleep(&ts, (struct timespec*)0);
    sigprocmask(SIG_SETMASK, &old, (sigset_t*)0);
    errno = EINTR;
    return -1;
}

// __xpg_strerror_r - the XSI-compliant (int-returning) strerror_r. Copies the
// message for `errnum` into buf, truncating to buflen; returns 0, or ERANGE if
// the buffer was too small.
int __xpg_strerror_r(int errnum, char* buf, size_t buflen) {
    if (!buf || buflen == 0) return ERANGE;
    const char* msg = strerror(errnum);
    size_t len = strlen(msg);
    if (len >= buflen) {
        memcpy(buf, msg, buflen - 1);
        buf[buflen - 1] = '\0';
        return ERANGE;
    }
    memcpy(buf, msg, len + 1);
    return 0;
}

// wcstombs - convert a wide-char string to a multibyte (UTF-8) byte string.
// Encodes each wchar_t as UTF-8. dst==NULL just measures the byte length.
static size_t wc_to_utf8(char* out, unsigned int wc) {
    if (wc < 0x80)          { if (out) out[0] = (char)wc; return 1; }
    if (wc < 0x800)         { if (out) { out[0]=(char)(0xC0|(wc>>6)); out[1]=(char)(0x80|(wc&0x3F)); } return 2; }
    if (wc < 0x10000)       { if (out) { out[0]=(char)(0xE0|(wc>>12)); out[1]=(char)(0x80|((wc>>6)&0x3F)); out[2]=(char)(0x80|(wc&0x3F)); } return 3; }
    if (out) { out[0]=(char)(0xF0|(wc>>18)); out[1]=(char)(0x80|((wc>>12)&0x3F)); out[2]=(char)(0x80|((wc>>6)&0x3F)); out[3]=(char)(0x80|(wc&0x3F)); }
    return 4;
}

size_t wcstombs(char* dst, const wchar_t* src, size_t n) {
    if (!src) { errno = EINVAL; return (size_t)-1; }
    size_t produced = 0;
    for (const wchar_t* p = src; *p; p++) {
        char tmp[4];
        size_t k = wc_to_utf8(tmp, (unsigned int)*p);
        if (dst) {
            if (produced + k > n) break;   // no room; stop (dst not NUL-terminated)
            memcpy(dst + produced, tmp, k);
        }
        produced += k;
        if (dst && produced >= n) return produced;
    }
    if (dst && produced < n) dst[produced] = '\0';
    return produced;
}

// sleep - suspend for `seconds`, returning the unslept remainder (0 on full
// sleep). libc.h has this as a static inline; export the matching extern.
unsigned int sleep(unsigned int seconds) {
    struct timespec req = { (long)seconds, 0 }, rem = { 0, 0 };
    if (nanosleep(&req, &rem) < 0 && errno == EINTR)
        return (unsigned int)rem.tv_sec + (rem.tv_nsec ? 1u : 0u);
    return 0;
}
