#pragma once

// ── errno constants ───────────────────────────────────────────────────────
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define EBADF        9
#define ECHILD      10
#define EAGAIN      11
#define ENOMEM      12
#define EACCES      13
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define ENOSPC      28
#define ERANGE      34
#define ENOTEMPTY   39
#define ENOSYS      38
#define ENOEXEC      8
#define EPIPE       32

// ── errno global ──────────────────────────────────────────────────────────
extern int errno;

// ── Basic types ───────────────────────────────────────────────────────────
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
typedef uint64_t           size_t;
typedef int64_t            ssize_t;

#define NULL ((void*)0)

// ── Syscall numbers ───────────────────────────────────────────────────────
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

#define WNOHANG 1

#define SYS_READ_NONBLOCK 1

// ── Signal numbers ────────────────────────────────────────────────────────
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17

// ── Signal action constants ───────────────────────────────────────────────
#define SIG_DFL  ((void(*)(int))0)
#define SIG_IGN  ((void(*)(int))1)

// ── sigprocmask how values ────────────────────────────────────────────────
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

// ── sigaction flags ───────────────────────────────────────────────────────
#define SA_RESTORER  0x04000000

// ── Structures ────────────────────────────────────────────────────────────

typedef struct {
    char     name[256];
    uint32_t inode_num;
    uint32_t size;
    uint8_t  is_dir;
} dirent_t;

// Must match k_sigaction_t in kernel/signal.h exactly.
typedef struct {
    void     (*sa_handler)(int);
    void     (*sa_restorer)(void);
    uint32_t  sa_mask;
    uint32_t  sa_flags;
} struct_sigaction;

typedef struct {
    uint32_t ino;
    uint32_t size;
    uint16_t mode;
    uint8_t  is_dir;
    uint8_t  _pad;
} stat_t;

// ── Raw syscall ───────────────────────────────────────────────────────────

static inline uint64_t syscall4(uint64_t nr, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4) {
    uint64_t ret;
    __asm__ volatile(
        "mov %5, %%r10\n\t"
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(a4)
        : "rcx", "r11", "r10", "memory"
    );
    return ret;
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

// ── errno-aware return helper ─────────────────────────────────────────────
// Kernel returns -(errno) on failure (Linux convention).
// If the raw return value is in (-4096, 0), set errno and return -1.
static inline long __syscall_ret(uint64_t r) {
    long v = (long)r;
    if (v < 0 && v > -4096) { errno = (int)-v; return -1; }
    return v;
}

// ── I/O ──────────────────────────────────────────────────────────────────

static inline ssize_t write(int fd, const void* buf, size_t len) {
    return (ssize_t)__syscall_ret(syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, len));
}

static inline ssize_t read(int fd, void* buf, size_t len) {
    return (ssize_t)__syscall_ret(syscall3(SYS_READ, (uint64_t)fd, (uint64_t)buf, len));
}

static inline ssize_t read_nonblock(int fd, void* buf, size_t len) {
    return (ssize_t)__syscall_ret(syscall4(SYS_READ, (uint64_t)fd, (uint64_t)buf, len, SYS_READ_NONBLOCK));
}

// open() flags (POSIX)
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x040
#define O_EXCL    0x080
#define O_TRUNC   0x200
#define O_APPEND  0x400

static inline int open(const char* path, int flags, ...) {
    return (int)__syscall_ret(syscall3(SYS_OPEN, (uint64_t)path, (uint64_t)flags, 0));
}

static inline int close(int fd) {
    return (int)__syscall_ret(syscall1(SYS_CLOSE, (uint64_t)fd));
}

// ── Process ───────────────────────────────────────────────────────────────

__attribute__((noreturn)) static inline void exit(int code) {
    syscall1(SYS_EXIT, (uint64_t)code);
    for (;;);
}

static inline int fork(void) {
    return (int)__syscall_ret(syscall0(SYS_FORK));
}

