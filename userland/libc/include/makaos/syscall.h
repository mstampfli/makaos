#ifndef _MAKAOS_SYSCALL_H
#define _MAKAOS_SYSCALL_H 1
// MakaOS-specific syscall numbers and raw syscall macros.
// These are the kernel ABI — numbers must match kernel/syscall/syscall.h.

#include <stdint.h>

#define SYS_WRITE    0
#define SYS_EXIT     1
#define SYS_READ     2
#define SYS_OPEN     3
#define SYS_CLOSE    4
#define SYS_BRK      5
#define SYS_KILL     6
#define SYS_FORK     7
#define SYS_EXEC     8
#define SYS_WAIT     9
#define SYS_GETPID   10
#define SYS_READDIR  11
#define SYS_SPAWN    12
#define SYS_THREAD   13
#define SYS_CLOCK_NS 14
#define SYS_STAT     15
#define SYS_UNLINK   16
#define SYS_RENAME   17
#define SYS_GETCWD   18
#define SYS_CHDIR    19
#define SYS_MKDIR       20
#define SYS_LSEEK       21
#define SYS_GETPPID     22
#define SYS_DUP         23
#define SYS_DUP2        24
#define SYS_PIPE        25
#define SYS_SIGACTION   26
#define SYS_SIGPROCMASK 27
#define SYS_SIGRETURN   28
#define SYS_MMAP        29
#define SYS_MUNMAP      30
#define SYS_NANOSLEEP   31
#define SYS_GETTOD      32
#define SYS_FB_BLIT     33
#define SYS_FB_INFO     34
#define SYS_SOCKET      35
#define SYS_BIND        36
#define SYS_LISTEN      37
#define SYS_ACCEPT      38
#define SYS_CONNECT     39
#define SYS_SENDTO      40
#define SYS_RECVFROM    41
#define SYS_SETSOCKOPT  42
#define SYS_SHUTDOWN    43
#define SYS_FCNTL       44
#define SYS_FSTAT       45
#define SYS_ACCESS      46
#define SYS_UNAME       47
#define SYS_UMASK       48
#define SYS_GETUID      49
#define SYS_GETEUID     50
#define SYS_GETGID      51
#define SYS_GETEGID     52
#define SYS_GETGROUPS   53
#define SYS_SETPGID     54
#define SYS_GETPGID     55
#define SYS_GETPGRP     56
#define SYS_SETSID      57
#define SYS_GETSID      58
#define SYS_IOCTL       59
#define SYS_SELECT      60
#define SYS_POLL        61
#define SYS_READLINK    62
#define SYS_SYMLINK     63
#define SYS_LINK        64
#define SYS_CHMOD       65
#define SYS_FCHMOD      66
#define SYS_CHOWN       67
#define SYS_FCHOWN      68
#define SYS_TRUNCATE    69
#define SYS_FTRUNCATE   70
#define SYS_TIMES       71
#define SYS_GETRUSAGE   72
#define SYS_TCGETPGRP   73
#define SYS_TCSETPGRP   74
#define SYS_REBOOT      75
#define SYS_SETUID      76
#define SYS_SETGID      77
#define SYS_SETEUID     78
#define SYS_SETEGID     79
#define SYS_SETREUID    80
#define SYS_SETREGID    81
#define SYS_SETGROUPS   82
#define SYS_PLEDGE      83
#define SYS_UNVEIL      84
#define SYS_UNVEIL_LOCK 85
#define SYS_RESTRICT_FD 86
#define SYS_SENDFD      87
#define SYS_RECVFD      88
#define SYS_REGISTER_POLICY_AGENT 89
#define SYS_SHM_OPEN    90
#define SYS_SHM_UNLINK  91
#define SYS_FB_MAP      92
#define SYS_NET_IFCONFIG 95
#define SYS_NET_MAC      96
#define SYS_EPOLL_CREATE 97
#define SYS_EPOLL_CTL    98
#define SYS_EPOLL_WAIT   99
#define SYS_SLABINFO    100
#define SYS_IO_URING_SETUP    101
#define SYS_IO_URING_ENTER    102
#define SYS_IO_URING_REGISTER 103
#define SYS_SCHED_YIELD       104

// SYS_READ nonblock hint — encoded in the 4th syscall arg to SYS_READ.
#define SYS_READ_NONBLOCK 1

// Raw syscall trampolines — 0–6 positional args on rdi/rsi/rdx/r10/r8/r9.
// Returns the raw kernel value (negative = -errno).  Callers that want
// errno-aware returns should route through the __syscall_ret helper in
// <makaos/errno.h> or a libc wrapper.

static inline uint64_t syscall6(uint64_t nr, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    register uint64_t r10 asm("r10") = a4;
    register uint64_t r8  asm("r8")  = a5;
    register uint64_t r9  asm("r9")  = a6;
    uint64_t ret;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}
static inline uint64_t syscall5(uint64_t nr, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    return syscall6(nr, a1, a2, a3, a4, a5, 0);
}
static inline uint64_t syscall4(uint64_t nr, uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4) {
    return syscall6(nr, a1, a2, a3, a4, 0, 0);
}
static inline uint64_t syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    return syscall4(nr, a1, a2, a3, 0);
}
static inline uint64_t syscall2(uint64_t nr, uint64_t a1, uint64_t a2) {
    return syscall4(nr, a1, a2, 0, 0);
}
static inline uint64_t syscall1(uint64_t nr, uint64_t a1) {
    return syscall4(nr, a1, 0, 0, 0);
}
static inline uint64_t syscall0(uint64_t nr) {
    return syscall4(nr, 0, 0, 0, 0);
}

// -errno decode: negative kernel return → set errno + return -1; positive
// → pass through unchanged.
extern int errno;
static inline long __syscall_ret(uint64_t r) {
    long s = (long)r;
    if (s < 0 && s > -4096) { errno = (int)-s; return -1; }
    return s;
}

#endif
