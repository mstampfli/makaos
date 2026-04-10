#pragma once
#include <stdint.h>

// ── Syscall numbers ───────────────────────────────────────────────────────
#define SYS_WRITE   0
#define SYS_EXIT    1
#define SYS_READ    2
#define SYS_OPEN    3
#define SYS_CLOSE   4
#define SYS_BRK     5
#define SYS_KILL    6
#define SYS_FORK    7
#define SYS_EXEC    8
#define SYS_WAIT    9
#define SYS_GETPID  10
#define SYS_READDIR 11
#define SYS_SPAWN   12
#define SYS_THREAD  13
#define SYS_CLOCK_NS 14
#define SYS_STAT    15
#define SYS_UNLINK  16
#define SYS_RENAME  17
#define SYS_GETCWD  18
#define SYS_CHDIR   19
#define SYS_MKDIR    20
#define SYS_LSEEK    21
#define SYS_GETPPID  22
#define SYS_DUP      23
#define SYS_DUP2     24
#define SYS_PIPE     25
#define SYS_SIGACTION 26
#define SYS_SIGPROCMASK 27
#define SYS_SIGRETURN 28
#define SYS_MMAP     29
#define SYS_MUNMAP   30
#define SYS_NANOSLEEP 31
#define SYS_GETTOD   32
#define SYS_FB_BLIT  33
#define SYS_FB_INFO  34
#define SYS_SOCKET     35
#define SYS_BIND       36
#define SYS_LISTEN     37
#define SYS_ACCEPT     38
#define SYS_CONNECT    39
#define SYS_SENDTO     40
#define SYS_RECVFROM   41
#define SYS_SETSOCKOPT 42
#define SYS_SHUTDOWN   43
#define SYS_FCNTL      44
#define SYS_FSTAT      45
#define SYS_ACCESS     46
#define SYS_UNAME      47
#define SYS_UMASK      48
#define SYS_GETUID     49
#define SYS_GETEUID    50
#define SYS_GETGID     51
#define SYS_GETEGID    52
#define SYS_GETGROUPS  53
#define SYS_SETPGID    54
#define SYS_GETPGID    55
#define SYS_GETPGRP    56
#define SYS_SETSID     57
#define SYS_GETSID     58
#define SYS_IOCTL      59
#define SYS_SELECT     60
#define SYS_POLL       61
#define SYS_READLINK   62
#define SYS_SYMLINK    63
#define SYS_LINK       64
#define SYS_CHMOD      65
#define SYS_FCHMOD     66
#define SYS_CHOWN      67
#define SYS_FCHOWN     68
#define SYS_TRUNCATE   69
#define SYS_FTRUNCATE  70
#define SYS_TIMES      71
#define SYS_GETRUSAGE  72
#define SYS_TCGETPGRP  73
#define SYS_TCSETPGRP  74

// ── waitpid options ───────────────────────────────────────────────────────
#define WNOHANG 1

// ── O_ file flags ─────────────────────────────────────────────────────────
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x040
#define O_EXCL      0x080
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_CLOEXEC   0x80000

// ── fcntl commands ────────────────────────────────────────────────────────
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_DUPFD_CLOEXEC 1030

// ── access() mode bits ────────────────────────────────────────────────────
#define F_OK  0
#define X_OK  1
#define W_OK  2
#define R_OK  4

// ── ioctl requests ────────────────────────────────────────────────────────
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCSBRK      0x5409
#define TCXONC      0x540A
#define TCFLSH      0x540B
#define TIOCEXCL    0x540C
#define TIOCNXCL    0x540D
#define TIOCSCTTY   0x540E
#define TIOCGPTN    0x80045430
#define TIOCSPTLCK  0x40045431
#define TIOCGSERIAL 0x541E
#define TIOCSTI     0x5412

// ── termios c_iflag bits ──────────────────────────────────────────────────
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IUCLC   0001000
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define IUTF8   0040000

