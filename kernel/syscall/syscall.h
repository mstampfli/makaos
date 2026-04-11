#pragma once
#include "common.h"

// ── Syscall numbers ───────────────────────────────────────────────────────
#define SYS_WRITE   0   // write(fd, buf, len)        → bytes written
#define SYS_EXIT    1   // exit(code)                  → does not return
#define SYS_READ    2   // read(fd, buf, len, flags)   → bytes read
#define SYS_OPEN    3   // open(path_ptr, flags, mode)  → fd, or -errno on error
#define SYS_CLOSE   4   // close(fd)                   → 0 or -1
#define SYS_BRK     5   // brk(new_brk)               → new brk, or -1 on error
#define SYS_KILL    6   // kill(pid, sig)              → 0 or -1
#define SYS_FORK    7   // fork()                      → child pid in parent, 0 in child
#define SYS_EXEC    8   // exec(name, len)             → doesn't return on success
#define SYS_WAIT    9   // wait(pid)                   → 0 when child exits
#define SYS_GETPID  10  // getpid()                    → current pid
#define SYS_READDIR 11  // readdir(path,pathlen,buf,max) → entry count or -1
#define SYS_SPAWN   12  // spawn(path, pathlen)        → child pid, or -1
#define SYS_THREAD  13  // thread(entry, stack_top)    → tid, or -1
#define SYS_CLOCK_NS 14 // clock_ns()                  → nanoseconds since boot
#define SYS_STAT    15  // stat(path_ptr, pathlen, stat_ptr) → 0 or -1
#define SYS_UNLINK  16  // unlink(path_ptr, pathlen)   → 0 or -1
#define SYS_RENAME  17  // rename(src_ptr,srclen,dst_ptr,dstlen) → 0 or -1
#define SYS_GETCWD  18  // getcwd(buf_ptr, buflen)     → 0 or -1
#define SYS_CHDIR   19  // chdir(path_ptr, pathlen)    → 0 or -1
#define SYS_MKDIR    20  // mkdir(path_ptr, pathlen)            → 0 or -errno
#define SYS_LSEEK    21  // lseek(fd, offset, whence)           → new offset or -errno
#define SYS_GETPPID  22  // getppid()                           → parent pid
#define SYS_DUP      23  // dup(oldfd)                          → new fd or -errno
#define SYS_DUP2     24  // dup2(oldfd, newfd)                  → newfd or -errno
#define SYS_PIPE     25  // pipe(fds[2])                        → 0 or -errno
#define SYS_SIGACTION 26 // sigaction(sig, handler, old)        → 0 or -errno
#define SYS_SIGPROCMASK 27 // sigprocmask(how, set, oldset)     → 0 or -errno
#define SYS_SIGRETURN 28 // sigreturn() — restore after handler → (no return)
#define SYS_MMAP     29  // mmap(addr,len,prot,flags,fd,off)  → addr or -errno
#define SYS_MUNMAP   30  // munmap(addr,len)                   → 0 or -errno
#define SYS_NANOSLEEP 31 // nanosleep(req,rem)                 → 0 or -errno
#define SYS_GETTOD   32  // gettimeofday(tv,tz)                → 0
#define SYS_FB_BLIT  33  // fb_blit(src,w,h,flags)            → 0 or -errno
#define SYS_FB_INFO  34  // fb_info(fb_info_user_t*)           → 0

// ── Socket syscalls ───────────────────────────────────────────────────────
#define SYS_SOCKET     35  // socket(domain, type, proto)         → fd or -errno
#define SYS_BIND       36  // bind(fd, sockaddr*, addrlen)        → 0 or -errno
#define SYS_LISTEN     37  // listen(fd, backlog)                 → 0 or -errno
#define SYS_ACCEPT     38  // accept(fd, sockaddr*, addrlen*)     → fd or -errno
#define SYS_CONNECT    39  // connect(fd, sockaddr*, addrlen)     → 0 or -errno
#define SYS_SENDTO     40  // sendto(fd,buf,len,flags,addr,alen)  → bytes or -errno
#define SYS_RECVFROM   41  // recvfrom(fd,buf,len,flags,addr,alen*)→ bytes or -errno
#define SYS_SETSOCKOPT 42  // setsockopt(fd,lvl,opt,val,vlen)    → 0 or -errno
#define SYS_SHUTDOWN   43  // shutdown(fd, how)                   → 0 or -errno

