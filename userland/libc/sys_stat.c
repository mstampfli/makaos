// ── sys_stat.c — <sys/stat.h> syscall wrappers ──────────────────────

#include <makaos/syscall.h>
#include <sys/stat.h>
#include <errno.h>

int stat(const char* path, struct stat* st) {
    size_t n = 0; while (path && path[n]) n++;
    return (int)__syscall_ret(syscall3(SYS_STAT, (uint64_t)path, n, (uint64_t)st));
}

int lstat(const char* path, struct stat* st) {
    // No symlink support yet — lstat behaves like stat.
    return stat(path, st);
}

int fstat(int fd, struct stat* st) {
    return (int)__syscall_ret(syscall2(SYS_FSTAT, (uint64_t)fd, (uint64_t)st));
}

int mkdir(const char* path, mode_t mode) {
    size_t n = 0; while (path && path[n]) n++;
    return (int)__syscall_ret(syscall3(SYS_MKDIR, (uint64_t)path, n, (uint64_t)mode));
}

int chmod(const char* path, mode_t mode) {
    size_t n = 0; while (path && path[n]) n++;
    return (int)__syscall_ret(
        syscall3(SYS_CHMOD, (uint64_t)path, n, (uint64_t)mode));
}

int fchmod(int fd, mode_t mode) {
    return (int)__syscall_ret(
        syscall2(SYS_FCHMOD, (uint64_t)fd, (uint64_t)mode));
}

// mknod: no kernel backing.  /dev/* is pre-populated via virtfs; ports
// that probe for this (libdrm's drmOpenDevice creation path) degrade
// gracefully on ENOSYS and fall through to the "open existing" branch.
// TODO(scalability-debt-ledger-#8): SYS_MKNOD once virtfs is writable
// and the device registry (ledger #2) can resolve major/minor → driver.
int mknod(const char* path, unsigned mode, unsigned dev) {
    (void)path; (void)mode; (void)dev;
    errno = ENOSYS;
    return -1;
}
