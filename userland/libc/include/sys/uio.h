#ifndef _MAKAOS_SYS_UIO_H
#define _MAKAOS_SYS_UIO_H 1

#include <stddef.h>
#include <sys/types.h>

// POSIX scatter/gather I/O.  wayland's connection.c builds iovec
// arrays to push a message header + body in one writev() call.

struct iovec {
    void*  iov_base;
    size_t iov_len;
};

#define IOV_MAX 1024

ssize_t readv(int fd, const struct iovec* iov, int iovcnt);
ssize_t writev(int fd, const struct iovec* iov, int iovcnt);

#endif
