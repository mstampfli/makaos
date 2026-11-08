// sys_statfs.c - <sys/vfs.h> (Linux statfs).
//
// MakaOS has no statfs syscall yet, so these report ENOSYS. Callers that only
// use statfs to classify a mount (e.g. OpenJDK's cgroup/container detection)
// then conclude the feature is absent and fall back to host behaviour, which
// is correct for MakaOS (it is not a container guest). TODO: back with a real
// SYS_STATFS when the VFS grows one.

#include <sys/vfs.h>
#include <errno.h>

int statfs(const char* path, struct statfs* buf) {
    (void)path; (void)buf;
    errno = ENOSYS;
    return -1;
}

int fstatfs(int fd, struct statfs* buf) {
    (void)fd; (void)buf;
    errno = ENOSYS;
    return -1;
}