// ── POSIX syscalls for shell / bash compatibility ─────────────────────────
#define SYS_FCNTL      44  // fcntl(fd, cmd, arg)                 → varies
#define SYS_FSTAT      45  // fstat(fd, stat_t*)                  → 0 or -errno
#define SYS_ACCESS     46  // access(path, mode)                  → 0 or -errno
#define SYS_UNAME      47  // uname(utsname*)                     → 0 or -errno
#define SYS_UMASK      48  // umask(mask)                         → old mask
#define SYS_GETUID     49  // getuid()                            → uid
#define SYS_GETEUID    50  // geteuid()                           → euid
#define SYS_GETGID     51  // getgid()                            → gid
#define SYS_GETEGID    52  // getegid()                           → egid
#define SYS_GETGROUPS  53  // getgroups(size, list)               → count
#define SYS_SETPGID    54  // setpgid(pid, pgid)                  → 0 or -errno
#define SYS_GETPGID    55  // getpgid(pid)                        → pgid or -errno
#define SYS_GETPGRP    56  // getpgrp()                           → pgid
#define SYS_SETSID     57  // setsid()                            → sid or -errno
#define SYS_GETSID     58  // getsid(pid)                         → sid or -errno
#define SYS_IOCTL      59  // ioctl(fd, request, arg)             → varies
#define SYS_SELECT     60  // select(nfds,rd,wr,ex,tv)            → count or -errno
#define SYS_POLL       61  // poll(fds, nfds, timeout_ms)         → count or -errno
#define SYS_READLINK   62  // readlink(path, buf, bufsz)          → len or -errno
#define SYS_SYMLINK    63  // symlink(target, linkpath)           → 0 or -errno
#define SYS_LINK       64  // link(oldpath, newpath)              → 0 or -errno
#define SYS_CHMOD      65  // chmod(path, mode)                   → 0 or -errno
#define SYS_FCHMOD     66  // fchmod(fd, mode)                    → 0 or -errno
#define SYS_CHOWN      67  // chown(path, uid, gid)               → 0 or -errno
#define SYS_FCHOWN     68  // fchown(fd, uid, gid)                → 0 or -errno
#define SYS_TRUNCATE   69  // truncate(path, length)              → 0 or -errno
#define SYS_FTRUNCATE  70  // ftruncate(fd, length)               → 0 or -errno
#define SYS_TIMES      71  // times(tms*)                         → clock ticks
#define SYS_GETRUSAGE  72  // getrusage(who, rusage*)             → 0 or -errno
#define SYS_TCGETPGRP  73  // tcgetpgrp(fd)                       → pgid or -errno
#define SYS_TCSETPGRP  74  // tcsetpgrp(fd, pgid)                 → 0 or -errno
#define SYS_REBOOT     75  // reboot()                            → does not return

// ── Security syscalls ─────────────────────────────────────────────────────
#define SYS_SETUID    76  // setuid(uid)                         → 0 or -errno
#define SYS_SETGID    77  // setgid(gid)                         → 0 or -errno
#define SYS_SETEUID   78  // seteuid(euid)                       → 0 or -errno
#define SYS_SETEGID   79  // setegid(egid)                       → 0 or -errno
#define SYS_SETREUID  80  // setreuid(ruid, euid)                → 0 or -errno
#define SYS_SETREGID  81  // setregid(rgid, egid)                → 0 or -errno
#define SYS_SETGROUPS 82  // setgroups(size, list)               → 0 or -errno
#define SYS_PLEDGE    83  // pledge(mask)                        → 0 or -errno
#define SYS_UNVEIL    84  // unveil(path, pathlen, perms)        → 0 or -errno
#define SYS_UNVEIL_LOCK 85 // unveil_lock()                      → 0
#define SYS_RESTRICT_FD 86 // restrict_fd(fd, rights_mask)       → 0 or -errno
#define SYS_SENDFD    87  // sendfd(sock_fd, target_fd, rights)  → 0 or -errno
#define SYS_RECVFD    88  // recvfd(sock_fd)                     → fd or -errno
#define SYS_REGISTER_POLICY_AGENT 89 // register_policy_agent(rfd,wfd) → 0 or -errno

// ── Shared memory syscalls ───────────────────────────────────────────────
#define SYS_SHM_OPEN   90  // shm_open(name,namelen,oflags,mode)  → fd or -errno
#define SYS_SHM_UNLINK 91  // shm_unlink(name,namelen)            → 0 or -errno

// waitpid options
#define WNOHANG 1

// ── O_ file flags (also used by fcntl F_GETFL/F_SETFL) ───────────────────
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x040
#define O_EXCL      0x080
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_CLOEXEC   0x80000   // set FD_CLOEXEC atomically on open

// ── fcntl commands ────────────────────────────────────────────────────────
#define F_DUPFD     0   // dup fd, using lowest available >= arg
#define F_GETFD     1   // get fd flags (FD_CLOEXEC)
#define F_SETFD     2   // set fd flags
#define F_GETFL     3   // get file status flags (O_APPEND, O_NONBLOCK, access mode)
#define F_SETFL     4   // set file status flags (only O_APPEND, O_NONBLOCK changeable)
#define F_DUPFD_CLOEXEC 1030 // dup + set FD_CLOEXEC atomically