static inline int exec(const char* path, size_t pathlen) {
    return (int)__syscall_ret(syscall2(SYS_EXEC, (uint64_t)path, pathlen));
}

static inline int spawn(const char* path, size_t pathlen) {
    return (int)__syscall_ret(syscall2(SYS_SPAWN, (uint64_t)path, pathlen));
}

// Macros for decoding the status filled by wait/waitpid.
#define WIFEXITED(s)   (((s) & 0xFF) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

// POSIX waitpid: wait for pid (-1 = any child), fill *status, options (WNOHANG).
static inline int waitpid(int pid, int* status, int options) {
    return (int)__syscall_ret(syscall3(SYS_WAIT, (uint64_t)(int64_t)pid,
                                       (uint64_t)status, (uint64_t)options));
}

// POSIX wait: wait for any child.
static inline int wait(int* status) {
    return waitpid(-1, status, 0);
}

static inline int getpid(void) {
    return (int)syscall0(SYS_GETPID);   // never fails
}

static inline int getppid(void) {
    return (int)syscall0(SYS_GETPPID);  // never fails
}

// _exit: same as exit (no stdio buffers to flush in our libc).
__attribute__((noreturn)) static inline void _exit(int code) {
    syscall1(SYS_EXIT, (uint64_t)code);
    for (;;);
}

static inline int kill(int pid, int sig) {
    return (int)__syscall_ret(syscall2(SYS_KILL, (uint64_t)pid, (uint64_t)sig));
}

static inline uint64_t thread(uint64_t entry, uint64_t stack_top, uint64_t flags) {
    return (uint64_t)__syscall_ret(syscall3(SYS_THREAD, entry, stack_top, flags));
}

static inline uint64_t clock_ns(void) {
    return syscall0(SYS_CLOCK_NS);  // never fails
}

static inline int pipe(int fds[2]) {
    return (int)__syscall_ret(syscall1(SYS_PIPE, (uint64_t)fds));
}

static inline int dup(int oldfd) {
    return (int)__syscall_ret(syscall1(SYS_DUP, (uint64_t)oldfd));
}

static inline int dup2(int oldfd, int newfd) {
    return (int)__syscall_ret(syscall2(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd));
}

// ── Signals ───────────────────────────────────────────────────────────────

// Restorer trampoline: signal handler executes `ret` into this, which calls
// SYS_SIGRETURN to restore the interrupted context.  Must be non-static so
// its address is stable and can be passed as sa_restorer.
void __sigreturn_trampoline(void);

static inline int sigaction(int sig, const struct_sigaction* act,
                            struct_sigaction* oldact) {
    return (int)__syscall_ret(syscall3(SYS_SIGACTION,
                                       (uint64_t)(uint32_t)sig,
                                       (uint64_t)act,
                                       (uint64_t)oldact));
}

static inline int sigprocmask(int how, const uint32_t* set, uint32_t* oldset) {
    return (int)__syscall_ret(syscall3(SYS_SIGPROCMASK,
                                       (uint64_t)(uint32_t)how,
                                       (uint64_t)set,
                                       (uint64_t)oldset));
}

// ── Filesystem ───────────────────────────────────────────────────────────

static inline int stat(const char* path, size_t pathlen, stat_t* st) {
    return (int)__syscall_ret(syscall3(SYS_STAT, (uint64_t)path, pathlen, (uint64_t)st));
}

static inline int unlink(const char* path, size_t pathlen) {
    return (int)__syscall_ret(syscall2(SYS_UNLINK, (uint64_t)path, pathlen));
}

static inline int sys_rename(const char* src, size_t srclen, const char* dst, size_t dstlen) {
    return (int)__syscall_ret(syscall4(SYS_RENAME, (uint64_t)src, srclen, (uint64_t)dst, dstlen));
}
// Default rename macro: 4-arg form for our own code.
// The doom include shim overrides this with a 2-arg POSIX version.
#ifndef _DOOM_RENAME_OVERRIDE
#define rename(src, srclen, dst, dstlen) sys_rename(src, srclen, dst, dstlen)
#endif

