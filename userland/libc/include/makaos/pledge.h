#ifndef _MAKAOS_PLEDGE_H
#define _MAKAOS_PLEDGE_H 1
// OpenBSD-style pledge + unveil + fd-rights restriction.

// pledge() — space-separated promise list.
int pledge(const char* promises, const char* execpromises);

// unveil() — hide everything except paths explicitly unveiled.
//   perms = "r", "w", "c", "x", or any concatenation
int unveil(const char* path, const char* perms);
int unveil_lock(void);

// Per-fd rights restriction.  Rights bits:
#define RIGHT_READ   0x0001
#define RIGHT_WRITE  0x0002
#define RIGHT_FSTAT  0x0004
#define RIGHT_FCNTL  0x0008
#define RIGHT_LSEEK  0x0010
#define RIGHT_MMAP   0x0020
int restrict_fd(int fd, unsigned rights);

#endif