// ── access() mode bits ────────────────────────────────────────────────────
#define F_OK  0   // file exists
#define X_OK  1   // execute permission
#define W_OK  2   // write permission
#define R_OK  4   // read permission

// ── ioctl requests ────────────────────────────────────────────────────────
#define TIOCGWINSZ  0x5413   // get window size
#define TIOCSWINSZ  0x5414   // set window size
#define TIOCGPGRP   0x540F   // get foreground process group
#define TIOCSPGRP   0x5410   // set foreground process group
#define TCGETS      0x5401   // get termios
#define TCSETS      0x5402   // set termios (drain+flush)
#define TCSETSW     0x5403   // set termios (wait for drain)
#define TCSETSF     0x5404   // set termios (flush)
#define TCSBRK      0x5409   // send break
#define TCXONC      0x540A   // flow control
#define TCFLSH      0x540B   // discard input/output
#define TIOCEXCL    0x540C   // exclusive use
#define TIOCNXCL    0x540D   // non-exclusive
#define TIOCSCTTY   0x540E   // set controlling terminal
#define TIOCGPTN    0x80045430  // get pty number
#define TIOCSPTLCK  0x40045431  // lock/unlock pty
#define TIOCGSERIAL 0x541E   // get serial info (stub)
#define TIOCSTI     0x5412   // insert char into input queue

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

// ── winsize struct (TIOCGWINSZ / TIOCSWINSZ) ─────────────────────────────
typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} winsize_t;

// ── utsname struct (uname syscall) ───────────────────────────────────────
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} utsname_t;

// ── tms struct (times syscall) ───────────────────────────────────────────
typedef struct {
    uint64_t tms_utime;   // user time (clock ticks)
    uint64_t tms_stime;   // system time
    uint64_t tms_cutime;  // children user time
    uint64_t tms_cstime;  // children system time
} tms_t;

// ── rusage struct (getrusage syscall) ────────────────────────────────────
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

// ── select fd_set (simplified: up to 1024 fds, 16 × uint64_t) ────────────
#define FD_SETSIZE 1024
typedef struct {
    uint64_t bits[FD_SETSIZE / 64];
} fd_set_t;

#define FD_ISSET(fd, setp) \
    (((setp)->bits[(fd)/64] >> ((fd)%64)) & 1)

// ── stat_t / struct stat (POSIX, for SYS_STAT / SYS_FSTAT) ──────────────
// Filled by sys_stat / sys_fstat and written to userspace.
// Layout must match stat_t / struct stat in userland/libc/libc.h exactly.
// Matches userland struct stat exactly (st_atim etc. are struct { int64 sec; int64 nsec }).
typedef struct k_timespec { int64_t tv_sec; int64_t tv_nsec; } k_timespec_t;
typedef struct stat {
    uint64_t     st_ino;
    uint64_t     st_nlink;
    uint32_t     st_mode;
    uint32_t     st_uid;
    uint32_t     st_gid;
    uint32_t     _pad0;
    uint64_t     st_size;
    k_timespec_t st_atim;
    k_timespec_t st_mtim;
    k_timespec_t st_ctim;
    uint64_t     st_blksize;
    int64_t      st_blocks;
    int32_t      st_dev;
    int32_t      st_rdev;
} stat_t;

#define SYS_READ_NONBLOCK 1

// ── Calling convention (System V AMD64, mirroring Linux) ─────────────────
// rax = syscall number
// rdi = arg1, rsi = arg2, rdx = arg3
// rcx and r11 are clobbered by syscall/sysret (CPU saves user rip/rflags there)
// return value in rax

// ── Globals exposed to syscall_entry.asm ─────────────────────────────────
extern uint64_t    g_syscall_user_rsp;
extern uint64_t    g_syscall_user_rip;
extern uint64_t    g_syscall_user_rflags;
extern uint64_t    g_syscall_arg5;
extern uint64_t    g_syscall_arg6;
extern uint8_t     g_exec_requested;
extern uint64_t    g_exec_entry;
extern uint64_t    g_exec_rsp;
extern phys_addr_t g_exec_pml4;

// ── Init ──────────────────────────────────────────────────────────────────
// Write STAR/LSTAR/SFMASK MSRs and enable SCE in EFER.
// Call once after tss_init().
void syscall_init(void);

// ── Dispatcher (called from assembly stub) ────────────────────────────────
uint64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);

// ── Native dispatcher (our internal syscall numbers) ──────────────────────
uint64_t native_syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);

// ── Entry point (defined in syscall_entry.asm) ────────────────────────────
extern void syscall_entry(void);