static inline int getcwd(char* buf, size_t buflen) {
    return (int)__syscall_ret(syscall2(SYS_GETCWD, (uint64_t)buf, buflen));
}

static inline int chdir(const char* path, size_t pathlen) {
    return (int)__syscall_ret(syscall2(SYS_CHDIR, (uint64_t)path, pathlen));
}

static inline int mkdir(const char* path, size_t pathlen) {
    return (int)__syscall_ret(syscall2(SYS_MKDIR, (uint64_t)path, pathlen));
}

static inline int readdir(const char* path, size_t pathlen, dirent_t* buf, int max) {
    return (int)__syscall_ret(syscall4(SYS_READDIR, (uint64_t)path, pathlen, (uint64_t)buf, (uint64_t)max));
}

static inline uint64_t brk(uint64_t new_brk) {
    return syscall1(SYS_BRK, new_brk);  // returns address, not error code
}

// lseek whence values
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static inline long lseek(int fd, long offset, int whence) {
    return (long)__syscall_ret(syscall3(SYS_LSEEK, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence));
}

// ── String functions ──────────────────────────────────────────────────────

size_t strlen(const char* s);
void*  memset(void* dst, int c, size_t n);
void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
int    strcasecmp(const char* a, const char* b);
int    strncasecmp(const char* a, const char* b, size_t n);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, size_t n);
char*  strcat(char* dst, const char* src);
char*  strncat(char* dst, const char* src, size_t n);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);
char*  strstr(const char* haystack, const char* needle);
char*  strdup(const char* s);
char*  strndup(const char* s, size_t max);
long   strtol(const char* s, char** endptr, int base);
unsigned long strtoul(const char* s, char** endptr, int base);
long   atoi(const char* s);

// ── printf / scanf ────────────────────────────────────────────────────────

int printf(const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vsnprintf(char* buf, size_t size, const char* fmt, __builtin_va_list ap);
int sscanf(const char* str, const char* fmt, ...);
int puts(const char* s);
int putchar(int c);

// ── malloc / free ─────────────────────────────────────────────────────────

void* malloc(size_t size);
void  free(void* ptr);
void* realloc(void* ptr, size_t new_size);
void* calloc(size_t nmemb, size_t size);

// ── Math helpers ──────────────────────────────────────────────────────────

int    abs(int x);
long   labs(long x);
long long llabs(long long x);

// ── Random number generation ──────────────────────────────────────────────

int  rand(void);
void srand(unsigned int seed);
#define RAND_MAX 0x7FFFFFFF

// ── Sorting ───────────────────────────────────────────────────────────────

void qsort(void* base, size_t nmemb, size_t size,
           int (*cmp)(const void*, const void*));

// ── Time ──────────────────────────────────────────────────────────────────

typedef long long time_t;
typedef long      clock_t;
#define CLOCKS_PER_SEC 1000

time_t  time(time_t* tloc);
clock_t clock(void);

// ── POSIX timespec / nanosleep ────────────────────────────────────────────

typedef struct { uint64_t tv_sec; uint64_t tv_nsec; } timespec_t;

int nanosleep(const timespec_t* req, timespec_t* rem);

// ── mmap / munmap ─────────────────────────────────────────────────────────

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANON    0x20
#define MAP_ANONYMOUS MAP_ANON

#define MAP_FAILED ((void*)-1)

void* mmap(void* addr, size_t len, int prot, int flags, int fd, long off);
int   munmap(void* addr, size_t len);

// ── New syscall numbers ───────────────────────────────────────────────────
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

// ── Extra errno values ────────────────────────────────────────────────────
#define EFAULT      14
#define EBUSY       16
#define EMFILE      24
#define ENOTTY      25
#define EFBIG       27
#define ESPIPE      29
#define EDEADLK     35
#define ENAMETOOLONG 36
#define ELOOP       40
#define EWOULDBLOCK EAGAIN
#define ENOTSOCK    88
#define EOPNOTSUPP  95
#define EAFNOSUPPORT 97
#define EADDRINUSE  98
#define ECONNRESET  104
#define ENOBUFS     105
#define EISCONN     106
#define ENOTCONN    107
#define ETIMEDOUT   110

// ── O_ flags for open / fcntl ─────────────────────────────────────────────
#define O_NONBLOCK  0x800
#define O_CLOEXEC   0x80000

// ── fcntl commands ────────────────────────────────────────────────────────
#define FD_CLOEXEC  1
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_DUPFD_CLOEXEC 1030

static inline int fcntl(int fd, int cmd, long arg) {
    return (int)__syscall_ret(syscall3(SYS_FCNTL, (uint64_t)fd,
                                       (uint64_t)cmd, (uint64_t)arg));
}

// ── access() mode bits ────────────────────────────────────────────────────
#define F_OK  0
#define X_OK  1
#define W_OK  2
#define R_OK  4

static inline int access(const char* path, int mode) {
    return (int)__syscall_ret(syscall2(SYS_ACCESS, (uint64_t)path, (uint64_t)mode));
}

// ── fstat ─────────────────────────────────────────────────────────────────
static inline int fstat(int fd, stat_t* st) {
    return (int)__syscall_ret(syscall2(SYS_FSTAT, (uint64_t)fd, (uint64_t)st));
}

// ── uname ─────────────────────────────────────────────────────────────────
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} utsname_t;

