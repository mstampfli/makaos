// ── unistd.c — <unistd.h> syscall wrappers ──────────────────────────
//
// One file per POSIX header.  libc.h has several of these as `static
// inline` for in-tree apps; this file exports the matching extern
// symbols for sysroot consumers (ports linked via -lc).

#include <makaos/syscall.h>
#include <unistd.h>
#include <errno.h>

// ── I/O primitives ──────────────────────────────────────────────────
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
int access(const char* path, int mode) {
    return (int)__syscall_ret(syscall2(SYS_ACCESS, (uint64_t)path, (uint64_t)mode));
}
int unlink(const char* path) {
    size_t n = 0; while (path && path[n]) n++;
    return (int)__syscall_ret(syscall2(SYS_UNLINK, (uint64_t)path, n));
}

// ── Process identity ────────────────────────────────────────────────
pid_t fork(void)     { return (pid_t)__syscall_ret(syscall0(SYS_FORK)); }
pid_t getpid(void)   { return (pid_t)syscall0(SYS_GETPID); }
pid_t getppid(void)  { return (pid_t)syscall0(SYS_GETPPID); }
uid_t getuid(void)   { return (uid_t)syscall0(SYS_GETUID); }
uid_t geteuid(void)  { return (uid_t)syscall0(SYS_GETEUID); }
gid_t getgid(void)   { return (gid_t)syscall0(SYS_GETGID); }
gid_t getegid(void)  { return (gid_t)syscall0(SYS_GETEGID); }

// ── File ownership ──────────────────────────────────────────────────
int chown(const char* path, uid_t uid, gid_t gid) {
    size_t n = 0; while (path && path[n]) n++;
    return (int)__syscall_ret(
        syscall4(SYS_CHOWN, (uint64_t)path, n, (uint64_t)uid, (uint64_t)gid));
}
int fchown(int fd, uid_t uid, gid_t gid) {
    return (int)__syscall_ret(
        syscall3(SYS_FCHOWN, (uint64_t)fd, (uint64_t)uid, (uint64_t)gid));
}

// ── Symlinks ────────────────────────────────────────────────────────
ssize_t readlink(const char* path, char* buf, size_t n) {
    size_t plen = 0; while (path && path[plen]) plen++;
    return (ssize_t)__syscall_ret(
        syscall4(SYS_READLINK,
                 (uint64_t)path, plen,
                 (uint64_t)buf, (uint64_t)n));
}

// ── readv / writev — scatter-gather I/O ─────────────────────────────
// No SYS_READV/SYS_WRITEV in kernel yet; emulate by aggregating into
// a single buffer + one read/write.  Keeps message atomicity for
// stream sockets (wayland depends on this).  Stack-alloc up to 4 KiB,
// malloc beyond.
// TODO(scalability-debt-ledger-#7): real SYS_READV / SYS_WRITEV — the
// memcpy in this emulator is measurable on 10 Gbit/s network paths.
#include <sys/uio.h>

extern void* malloc(size_t);
extern void  free(void*);
extern void* memcpy(void*, const void*, size_t);

static ssize_t __iov_total(const struct iovec* iov, int n) {
    ssize_t s = 0;
    for (int i = 0; i < n; i++) {
        if (iov[i].iov_len > (size_t)(__INTPTR_MAX__ - s)) { errno = EINVAL; return -1; }
        s += (ssize_t)iov[i].iov_len;
    }
    return s;
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    if (!iov || iovcnt <= 0 || iovcnt > IOV_MAX) { errno = EINVAL; return -1; }
    ssize_t total = __iov_total(iov, iovcnt);
    if (total < 0) return -1;
    if (total == 0) return 0;
    char stack_buf[4096];
    char* buf = total <= (ssize_t)sizeof(stack_buf) ? stack_buf : (char*)malloc((size_t)total);
    if (!buf) { errno = ENOMEM; return -1; }
    ssize_t off = 0;
    for (int i = 0; i < iovcnt; i++) {
        memcpy(buf + off, iov[i].iov_base, iov[i].iov_len);
        off += (ssize_t)iov[i].iov_len;
    }
    ssize_t w = write(fd, buf, (size_t)total);
    if (buf != stack_buf) free(buf);
    return w;
}

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    if (!iov || iovcnt <= 0 || iovcnt > IOV_MAX) { errno = EINVAL; return -1; }
    ssize_t total = __iov_total(iov, iovcnt);
    if (total < 0) return -1;
    if (total == 0) return 0;
    char stack_buf[4096];
    char* buf = total <= (ssize_t)sizeof(stack_buf) ? stack_buf : (char*)malloc((size_t)total);
    if (!buf) { errno = ENOMEM; return -1; }
    ssize_t got = read(fd, buf, (size_t)total);
    if (got <= 0) { if (buf != stack_buf) free(buf); return got; }
    ssize_t off = 0;
    for (int i = 0; i < iovcnt && off < got; i++) {
        size_t take = iov[i].iov_len < (size_t)(got - off) ? iov[i].iov_len : (size_t)(got - off);
        memcpy(iov[i].iov_base, buf + off, take);
        off += (ssize_t)take;
    }
    if (buf != stack_buf) free(buf);
    return got;
}

// ── getcwd(buf, n) ──────────────────────────────────────────────────
char* getcwd(char* buf, size_t n) {
    long r = (long)__syscall_ret(syscall2(SYS_GETCWD, (uint64_t)buf, (uint64_t)n));
    return r < 0 ? (char*)0 : buf;
}

// ── symlink(target, linkpath) — no kernel backing yet ───────────────
int symlink(const char* target, const char* linkpath) {
    return (int)__syscall_ret(
        syscall2(SYS_SYMLINK, (uint64_t)target, (uint64_t)linkpath));
}

// ── link(oldpath, newpath) ──────────────────────────────────────────
int link(const char* oldpath, const char* newpath) {
    return (int)__syscall_ret(
        syscall2(SYS_LINK, (uint64_t)oldpath, (uint64_t)newpath));
}

// ── rename(old, new) ────────────────────────────────────────────────
int rename(const char* old, const char* nwp) {
    size_t olen = 0, nlen = 0;
    while (old && old[olen]) olen++;
    while (nwp && nwp[nlen]) nlen++;
    return (int)__syscall_ret(
        syscall4(SYS_RENAME, (uint64_t)old, olen, (uint64_t)nwp, nlen));
}

// ── rmdir(path) — remove empty dir ──────────────────────────────────
// No dedicated SYS_RMDIR yet.  MakaOS's virtfs treats directory
// removal through unlink on directories (kernel rejects if non-empty).
int rmdir(const char* path) {
    size_t n = 0; while (path && path[n]) n++;
    return (int)__syscall_ret(syscall2(SYS_UNLINK, (uint64_t)path, n));
}

// ── Misc ────────────────────────────────────────────────────────────
// x86_64 page size is fixed at 4 KiB.  No syscall needed.
int getpagesize(void) { return 4096; }
