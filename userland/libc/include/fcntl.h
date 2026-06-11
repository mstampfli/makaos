#ifndef _MAKAOS_FCNTL_H
#define _MAKAOS_FCNTL_H 1

#include <sys/types.h>

// open() flags
#define O_RDONLY    0x00000
#define O_WRONLY    0x00001
#define O_RDWR      0x00002
#define O_ACCMODE   0x00003
#define O_CREAT     0x00040
#define O_EXCL      0x00080
#define O_NOCTTY    0x00100
#define O_TRUNC     0x00200
#define O_APPEND    0x00400
#define O_NONBLOCK  0x00800
#define O_DSYNC     0x01000
#define O_SYNC      0x04000
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_CLOEXEC   0x80000

// fcntl() commands
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_GETLK     5
#define F_SETLK     6
#define F_SETLKW    7
#define F_DUPFD_CLOEXEC 8

// fcntl fd flags
#define FD_CLOEXEC 1

// POSIX record locking — fcntl(fd, F_SETLK/F_SETLKW/F_GETLK, &struct flock).
// Fontconfig / sqlite / gdbm use this for cache coherency across
// processes.  MakaOS is single-process in practice so the lock request
// always succeeds (advisory + uncontended).
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

struct flock {
    short   l_type;     // F_RDLCK / F_WRLCK / F_UNLCK
    short   l_whence;   // SEEK_SET / SEEK_CUR / SEEK_END
    off_t   l_start;
    off_t   l_len;
    pid_t   l_pid;
};

int open(const char* path, int flags, ...);
int openat(int dirfd, const char* path, int flags, ...);
int creat(const char* path, mode_t mode);
int fcntl(int fd, int cmd, ...);

// memfd sealing (Linux values).  The kernel's fcntl returns -EINVAL
// for F_ADD_SEALS/F_GET_SEALS today — every caller we ship (foot's
// wl_shm pool) treats seal failure as non-fatal.  Constants exist so
// the code compiles on its preferred memfd path.
#define F_ADD_SEALS     1033
#define F_GET_SEALS     1034
#define F_SEAL_SEAL     0x0001
#define F_SEAL_SHRINK   0x0002
#define F_SEAL_GROW     0x0004
#define F_SEAL_WRITE    0x0008

#endif