// ── termios c_oflag bits ──────────────────────────────────────────────────
#define OPOST   0000001
#define OLCUC   0000002
#define ONLCR   0000004
#define OCRNL   0000010
#define ONOCR   0000020
#define ONLRET  0000040
#define OFILL   0000100
#define OFDEL   0000200
#define NLDLY   0000400
#define  NL0    0000000
#define  NL1    0000400
#define CRDLY   0003000
#define  CR0    0000000
#define  CR1    0001000
#define  CR2    0002000
#define  CR3    0003000
#define TABDLY  0014000
#define  TAB0   0000000
#define  TAB1   0004000
#define  TAB2   0010000
#define  TAB3   0014000
#define BSDLY   0020000
#define  BS0    0000000
#define  BS1    0020000
#define FFDLY   0100000
#define  FF0    0000000
#define  FF1    0100000
#define VTDLY   0040000
#define  VT0    0000000
#define  VT1    0040000
#define XTABS   0014000

// ── termios c_cflag bits ──────────────────────────────────────────────────
#define CBAUD   0010017
#define  B0     0000000
#define  B50    0000001
#define  B75    0000002
#define  B110   0000003
#define  B134   0000004
#define  B150   0000005
#define  B200   0000006
#define  B300   0000007
#define  B600   0000010
#define  B1200  0000011
#define  B1800  0000012
#define  B2400  0000013
#define  B4800  0000014
#define  B9600  0000015
#define  B19200 0000016
#define  B38400 0000017
#define CSIZE   0000060
#define  CS5    0000000
#define  CS6    0000020
#define  CS7    0000040
#define  CS8    0000060
#define CSTOPB  0000100
#define CREAD   0000200
#define PARENB  0000400
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000

// ── termios c_lflag bits ──────────────────────────────────────────────────
#define ISIG    0000001
#define ICANON  0000002
#define XCASE   0000004
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOPRT 0002000
#define ECHOKE  0004000
#define FLUSHO  0010000
#define PENDIN  0040000
#define IEXTEN  0100000
#define EXTPROC 0200000

// ── termios VMIN/VTIME/VINTR/... indices ─────────────────────────────────
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP   10
#define VEOL    11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2   16
#define NCCS     19

// ── termios struct ────────────────────────────────────────────────────────
typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
} termios_t;

// ── winsize struct ────────────────────────────────────────────────────────
typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} winsize_t;

// ── utsname struct ───────────────────────────────────────────────────────
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} utsname_t;

// ── tms struct ───────────────────────────────────────────────────────────
typedef struct {
    uint64_t tms_utime;
    uint64_t tms_stime;
    uint64_t tms_cutime;
    uint64_t tms_cstime;
} tms_t;

// ── rusage struct ────────────────────────────────────────────────────────
typedef struct {
    uint64_t ru_utime_sec;
    uint64_t ru_utime_usec;
    uint64_t ru_stime_sec;
    uint64_t ru_stime_usec;
    uint64_t ru_maxrss;
    uint64_t ru_ixrss;
    uint64_t ru_idrss;
    uint64_t ru_isrss;
    uint64_t ru_minflt;
    uint64_t ru_majflt;
    uint64_t ru_nswap;
    uint64_t ru_inblock;
    uint64_t ru_oublock;
    uint64_t ru_msgsnd;
    uint64_t ru_msgrcv;
    uint64_t ru_nsignals;
    uint64_t ru_nvcsw;
    uint64_t ru_nivcsw;
} rusage_t;

// ── poll structures ───────────────────────────────────────────────────────
#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

typedef struct {
    int32_t  fd;
    uint16_t events;
    uint16_t revents;
} pollfd_t;

// ── select fd_set ─────────────────────────────────────────────────────────
#define FD_SETSIZE 1024
typedef struct {
    uint64_t bits[FD_SETSIZE / 64];
} fd_set_t;

#define FD_ISSET(fd, setp) \
    (((setp)->bits[(fd)/64] >> ((fd)%64)) & 1)

// stat_t removed — use struct stat (POSIX) from libc.h

#define SYS_READ_NONBLOCK 1