static inline int uname(utsname_t* buf) {
    return (int)__syscall_ret(syscall1(SYS_UNAME, (uint64_t)buf));
}

// ── umask ─────────────────────────────────────────────────────────────────
#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned int mode_t;
#endif
static inline mode_t umask(mode_t mask) {
    return (mode_t)syscall1(SYS_UMASK, (uint64_t)mask);
}

// ── Identity ─────────────────────────────────────────────────────────────
#ifndef _UID_T_DEFINED
#define _UID_T_DEFINED
typedef unsigned int uid_t;
#endif
#ifndef _GID_T_DEFINED
#define _GID_T_DEFINED
typedef unsigned int gid_t;
#endif

static inline uid_t getuid(void)  { return (uid_t)syscall0(SYS_GETUID); }
static inline uid_t geteuid(void) { return (uid_t)syscall0(SYS_GETEUID); }
static inline gid_t getgid(void)  { return (gid_t)syscall0(SYS_GETGID); }
static inline gid_t getegid(void) { return (gid_t)syscall0(SYS_GETEGID); }
static inline int getgroups(int sz, gid_t* list) {
    return (int)__syscall_ret(syscall2(SYS_GETGROUPS, (uint64_t)sz, (uint64_t)list));
}

// ── Process groups / sessions ─────────────────────────────────────────────
#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int pid_t;
#endif

static inline int setpgid(pid_t pid, pid_t pgid) {
    return (int)__syscall_ret(syscall2(SYS_SETPGID, (uint64_t)pid, (uint64_t)pgid));
}
static inline pid_t getpgid(pid_t pid) {
    return (pid_t)__syscall_ret(syscall1(SYS_GETPGID, (uint64_t)pid));
}
static inline pid_t getpgrp(void) {
    return (pid_t)syscall0(SYS_GETPGRP);
}
static inline pid_t setsid(void) {
    return (pid_t)__syscall_ret(syscall0(SYS_SETSID));
}
static inline pid_t getsid(pid_t pid) {
    return (pid_t)__syscall_ret(syscall1(SYS_GETSID, (uint64_t)pid));
}

