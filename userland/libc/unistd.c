// ── unistd.c — <unistd.h> syscall wrappers ──────────────────────────
//
// One file per POSIX header.  libc.h has several of these as `static
// inline` for in-tree apps; this file exports the matching extern
// symbols for sysroot consumers (ports linked via -lc).

#include <makaos/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

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

// pipe2 — Linux extension.  Implement as pipe() + fcntl flag
// applications.  We don't yet route an atomic SYS_PIPE2; the race
// window between pipe() creating the fds and fcntl() setting
// O_CLOEXEC doesn't matter for single-threaded SDL init paths.
// TODO(scalability-debt-ledger): expose a real SYS_PIPE2 so callers
// can't leak fds across fork+exec in a multi-threaded race.
extern int fcntl(int fd, int cmd, ...);
#ifndef O_CLOEXEC
#define O_CLOEXEC  02000000
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 00004000
#endif
#ifndef F_GETFL
#define F_GETFL    3
#endif
#ifndef F_SETFL
#define F_SETFL    4
#endif
#ifndef F_GETFD
#define F_GETFD    1
#endif
#ifndef F_SETFD
#define F_SETFD    2
#endif
#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif
int pipe2(int fds[2], int flags) {
    int r = pipe(fds);
    if (r < 0) return r;
    if (flags & O_CLOEXEC) {
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }
    if (flags & O_NONBLOCK) {
        int f0 = fcntl(fds[0], F_GETFL);
        int f1 = fcntl(fds[1], F_GETFL);
        fcntl(fds[0], F_SETFL, f0 | O_NONBLOCK);
        fcntl(fds[1], F_SETFL, f1 | O_NONBLOCK);
    }
    return 0;
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

// ── pread / pwrite — positioned I/O ──────────────────────────────────
// No SYS_PREAD/SYS_PWRITE in kernel yet.  Emulate with lseek+read /
// lseek+write — NOT atomic.  Caller must serialize concurrent pread
// on the same fd.  Fine for wlroots' gamma/EDID paths (single-threaded
// device reads); NOT OK for threaded databases.
// TODO(scalability-debt-ledger-#7): fold into readv/writev work — add
// SYS_PREAD / SYS_PWRITE that atomically lseek+read at the kernel.
ssize_t pread(int fd, void* buf, size_t n, off_t off) {
    off_t cur = lseek(fd, 0, 1 /*SEEK_CUR*/);
    if (cur < 0) return -1;
    if (lseek(fd, off, 0 /*SEEK_SET*/) < 0) return -1;
    ssize_t r = read(fd, buf, n);
    int saved = errno;
    lseek(fd, cur, 0);  // restore
    errno = saved;
    return r;
}

ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) {
    off_t cur = lseek(fd, 0, 1);
    if (cur < 0) return -1;
    if (lseek(fd, off, 0) < 0) return -1;
    ssize_t w = write(fd, buf, n);
    int saved = errno;
    lseek(fd, cur, 0);
    errno = saved;
    return w;
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

// ── waitpid / wait — extern wrappers for sysroot consumers ──────────
pid_t waitpid(pid_t pid, int* status, int options) {
    return (pid_t)__syscall_ret(
        syscall3(SYS_WAIT, (uint64_t)(int64_t)pid, (uint64_t)status, (uint64_t)options));
}
pid_t wait(int* status) { return waitpid(-1, status, 0); }

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

// exec-family — kernel sys_exec takes (path, argv_ptr, envp_ptr).
// argv_ptr and envp_ptr are NULL-terminated user-space arrays of
// char* pointers.  Pass them through straight; the kernel handles
// the copy.  Earlier revisions lost argv because the libc wrappers
// were calling syscall2(SYS_EXEC, path, pathlen) — the kernel then
// saw pathlen in argv's slot and read garbage.
#include <stdarg.h>

#define EXEC_MAX_ARGS 256

int execve(const char* path, char* const argv[], char* const envp[]) {
    return (int)__syscall_ret(syscall3(SYS_EXEC,
        (uint64_t)path, (uint64_t)argv, (uint64_t)envp));
}

int execl(const char* path, const char* arg0, ...) {
    // Collect variadic args into a NULL-terminated array.
    const char* argv[EXEC_MAX_ARGS + 1];
    int argc = 0;
    argv[argc++] = arg0;
    va_list ap;
    va_start(ap, arg0);
    while (argc < EXEC_MAX_ARGS) {
        const char* a = va_arg(ap, const char*);
        argv[argc++] = a;
        if (!a) break;
    }
    argv[argc] = NULL;
    va_end(ap);
    // POSIX: execl passes the caller's environment — only execle takes
    // an explicit envp.  Passing 0 here stripped WAYLAND_DISPLAY and
    // XDG_RUNTIME_DIR from every execl'd child: dwl's `-s` startup
    // command spawned foot into an env with no compositor socket and
    // foot exited with "failed to connect to wayland".
    extern char** environ;
    return (int)__syscall_ret(syscall3(SYS_EXEC,
        (uint64_t)path, (uint64_t)argv, (uint64_t)environ));
}

int execlp(const char* file, const char* arg0, ...) {
    char* argv[EXEC_MAX_ARGS + 1];
    int argc = 0;
    argv[argc++] = (char*)arg0;
    va_list ap;
    va_start(ap, arg0);
    while (argc < EXEC_MAX_ARGS) {
        char* a = va_arg(ap, char*);
        argv[argc++] = a;
        if (!a) break;
    }
    argv[argc] = NULL;
    va_end(ap);
    // Route through execvp so PATH search happens.
    return execvp(file, argv);
}

// POSIX execvp: if `file` contains '/', pass through unchanged.
// Otherwise walk $PATH, trying execve(candidate, argv, environ)
// on each directory, moving on if it returns ENOENT (and a few
// other non-fatal errnos).  Without this, `execvp("foot", ...)`
// hits sys_exec's cwd-based resolver and fails with ENOENT for
// any command not in the current directory.
extern char** environ;

static int try_exec_path(const char* file, char* const argv[]) {
    return (int)__syscall_ret(syscall3(SYS_EXEC,
        (uint64_t)file, (uint64_t)argv, (uint64_t)environ));
}

static const char* getenv_PATH(void) {
    if (!environ) return NULL;
    for (char** e = environ; *e; e++) {
        const char* s = *e;
        if (s[0]=='P' && s[1]=='A' && s[2]=='T' && s[3]=='H' && s[4]=='=')
            return s + 5;
    }
    return NULL;
}

int execvp(const char* file, char* const argv[]) {
    // Contains '/' → direct exec, no PATH search.
    for (const char* p = file; *p; p++) {
        if (*p == '/') return try_exec_path(file, argv);
    }
    const char* PATH = getenv_PATH();
    if (!PATH || !*PATH) PATH = "/bin:/usr/bin";
    // Walk PATH entries separated by ':'.
    char candidate[512];
    const char* cur = PATH;
    while (1) {
        const char* colon = cur;
        while (*colon && *colon != ':') colon++;
        size_t dir_len = (size_t)(colon - cur);
        if (dir_len == 0) { dir_len = 1; }  // "::" treated as "."; approximate with "/"
        if (dir_len + 1 + strlen(file) + 1 > sizeof(candidate)) goto next;
        size_t j = 0;
        for (size_t i = 0; i < dir_len; i++) candidate[j++] = cur[i];
        if (candidate[j-1] != '/') candidate[j++] = '/';
        for (size_t i = 0; file[i]; i++) candidate[j++] = file[i];
        candidate[j] = '\0';
        try_exec_path(candidate, argv);
        // If we get here, try_exec_path returned -1; errno tells us
        // whether to keep searching or abort.
        if (errno != ENOENT && errno != ENOTDIR && errno != EACCES) return -1;
next:
        if (!*colon) break;
        cur = colon + 1;
    }
    errno = ENOENT;
    return -1;
}

int execv(const char* path, char* const argv[]) {
    return try_exec_path(path, argv);
}

int execvpe(const char* path, char* const argv[], char* const envp[]) {
    return (int)__syscall_ret(syscall3(SYS_EXEC,
        (uint64_t)path, (uint64_t)argv, (uint64_t)envp));
}

// setsid — new session/process group.  MakaOS has no multi-session
// model; report the current pid as the "new session id."
pid_t setsid(void) { return getpid(); }

int setpgid(pid_t pid, pid_t pgid) {
    (void)pid; (void)pgid; return 0;
}
pid_t getpgid(pid_t pid) { (void)pid; return getpid(); }
pid_t getpgrp(void)      { return getpid(); }
pid_t getsid(pid_t pid)  { (void)pid; return getpid(); }

// kill — signal a pid.  SYS_KILL backs this.
int kill(pid_t pid, int sig) {
    return (int)__syscall_ret(
        syscall2(SYS_KILL, (uint64_t)(int64_t)pid, (uint64_t)(unsigned)sig));
}

// ffs — find first set bit (1-based).  Compiler builtin.
int ffs(int i)          { return __builtin_ffs(i); }
int ffsl(long i)        { return __builtin_ffsl(i); }
int ffsll(long long i)  { return __builtin_ffsll(i); }

// waitid — POSIX.1-2008 extended wait.  MakaOS kernel only has
// SYS_WAIT (waitpid semantics); fill siginfo_t from the waitpid
// status.  Supports P_PID and P_ALL; P_PGID falls back to P_ALL
// since we don't track pgid sets kernel-side yet.
// TODO(scalability-debt-ledger): SYS_WAITID with true si_code
// distinctions (CLD_STOPPED vs CLD_TRAPPED needs ptrace-like infra).
#include <sys/wait.h>
#include <signal.h>
int waitid(idtype_t idtype, id_t id, siginfo_t* info, int options) {
    int status = 0;
    pid_t target = (idtype == P_PID) ? (pid_t)id : -1;
    pid_t r = waitpid(target, &status, options);
    if (r < 0) return -1;
    if (info) {
        info->si_signo = SIGCHLD;
        info->si_pid   = r;
        info->si_status= status;
        if (WIFEXITED(status))         info->si_code = CLD_EXITED;
        else if (WIFSIGNALED(status))  info->si_code = (WTERMSIG(status) == 6) ? CLD_DUMPED : CLD_KILLED;
        else if (WIFSTOPPED(status))   info->si_code = CLD_STOPPED;
        else if (WIFCONTINUED(status)) info->si_code = CLD_CONTINUED;
        else                           info->si_code = 0;
    }
    return 0;
}

// isatty — we don't yet track whether an fd is a tty in userland.
// Report fds 0/1/2 as ttys (stdio) and everything else as non-tty.
// Good enough for wlroots/bash/harfbuzz "am I interactive?" checks.
int isatty(int fd) {
    return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0;
}

// truncate / ftruncate — kernel-backed file size change.
int truncate(const char* path, off_t length) {
    size_t n = 0; while (path && path[n]) n++;
    return (int)__syscall_ret(
        syscall3(SYS_TRUNCATE, (uint64_t)path, n, (uint64_t)length));
}

int ftruncate(int fd, off_t length) {
    return (int)__syscall_ret(
        syscall2(SYS_FTRUNCATE, (uint64_t)fd, (uint64_t)length));
}

// ── Port-surface extern impls ────────────────────────────────────────
// Declared in <unistd.h>; previously only inline in libc.h.

int gethostname(char* name, size_t len) {
    const char* host = "makaos";
    if (!name || !len) { errno = EINVAL; return -1; }
    size_t n = 0;
    while (host[n] && n + 1 < len) { name[n] = host[n]; n++; }
    name[n] = '\0';
    return 0;
}

// fsync / fdatasync — MakaOS's VFS doesn't yet expose a flush
// syscall; writes go through the page cache with periodic writeback.
// Return 0 so apps that call these don't error out; once the kernel
// grows SYS_FSYNC these become one-line wrappers.
// TODO(scalability-debt-ledger): expose SYS_FSYNC so crash-durable
// apps (databases, journal files) can actually force a flush.
int fsync(int fd)     { (void)fd; return 0; }
int fdatasync(int fd) { (void)fd; return 0; }
