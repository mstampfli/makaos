// ── libc syscall wrappers — extern symbols for sysroot consumers ──────
//
// userland/libc/libc.h declares the basic syscall wrappers as `static
// inline` so legacy apps that include libc.h get them inlined.  Any TU
// that uses the new split headers (<unistd.h>, <fcntl.h>, <sys/socket.h>,
// etc.) expects external linkage — this file provides those symbols.
//
// Each wrapper routes through syscall* defined in <makaos/syscall.h>
// and translates the negative-kernel-return convention into POSIX
// errno via __syscall_ret.

#include <makaos/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>

// ── I/O ───────────────────────────────────────────────────────────────

ssize_t read(int fd, void* buf, size_t n) {
    return (ssize_t)__syscall_ret(syscall3(SYS_READ, (uint64_t)fd, (uint64_t)buf, n));
}
ssize_t write(int fd, const void* buf, size_t n) {
    return (ssize_t)__syscall_ret(syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, n));
}
int close(int fd) {
    return (int)__syscall_ret(syscall1(SYS_CLOSE, (uint64_t)fd));
}
off_t lseek(int fd, off_t off, int whence) {
    return (off_t)__syscall_ret(syscall3(SYS_LSEEK, (uint64_t)fd, (uint64_t)off, (uint64_t)whence));
}
int dup(int fd) {
    return (int)__syscall_ret(syscall1(SYS_DUP, (uint64_t)fd));
}
int dup2(int oldfd, int newfd) {
    return (int)__syscall_ret(syscall2(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd));
}
int pipe(int fds[2]) {
    return (int)__syscall_ret(syscall1(SYS_PIPE, (uint64_t)fds));
}

int open(const char* path, int flags, ...) {
    uint64_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (uint64_t)va_arg(ap, int);
        va_end(ap);
    }
    return (int)__syscall_ret(syscall3(SYS_OPEN, (uint64_t)path, (uint64_t)flags, mode));
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    uint64_t arg = (uint64_t)va_arg(ap, long);
    va_end(ap);
    return (int)__syscall_ret(syscall3(SYS_FCNTL, (uint64_t)fd, (uint64_t)cmd, arg));
}

int access(const char* path, int mode) {
    return (int)__syscall_ret(syscall2(SYS_ACCESS, (uint64_t)path, (uint64_t)mode));
}

// ── Process ───────────────────────────────────────────────────────────

pid_t fork(void)     { return (pid_t)__syscall_ret(syscall0(SYS_FORK)); }
pid_t getpid(void)   { return (pid_t)syscall0(SYS_GETPID); }
pid_t getppid(void)  { return (pid_t)syscall0(SYS_GETPPID); }
uid_t getuid(void)   { return (uid_t)syscall0(SYS_GETUID); }
uid_t geteuid(void)  { return (uid_t)syscall0(SYS_GETEUID); }
gid_t getgid(void)   { return (gid_t)syscall0(SYS_GETGID); }
gid_t getegid(void)  { return (gid_t)syscall0(SYS_GETEGID); }

// ── clock_gettime / clock_getres (POSIX) ──────────────────────────────
// MakaOS has a single monotonic nanosecond clock exposed via SYS_CLOCK_NS.
// Every clock id maps to it — we don't separate wall vs. monotonic yet.
int clock_gettime(clockid_t id, struct timespec* ts) {
    (void)id;
    if (!ts) { errno = EINVAL; return -1; }
    uint64_t ns = syscall0(SYS_CLOCK_NS);
    ts->tv_sec  = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)  (ns % 1000000000ull);
    return 0;
}

int clock_getres(clockid_t id, struct timespec* res) {
    (void)id;
    if (!res) { errno = EINVAL; return -1; }
    res->tv_sec  = 0;
    res->tv_nsec = 1;                      // nanosecond resolution
    return 0;
}

// ── setbuf — thin wrapper over setvbuf (which libc.c provides). ───────
void setbuf(FILE* f, char* buf) {
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

// ── eventfd (Linux-compatible 64-bit counter fd) ──────────────────────
// Thin kernel wrapper; read/write go through the normal fd read/write
// path because kernel/io/eventfd.c installs its own vfs ops.
int eventfd(unsigned int initval, int flags) {
    return (int)__syscall_ret(
        syscall2(SYS_EVENTFD, (uint64_t)initval, (uint64_t)(unsigned int)flags));
}

int eventfd_read(int fd, eventfd_t* value) {
    if (!value) { errno = EINVAL; return -1; }
    return read(fd, value, sizeof(*value)) == (ssize_t)sizeof(*value) ? 0 : -1;
}

int eventfd_write(int fd, eventfd_t value) {
    return write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value) ? 0 : -1;
}

// ── timerfd (Linux-compatible) ────────────────────────────────────────
int timerfd_create(int clockid, int flags) {
    return (int)__syscall_ret(
        syscall2(SYS_TIMERFD_CREATE, (uint64_t)clockid, (uint64_t)(unsigned int)flags));
}

int timerfd_settime(int fd, int flags,
                    const struct itimerspec* new_value,
                    struct itimerspec* old_value) {
    return (int)__syscall_ret(
        syscall4(SYS_TIMERFD_SETTIME,
                 (uint64_t)fd, (uint64_t)(unsigned int)flags,
                 (uint64_t)new_value, (uint64_t)old_value));
}

int timerfd_gettime(int fd, struct itimerspec* curr_value) {
    return (int)__syscall_ret(
        syscall2(SYS_TIMERFD_GETTIME, (uint64_t)fd, (uint64_t)curr_value));
}

// ── socketpair (POSIX) ────────────────────────────────────────────────
int socketpair(int domain, int type, int protocol, int fds[2]) {
    return (int)__syscall_ret(
        syscall4(SYS_SOCKETPAIR,
                 (uint64_t)domain, (uint64_t)type,
                 (uint64_t)protocol, (uint64_t)fds));
}

// ── sendmsg / recvmsg with SCM_RIGHTS (POSIX) ─────────────────────────
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags) {
    return (ssize_t)__syscall_ret(
        syscall3(SYS_SENDMSG, (uint64_t)fd, (uint64_t)msg, (uint64_t)flags));
}

ssize_t recvmsg(int fd, struct msghdr* msg, int flags) {
    return (ssize_t)__syscall_ret(
        syscall3(SYS_RECVMSG, (uint64_t)fd, (uint64_t)msg, (uint64_t)flags));
}

// ── signalfd (POSIX / Linux-compatible) ───────────────────────────────
int signalfd(int fd, const sigset_t* mask, int flags) {
    return (int)__syscall_ret(
        syscall3(SYS_SIGNALFD,
                 (uint64_t)(int64_t)fd,
                 (uint64_t)mask,
                 (uint64_t)(unsigned int)flags));
}

// ── Sockets + time(NULL)/nanosleep — provided by libc.c's extern
//    wrappers, not here. ─────────────────────────────────────────────

// ── net byte order (libc.h has them static-inline — sysroot consumers
//    need real symbols) ───────────────────────────────────────────────

uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
uint16_t ntohs(uint16_t v) { return htons(v); }
uint32_t htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | (((v >> 16) & 0xFF) << 8)
         | (((v >> 8) & 0xFF) << 16) | ((v & 0xFF) << 24);
}
uint32_t ntohl(uint32_t v) { return htonl(v); }

// ── memchr (used by http_get) — not a syscall, simple loop ────────────

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char target = (unsigned char)c;
    while (n--) {
        if (*p == target) return (void*)p;
        p++;
    }
    return (void*)0;
}