// ── ioctl ─────────────────────────────────────────────────────────────────
static inline int ioctl(int fd, unsigned long req, void* arg) {
    return (int)__syscall_ret(syscall3(SYS_IOCTL, (uint64_t)fd,
                                       (uint64_t)req, (uint64_t)arg));
}

// ── termios ───────────────────────────────────────────────────────────────
#define NCCS 19
typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
} termios_t;

// c_lflag bits
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define IEXTEN  0100000

// c_iflag bits
#define ICRNL   0000400
#define IXON    0002000
#define IXOFF   0010000

// c_oflag bits
#define OPOST   0000001
#define ONLCR   0000004

// termios baud rates
#define B0      0
#define B9600   0000015
#define B38400  0000017

// termios VMIN/VTIME indices
#define VMIN    6
#define VTIME   5
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VSUSP   10

// ioctl requests
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

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} winsize_t;

static inline int tcgetattr(int fd, termios_t* t) {
    return ioctl(fd, TCGETS, t);
}
static inline int tcsetattr(int fd, int action, const termios_t* t) {
    unsigned long req = (action == 0) ? TCSETS : (action == 1) ? TCSETSW : TCSETSF;
    return ioctl(fd, req, (void*)t);
}
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

static inline pid_t tcgetpgrp(int fd) {
    return (pid_t)__syscall_ret(syscall1(SYS_TCGETPGRP, (uint64_t)fd));
}
static inline int tcsetpgrp(int fd, pid_t pgid) {
    return (int)__syscall_ret(syscall2(SYS_TCSETPGRP, (uint64_t)fd, (uint64_t)pgid));
}

// isatty: check if fd refers to a terminal (always true for 0/1/2 on our OS)
static inline int isatty(int fd) {
    termios_t t;
    return (tcgetattr(fd, &t) == 0) ? 1 : 0;
}

// ── select ────────────────────────────────────────────────────────────────
#define FD_SETSIZE 1024
typedef struct { uint64_t bits[FD_SETSIZE/64]; } fd_set;

#define FD_ZERO(s)   do { for(int _i=0;_i<(int)(FD_SETSIZE/64);_i++) (s)->bits[_i]=0; } while(0)
#define FD_SET(fd,s) ((s)->bits[(fd)/64] |= (1ULL<<((fd)%64)))
#define FD_CLR(fd,s) ((s)->bits[(fd)/64] &= ~(1ULL<<((fd)%64)))
#define FD_ISSET(fd,s) (((s)->bits[(fd)/64]>>((fd)%64))&1)

typedef struct { int64_t tv_sec; int64_t tv_usec; } timeval_t;

static inline int select(int nfds, fd_set* rfds, fd_set* wfds,
                          fd_set* efds, timeval_t* tv) {
    // Pass 5th arg via r8 (g_syscall_arg5 in kernel).
    uint64_t ret;
    __asm__ volatile(
        "mov %6, %%r8\n\t"
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)SYS_SELECT),
          "D"((uint64_t)nfds), "S"((uint64_t)rfds),
          "d"((uint64_t)wfds), "r"((uint64_t)efds),
          "r"((uint64_t)tv)
        : "rcx", "r11", "r8", "memory"
    );
    return (int)__syscall_ret(ret);
}

// ── poll ─────────────────────────────────────────────────────────────────
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

static inline int poll(pollfd_t* fds, uint32_t nfds, int timeout_ms) {
    return (int)__syscall_ret(syscall3(SYS_POLL, (uint64_t)fds,
                                       (uint64_t)nfds, (uint64_t)timeout_ms));
}

// ── readlink / symlink / link ─────────────────────────────────────────────
static inline ssize_t readlink(const char* path, char* buf, size_t bufsz) {
    return (ssize_t)__syscall_ret(syscall3(SYS_READLINK, (uint64_t)path,
                                            (uint64_t)buf, (uint64_t)bufsz));
}
static inline int symlink(const char* target, const char* linkpath) {
    return (int)__syscall_ret(syscall2(SYS_SYMLINK, (uint64_t)target, (uint64_t)linkpath));
}
static inline int link(const char* old, const char* newp) {
    return (int)__syscall_ret(syscall2(SYS_LINK, (uint64_t)old, (uint64_t)newp));
}

