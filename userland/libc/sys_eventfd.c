// ── sys_eventfd.c — <sys/eventfd.h> (Linux-compatible) ──────────────

#include <makaos/syscall.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <unistd.h>

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
