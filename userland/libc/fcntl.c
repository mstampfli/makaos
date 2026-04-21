// ── fcntl.c — <fcntl.h> syscall wrappers ────────────────────────────

#include <makaos/syscall.h>
#include <fcntl.h>
#include <stdarg.h>

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
    // Advisory record locking — MakaOS is single-process; no kernel
    // backing yet.  Report success so fontconfig/sqlite don't abort.
    // TODO(scalability-debt-ledger-#10): byte-range locks via the same
    // inode-keyed lock table that backs flock (#5).
    if (cmd == F_SETLK || cmd == F_SETLKW || cmd == F_GETLK) {
        return 0;
    }
    return (int)__syscall_ret(syscall3(SYS_FCNTL, (uint64_t)fd, (uint64_t)cmd, arg));
}