// ── chmod / chown / fchmod / fchown ──────────────────────────────────────
static inline int chmod(const char* path, mode_t mode) {
    return (int)__syscall_ret(syscall2(SYS_CHMOD, (uint64_t)path, (uint64_t)mode));
}
static inline int fchmod(int fd, mode_t mode) {
    return (int)__syscall_ret(syscall2(SYS_FCHMOD, (uint64_t)fd, (uint64_t)mode));
}
static inline int chown(const char* path, uid_t uid, gid_t gid) {
    return (int)__syscall_ret(syscall3(SYS_CHOWN, (uint64_t)path, (uint64_t)uid, (uint64_t)gid));
}
static inline int fchown(int fd, uid_t uid, gid_t gid) {
    return (int)__syscall_ret(syscall3(SYS_FCHOWN, (uint64_t)fd, (uint64_t)uid, (uint64_t)gid));
}

// ── truncate / ftruncate ──────────────────────────────────────────────────
static inline int truncate(const char* path, long length) {
    return (int)__syscall_ret(syscall2(SYS_TRUNCATE, (uint64_t)path, (uint64_t)length));
}
static inline int ftruncate(int fd, long length) {
    return (int)__syscall_ret(syscall2(SYS_FTRUNCATE, (uint64_t)fd, (uint64_t)length));
}

// ── times / getrusage ─────────────────────────────────────────────────────
typedef struct {
    uint64_t tms_utime;
    uint64_t tms_stime;
    uint64_t tms_cutime;
    uint64_t tms_cstime;
} tms_t;

typedef struct {
    uint64_t ru_utime_sec, ru_utime_usec;
    uint64_t ru_stime_sec, ru_stime_usec;
    uint64_t ru_maxrss;
    uint64_t ru_ixrss, ru_idrss, ru_isrss;
    uint64_t ru_minflt, ru_majflt;
    uint64_t ru_nswap, ru_inblock, ru_oublock;
    uint64_t ru_msgsnd, ru_msgrcv;
    uint64_t ru_nsignals, ru_nvcsw, ru_nivcsw;
} rusage_t;

static inline uint64_t times(tms_t* buf) {
    return syscall1(SYS_TIMES, (uint64_t)buf);
}
static inline int getrusage(int who, rusage_t* usage) {
    return (int)__syscall_ret(syscall2(SYS_GETRUSAGE, (uint64_t)who, (uint64_t)usage));
}

// ── execve ────────────────────────────────────────────────────────────────
// Full POSIX execve: replaces process image with new ELF.
// argv and envp are NULL-terminated arrays of C strings.
static inline int execve(const char* path, const char* const* argv,
                          const char* const* envp) {
    return (int)__syscall_ret(syscall3(SYS_EXEC, (uint64_t)path,
                                       (uint64_t)argv, (uint64_t)envp));
}

// execv: like execve but inherits envp = {NULL}.
static inline int execv(const char* path, const char* const* argv) {
    const char* empty[1] = { NULL };
    return execve(path, argv, empty);
}

// execvp: like execv but searches PATH.
// Simplified: try the path as-is, then /bin/<path>.
static inline int execvp(const char* file, const char* const* argv) {
    int r = execve(file, argv, (const char* const*)0);
    if (r == -1 && errno == ENOENT && file[0] != '/') {
        // Try /bin/<file>.
        char full[256];
        const char* pfx = "/bin/";
        int i = 0;
        while (pfx[i]) { full[i] = pfx[i]; i++; }
        int j = 0;
        while (file[j] && i < 255) { full[i++] = file[j++]; }
        full[i] = '\0';
        r = execve(full, argv, (const char* const*)0);
    }
    return r;
}
