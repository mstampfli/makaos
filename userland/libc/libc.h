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
#define E2BIG        7
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
typedef int64_t            intmax_t;
typedef uint64_t           uintmax_t;
typedef int64_t            intptr_t;
typedef uint64_t           uintptr_t;
typedef int64_t            ptrdiff_t;
typedef volatile int       sig_atomic_t;
typedef unsigned long      ulong_t;
typedef int64_t            off_t;
typedef uint32_t           socklen_t;
#ifndef _BOOL_DEFINED
#define _BOOL_DEFINED
typedef _Bool              bool;
#define true  1
#define false 0
#endif

// stdint min/max macros
#define INT8_MIN    (-128)
#define INT8_MAX    127
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define INT32_MIN   (-2147483648)
#define INT32_MAX   2147483647
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   9223372036854775807LL
#define UINT8_MAX   255U
#define UINT16_MAX  65535U
#define UINT32_MAX  4294967295U
#define UINT64_MAX  18446744073709551615ULL
#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX
#define SIZE_MAX    UINT64_MAX
#define PTRDIFF_MIN INT64_MIN
#define PTRDIFF_MAX INT64_MAX

// limits.h values
#define CHAR_BIT    8
#define CHAR_MIN    (-128)
#define CHAR_MAX    127
#define SHRT_MIN    INT16_MIN
#define SHRT_MAX    INT16_MAX
#define INT_MIN     INT32_MIN
#define INT_MAX     INT32_MAX
#define LONG_MIN    INT64_MIN
#define LONG_MAX    INT64_MAX
#define ULONG_MAX   UINT64_MAX
#define LLONG_MIN   INT64_MIN
#define LLONG_MAX   INT64_MAX
#define ULLONG_MAX  UINT64_MAX
#define PATH_MAX    4096
#define NAME_MAX    255
#define ARG_MAX     65536
#define UCHAR_MAX   255U
#define USHRT_MAX   65535U
#define BUFSIZ      4096
#define _IOFBF      0
#define _IOLBF      1
#define _IONBF      2
#define LOCALEDIR   "/usr/share/locale"
#ifndef PACKAGE
#define PACKAGE     "bash"
#endif

#define NULL ((void*)0)
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

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

// ── Security syscall numbers (match kernel/syscall/syscall.h) ─────────────
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

// ── Shared memory syscall numbers ────────────────────────────────────────
#define SYS_SHM_OPEN    90
#define SYS_SHM_UNLINK  91
#define SYS_FB_MAP      92

// ── Misc syscall numbers ──────────────────────────────────────────────────
#define SYS_GETUID      49
#define SYS_GETEUID     50
#define SYS_GETGID      51
#define SYS_GETEGID     52
#define SYS_GETGROUPS   53
#define SYS_UMASK       48
#define SYS_CHMOD       65
#define SYS_CHOWN       67

#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

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

// Must match k_sigaction_t in kernel/signal.h exactly.
// Named struct sigaction so POSIX code can use either struct sigaction or struct_sigaction.
typedef struct sigaction {
    void     (*sa_handler)(int);
    void     (*sa_restorer)(void);
    uint32_t  sa_mask;
    uint32_t  sa_flags;
} struct_sigaction;

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

// 5- and 6-argument syscalls: r8 / r9 carry args 5 and 6.  Needed by
// sendto/recvfrom/mmap, which have six parameters in the kernel ABI.
static inline uint64_t syscall6(uint64_t nr, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4,
                                  uint64_t a5, uint64_t a6) {
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    register uint64_t r9  __asm__("r9")  = a6;
    uint64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline uint64_t syscall5(uint64_t nr, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
    return syscall6(nr, a1, a2, a3, a4, a5, 0);
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

// _flush_all: flush stdout/stderr before exit.  Implemented in stdio.c.
// We can't forward-declare fflush(FILE*) here because FILE isn't defined yet.
void _flush_all(void);

__attribute__((noreturn)) static inline void exit(int code) {
    _flush_all();
    syscall1(SYS_EXIT, (uint64_t)code);
    for (;;);
}

static inline int fork(void) {
    return (int)__syscall_ret(syscall0(SYS_FORK));
}

static inline int exec(const char* path, size_t pathlen) {
    return (int)__syscall_ret(syscall2(SYS_EXEC, (uint64_t)path, pathlen));
}

// spawn(path, argv, envp, stdio) → child pid or -errno.
//   argv: NULL-terminated array of char* (NULL → synthesise {path, NULL})
//   envp: NULL-terminated array of char* (NULL → kernel default env)
//   stdio: int[3] fd specs for child's stdin/stdout/stderr
//     -1 = inherit that fd from this process
//     >=0 = dup that specific fd into the child
//     NULL → open /dev/tty0 for all three
static inline int spawn(const char* path, const char* const* argv,
                        const char* const* envp, const int stdio[3]) {
    return (int)__syscall_ret(syscall4(SYS_SPAWN, (uint64_t)path,
                                       (uint64_t)argv, (uint64_t)envp,
                                       (uint64_t)stdio));
}

// Macros for decoding the status filled by wait/waitpid.
#define WIFEXITED(s)   (((s) & 0xFF) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

// ── Credential transitions ────────────────────────────────────────────────
static inline int setuid(uint32_t uid) {
    return (int)__syscall_ret(syscall1(SYS_SETUID, uid));
}
static inline int seteuid(uint32_t euid) {
    return (int)__syscall_ret(syscall1(SYS_SETEUID, euid));
}
static inline int setgid(uint32_t gid) {
    return (int)__syscall_ret(syscall1(SYS_SETGID, gid));
}
static inline int setegid(uint32_t egid) {
    return (int)__syscall_ret(syscall1(SYS_SETEGID, egid));
}
static inline int setreuid(uint32_t ruid, uint32_t euid) {
    return (int)__syscall_ret(syscall2(SYS_SETREUID, ruid, euid));
}
static inline int setregid(uint32_t rgid, uint32_t egid) {
    return (int)__syscall_ret(syscall2(SYS_SETREGID, rgid, egid));
}
static inline int setgroups(size_t n, const uint32_t* list) {
    return (int)__syscall_ret(syscall2(SYS_SETGROUPS, n, (uint64_t)list));
}

// ── pledge() — irrevocable syscall restriction ────────────────────────────
// PLEDGE_* bitmask constants (must match kernel/security/pledge.h):
#define PLEDGE_STDIO      (1u <<  0)
#define PLEDGE_RPATH      (1u <<  1)
#define PLEDGE_WPATH      (1u <<  2)
#define PLEDGE_CPATH      (1u <<  3)
#define PLEDGE_EXEC       (1u <<  4)
#define PLEDGE_PROC       (1u <<  5)
#define PLEDGE_INET       (1u <<  6)
#define PLEDGE_UNIX       (1u <<  7)
#define PLEDGE_SIGNAL     (1u <<  8)
#define PLEDGE_THREAD     (1u <<  9)
#define PLEDGE_PROT_EXEC  (1u << 10)
#define PLEDGE_SETUID     (1u << 11)
#define PLEDGE_CHOWN      (1u << 12)
#define PLEDGE_CHMOD      (1u << 13)
#define PLEDGE_TTY        (1u << 14)
#define PLEDGE_IOCTL      (1u << 15)
#define PLEDGE_SENDFD     (1u << 16)
#define PLEDGE_ALL        (~0u)

static inline int pledge(uint32_t mask) {
    return (int)__syscall_ret(syscall1(SYS_PLEDGE, mask));
}

// ── unveil() — per-process filesystem view restriction ────────────────────
#define UNVEIL_READ    (1u << 0)
#define UNVEIL_WRITE   (1u << 1)
#define UNVEIL_EXEC    (1u << 2)
#define UNVEIL_CREATE  (1u << 3)

static inline int unveil(const char* path, uint8_t perms) {
    size_t plen = 0; while (path[plen]) plen++;
    return (int)__syscall_ret(syscall3(SYS_UNVEIL, (uint64_t)path, plen, perms));
}
static inline int unveil_lock(void) {
    return (int)__syscall_ret(syscall0(SYS_UNVEIL_LOCK));
}

// ── fd rights restriction ─────────────────────────────────────────────────
#define RIGHT_READ    (1u << 0)
#define RIGHT_WRITE   (1u << 1)
#define RIGHT_EXEC    (1u << 2)
#define RIGHT_SEEK    (1u << 3)
#define RIGHT_POLL    (1u << 4)
#define RIGHT_MMAP_R  (1u << 5)
#define RIGHT_MMAP_W  (1u << 6)
#define RIGHT_MMAP_X  (1u << 7)
#define RIGHT_IOCTL   (1u << 8)
#define RIGHT_SEND_FD (1u << 9)

static inline int restrict_fd(int fd, uint32_t rights_mask) {
    return (int)__syscall_ret(syscall2(SYS_RESTRICT_FD, (uint64_t)fd, rights_mask));
}

// ── Policy agent registration (ksec daemon only) ──────────────────────────
static inline int register_policy_agent(int read_fd, int write_fd) {
    return (int)__syscall_ret(syscall2(SYS_REGISTER_POLICY_AGENT,
                                       (uint64_t)read_fd, (uint64_t)write_fd));
}

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
    struct_sigaction fixed;
    if (act) {
        fixed = *act;
        // Always install the sigreturn trampoline as restorer so that
        // the signal handler can return normally via 'ret'.
        fixed.sa_restorer = __sigreturn_trampoline;
        act = &fixed;
    }
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

#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned int mode_t;
#endif

static inline char* getcwd(char* buf, size_t buflen) {
    int64_t ret = (int64_t)syscall2(SYS_GETCWD, (uint64_t)buf, buflen);
    if (ret < 0) { errno = (int)-ret; return (char*)0; }
    // Linux extension: buf==NULL && buflen==0 → kernel allocates and returns the pointer.
    if (!buf) return (char*)(uint64_t)ret;
    return buf;
}

// POSIX rename(old, new) — 2-arg form
static inline int rename(const char* old, const char* new) {
    size_t olen = 0, nlen = 0;
    while (old[olen]) olen++;
    while (new[nlen]) nlen++;
    return (int)__syscall_ret(syscall4(SYS_RENAME, (uint64_t)old, olen, (uint64_t)new, nlen));
}

// POSIX unlink(path)
static inline int unlink(const char* path) {
    size_t n = 0; while (path[n]) n++;
    return (int)__syscall_ret(syscall2(SYS_UNLINK, (uint64_t)path, n));
}

// POSIX chdir(path)
static inline int chdir(const char* path) {
    size_t n = 0; while (path[n]) n++;
    return (int)__syscall_ret(syscall2(SYS_CHDIR, (uint64_t)path, n));
}

// POSIX mkdir(path, mode)
static inline int mkdir(const char* path, mode_t mode) {
    (void)mode;
    size_t n = 0; while (path[n]) n++;
    return (int)__syscall_ret(syscall2(SYS_MKDIR, (uint64_t)path, n));
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
static inline void  bcopy(const void* src, void* dst, size_t n) { memmove(dst, src, n); }
static inline void  bzero(void* s, size_t n) { memset(s, 0, n); }
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
char*  strerror(int errnum);
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

// ── POSIX timeval / timespec ──────────────────────────────────────────────
// `struct timeval` / `struct timespec` for POSIX compat; timeval_t / timespec_t for our style.
typedef struct timeval  { int64_t tv_sec; int64_t tv_usec; } timeval_t;
typedef struct timespec { int64_t tv_sec; int64_t tv_nsec; } timespec_t;

// ── nanosleep ─────────────────────────────────────────────────────────────
int nanosleep(const struct timespec* req, struct timespec* rem);

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

// ── POSIX shared memory ─────────────────────────────────────────────────
int shm_open(const char* name, int oflag, int mode);
int shm_unlink(const char* name);

// ── Framebuffer mapping (root only) ─────────────────────────────────────
void* fb_map(void);

// ── Pseudo-terminal ────────────────────────────────────────────────────
// openpty(fds) → 0 or -1.  fds[0]=master, fds[1]=slave.
int openpty(int fds[2]);

// getpeerpid(fd) → pid of peer on AF_UNIX SOCK_STREAM socket, -1 on error.
// Kernel-trusted; stamped at accept/connect. Use for privileged IPC peers
// where you need to act on the peer process (e.g. compositor SIGKILL of an
// unresponsive client).
int getpeerpid(int fd);

// ── BSD Sockets ──────────────────────────────────────────────────────────

#define AF_UNIX     1
#define AF_LOCAL    AF_UNIX
#define AF_INET     2

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define SHUT_RD     0
#define SHUT_WR     1
#define SHUT_RDWR   2

#define UNIX_PATH_MAX 108

typedef struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
} sockaddr_t;

typedef struct sockaddr_un {
    uint16_t sun_family;
    char     sun_path[UNIX_PATH_MAX];
} sockaddr_un_t;

typedef struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;      // network byte order
    uint32_t sin_addr;      // network byte order
    uint8_t  sin_zero[8];
} sockaddr_in_t;

typedef uint32_t socklen_t;

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x000000FFu) << 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

// INADDR_ANY / INADDR_BROADCAST — both in network byte order.
#define INADDR_ANY        0x00000000u
#define INADDR_BROADCAST  0xFFFFFFFFu
#define INADDR_LOOPBACK   0x0100007Fu   // 127.0.0.1 (network byte order)

// setsockopt levels / options — match the kernel.
#define SOL_SOCKET     1
#define SO_DEBUG       1
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define SO_ERROR       4
#define SO_DONTROUTE   5
#define SO_BROADCAST   6
#define SO_SNDBUF      7
#define SO_RCVBUF      8
#define SO_KEEPALIVE   9
#define SO_OOBINLINE  10
#define SO_LINGER     13
#define SO_REUSEPORT  15
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21

// Flags for send/recv (currently ignored by the kernel but accepted so
// portable code compiles).
#define MSG_PEEK      0x02
#define MSG_DONTWAIT  0x40

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
int connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
ssize_t send(int fd, const void* buf, size_t len, int flags);
ssize_t recv(int fd, void* buf, size_t len, int flags);
ssize_t sendto(int fd, const void* buf, size_t len, int flags,
                const struct sockaddr* dst, socklen_t dstlen);
ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                  struct sockaddr* src, socklen_t* srclen);
int setsockopt(int fd, int level, int optname,
                const void* optval, socklen_t optlen);
int shutdown(int fd, int how);
int sendfd(int sock_fd, int target_fd, unsigned int rights);
int recvfd(int sock_fd);

// inet_pton / inet_ntop — IPv4 only (AF_INET).
// inet_pton: parse "a.b.c.d" → *out (network byte order). Returns 1 on
//   success, 0 if `src` is malformed, -1 on unsupported family.
// inet_ntop: format 4-byte network-order address into `dst[dst_len]`.
//   Returns dst on success, NULL on buffer-too-small.
int         inet_pton(int family, const char* src, void* out);
const char* inet_ntop(int family, const void* src, char* dst, socklen_t dst_len);

// ── Network interface configuration (root-only, called by dhcpcd) ──────
#define IFCFG_MAX_DNS 3
typedef struct {
    uint32_t ip_be;
    uint32_t gateway_be;
    uint32_t netmask_be;
    uint32_t dns_be[IFCFG_MAX_DNS];
    uint32_t lease_seconds;
} ifcfg_t;

int net_ifconfig(const ifcfg_t* cfg);
int net_mac(uint8_t out[6]);

// ── DNS resolver (RFC 1035) ────────────────────────────────────────────────
// POSIX-style name resolution. Reads /etc/resolv.conf on first use, falls back
// to the gateway if no nameservers are configured.  IPv4 only.
//
// gethostbyname_ipv4("example.com", out_ip_be) -> 0 on success, -1 on failure
// (errno = EINVAL / ENOENT / EAGAIN / ETIMEDOUT).
// Numeric "a.b.c.d" is resolved via inet_pton without a network round-trip.
int gethostbyname_ipv4(const char* name, uint32_t* out_ip_be);

// Simple blocking getaddrinfo for IPv4. Caller passes a statically-sized
// sockaddr_in_t and the port in host byte order. Returns 0 on success.
int getaddrinfo_ipv4(const char* host, uint16_t port,
                     sockaddr_in_t* out_addr);

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
#define SYS_REBOOT      75
#define SYS_NET_IFCONFIG 95
#define SYS_NET_MAC      96

// ── Extra errno values ────────────────────────────────────────────────────
#define EILSEQ      84
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

static inline int fcntl(int fd, int cmd, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, cmd);
    long arg = __builtin_va_arg(ap, long);
    __builtin_va_end(ap);
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

// ── fstat — defined earlier as direct syscall inline ──────────────────────

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
typedef struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
} termios_t;

// c_iflag bits
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

// c_oflag bits
#define OPOST   0000001
#define OLCUC   0000002
#define ONLCR   0000004
#define OCRNL   0000010
#define ONOCR   0000020
#define ONLRET  0000040
#define OFILL   0000100
#define OFDEL   0000200

// c_cflag bits
#define CBAUD   0010017
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

// c_lflag bits
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

// termios baud rates
#define B0      0000000
#define B50     0000001
#define B75     0000002
#define B110    0000003
#define B134    0000004
#define B150    0000005
#define B200    0000006
#define B300    0000007
#define B600    0000010
#define B1200   0000011
#define B1800   0000012
#define B2400   0000013
#define B4800   0000014
#define B9600   0000015
#define B19200  0000016
#define B38400  0000017

// termios special character indices
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
#define NCCS    19

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

typedef struct winsize {
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

// tcflow / tcdrain action codes
#define TCOOFF  0
#define TCOON   1
#define TCIOFF  2
#define TCION   3

static inline int tcdrain(int fd) {
    return ioctl(fd, TCSBRK, (void*)(intptr_t)1);
}
static inline int tcflow(int fd, int action) {
    return ioctl(fd, TCXONC, (void*)(intptr_t)action);
}
static inline int tcflush(int fd, int queue) {
    return ioctl(fd, TCFLSH, (void*)(intptr_t)queue);
}

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

static inline int select(int nfds, fd_set* rfds, fd_set* wfds,
                          fd_set* efds, struct timeval* tv) {
    // Syscall ABI: rdi=nfds, rsi=rfds, rdx=wfds, r10=efds, r8=tv
    uint64_t ret;
    __asm__ volatile(
        "mov %5, %%r10\n\t"
        "mov %6, %%r8\n\t"
        "syscall"
        : "=a"(ret)
        : "a"((uint64_t)SYS_SELECT),
          "D"((uint64_t)nfds), "S"((uint64_t)rfds),
          "d"((uint64_t)wfds), "r"((uint64_t)efds),
          "r"((uint64_t)tv)
        : "rcx", "r11", "r10", "r8", "memory"
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

typedef struct rusage {
    timeval_t ru_utime;
    timeval_t ru_stime;
    long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss;
    long ru_minflt, ru_majflt, ru_nswap;
    long ru_inblock, ru_oublock;
    long ru_msgsnd, ru_msgrcv;
    long ru_nsignals, ru_nvcsw, ru_nivcsw;
} rusage_t;
#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

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

// ── POSIX struct stat ─────────────────────────────────────────────────────
// Layout must match struct stat in kernel/syscall/syscall.h exactly.
// The kernel fills this struct directly via SYS_STAT / SYS_FSTAT.
// st_atim/st_mtim/st_ctim are struct timespec so bash's stat-time.h works.
typedef struct stat {
    uint64_t      st_ino;
    uint64_t      st_nlink;
    uint32_t      st_mode;
    uint32_t      st_uid;
    uint32_t      st_gid;
    uint32_t      _pad0;
    uint64_t      st_size;
    timespec_t    st_atim;   // st_atim.tv_sec = access time
    timespec_t    st_mtim;   // st_mtim.tv_sec = mod time
    timespec_t    st_ctim;   // st_ctim.tv_sec = change time
    uint64_t      st_blksize;
    int64_t       st_blocks;
    int32_t       st_dev;
    int32_t       st_rdev;
} stat_t;
// POSIX scalar aliases
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
// S_IF* mode bits
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFSOCK 0140000
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
// Mode permission bits
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRWXU 0700
#define S_IRWXG 0070
#define S_IRWXO 0007
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

// ── getenv / environ ──────────────────────────────────────────────────────
extern char** environ;
static inline char* getenv(const char* name) {
    if (!environ || !name) return NULL;
    size_t nlen = 0; while (name[nlen]) nlen++;
    for (char** e = environ; *e; e++) {
        char* eq = strchr(*e, '=');
        if (!eq) continue;
        size_t klen = (size_t)(eq - *e);
        if (klen == nlen && strncmp(*e, name, nlen) == 0) return eq + 1;
    }
    return NULL;
}

int    setenv(const char* name, const char* value, int overwrite);
int    unsetenv(const char* name);
int    putenv(char* string);

// ── POSIX stat / lstat / fstat ────────────────────────────────────────────
// The kernel fills struct stat directly — no bridge needed.
static inline int stat(const char* path, struct stat* st) {
    size_t n = 0; while (path[n]) n++;
    return (int)__syscall_ret(syscall3(SYS_STAT, (uint64_t)path, n, (uint64_t)st));
}
static inline int lstat(const char* path, struct stat* st) {
    // No symlink support yet — lstat behaves like stat.
    return stat(path, st);
}
static inline int fstat(int fd, struct stat* st) {
    return (int)__syscall_ret(syscall2(SYS_FSTAT, (uint64_t)fd, (uint64_t)st));
}

// ── EOF sentinel (defined early so inline stubs can use it) ──────────────
#ifndef EOF
#define EOF (-1)
#endif

// ── POSIX DIR* API ────────────────────────────────────────────────────────
// The kernel SYS_READDIR syscall fills k_dirent_t entries (internal ABI).
// opendir/readdir translate them to POSIX struct dirent for callers.

// Internal kernel readdir entry — matches ext2_entry_t written by sys_readdir.
typedef struct {
    char     name[256];
    uint32_t inode_num;
    uint32_t size;
    uint8_t  is_dir;
} k_dirent_t;

static inline int _sys_readdir(const char* path, size_t pathlen,
                                k_dirent_t* buf, int max) {
    return (int)__syscall_ret(syscall4(SYS_READDIR, (uint64_t)path, pathlen,
                                       (uint64_t)buf, (uint64_t)max));
}

// POSIX dirent — struct dirent for compat, dirent_t for our style
typedef struct dirent {
    unsigned long  d_ino;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
} dirent_t;

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4
#define DT_LNK     10

typedef struct {
    char        path[512];      // directory path this DIR was opened on
    k_dirent_t* entries;        // heap buffer of kernel entries
    int         count;          // total entries returned by readdir syscall
    int         pos;            // current read position
    struct dirent cur;          // storage for current POSIX dirent
} DIR;

DIR*           opendir(const char* path);
struct dirent* readdir(DIR* dirp);
int            closedir(DIR* dirp);
void           rewinddir(DIR* dirp);

// ── Additional string functions ───────────────────────────────────────────
char*  strpbrk(const char* s, const char* accept);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char*  strtok(char* s, const char* delim);
char*  strtok_r(char* s, const char* delim, char** saveptr);
char*  stpcpy(char* dst, const char* src);
char*  strchrnul(const char* s, int c);
char*  strcasestr(const char* haystack, const char* needle);
char*  strnlen_s(const char* s, size_t maxlen);  // internal helper
size_t strnlen(const char* s, size_t maxlen);
char*  strcoll_helper(const char* a, const char* b); // internal
int    strcoll(const char* a, const char* b);
double strtod(const char* s, char** endptr);
long long strtoll(const char* s, char** endptr, int base);
unsigned long long strtoull(const char* s, char** endptr, int base);
int    asprintf(char** strp, const char* fmt, ...);
int    vasprintf(char** strp, const char* fmt, __builtin_va_list ap);

// ── POSIX ctype ───────────────────────────────────────────────────────────
static inline int isdigit(int c)  { return c >= '0' && c <= '9'; }
static inline int isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int isalnum(int c)  { return isdigit(c) || isalpha(c); }
static inline int isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static inline int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c)  { return c >= 'a' && c <= 'z'; }
static inline int isprint(int c)  { return c >= 0x20 && c < 0x7F; }
static inline int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7F; }
static inline int ispunct(int c)  { return isprint(c) && !isalnum(c) && c != ' '; }
static inline int isxdigit(int c) { return isdigit(c) || (c>='a'&&c<='f') || (c>='A'&&c<='F'); }
static inline int isblank(int c)  { return c == ' ' || c == '\t'; }
static inline int isascii(int c)  { return (unsigned)c < 128; }
static inline int isgraph(int c)  { return c > 0x20 && c < 0x7F; }
static inline int toupper(int c)  { return islower(c) ? c - 32 : c; }
static inline int tolower(int c)  { return isupper(c) ? c + 32 : c; }
static inline void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    while (n--) { if (*p == (unsigned char)c) return (void*)p; p++; }
    return NULL;
}

// ── POSIX signal sets ─────────────────────────────────────────────────────
// Our sigset is a uint32_t bitmask. These are the standard POSIX operations.
typedef uint32_t sigset_t;
static inline int sigemptyset(sigset_t* s) { *s = 0; return 0; }
static inline int sigfillset(sigset_t* s)  { *s = ~0u; return 0; }
static inline int sigaddset(sigset_t* s, int sig) {
    if (sig < 1 || sig > 31) { errno = EINVAL; return -1; }
    *s |= (1u << (sig-1)); return 0;
}
static inline int sigdelset(sigset_t* s, int sig) {
    if (sig < 1 || sig > 31) { errno = EINVAL; return -1; }
    *s &= ~(1u << (sig-1)); return 0;
}
static inline int sigismember(const sigset_t* s, int sig) {
    if (sig < 1 || sig > 31) { errno = EINVAL; return -1; }
    return (*s >> (sig-1)) & 1;
}

// Job-control signals (not yet in our signal.h but bash expects the defines)
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGCONT   18
#define SIGWINCH  28
#define SIGURG    23

// killpg: send signal to a process group
static inline int killpg(pid_t pgrp, int sig) {
    return kill(-(int)pgrp, sig);
}

// alarm: schedule SIGALRM after `seconds` seconds.
// Implemented via setitimer syscall (SYS_SETITIMER = kernel number).
unsigned int alarm(unsigned int seconds);

// sleep: sleep for seconds (implemented via nanosleep)
static inline unsigned int sleep(unsigned int seconds) {
    struct timespec req = { .tv_sec = seconds, .tv_nsec = 0 };
    nanosleep(&req, NULL);
    return 0;
}

// ── passwd database ───────────────────────────────────────────────────────
struct passwd {
    char*  pw_name;
    char*  pw_passwd;
    uid_t  pw_uid;
    gid_t  pw_gid;
    char*  pw_gecos;
    char*  pw_dir;
    char*  pw_shell;
};

struct passwd* getpwuid(uid_t uid);
struct passwd* getpwnam(const char* name);
void           setpwent(void);
void           endpwent(void);
struct passwd* getpwent(void);

// ── Process / resource limits ─────────────────────────────────────────────
typedef unsigned long rlim_t;
struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; };
#define RLIMIT_CPU    0
#define RLIMIT_FSIZE  1
#define RLIMIT_DATA   2
#define RLIMIT_STACK  3
#define RLIMIT_CORE   4
#define RLIMIT_NOFILE 7
#define RLIMIT_AS     9
#define RLIM_INFINITY (~0UL)

static inline int getrlimit(int resource, struct rlimit* rlim) {
    (void)resource;
    if (!rlim) { errno = EINVAL; return -1; }
    rlim->rlim_cur = rlim->rlim_max = RLIM_INFINITY;
    return 0;
}
static inline int setrlimit(int resource, const struct rlimit* rlim) {
    (void)resource; (void)rlim; return 0;   // accept silently
}

// ── sysconf / pathconf / confstr / getdtablesize ─────────────────────────
#define _SC_CLK_TCK         2
#define _SC_OPEN_MAX        4
#define _SC_PAGESIZE        30
#define _SC_NGROUPS_MAX     3
#define _SC_ARG_MAX         0
long sysconf(int name);
long pathconf(const char* path, int name);
int  confstr(int name, char* buf, size_t len);
static inline int getdtablesize(void) { return 1024; }

// ── hostname ──────────────────────────────────────────────────────────────
static inline int gethostname(char* name, size_t len) {
    const char* h = "makaos";
    size_t i = 0;
    while (i + 1 < len && h[i]) { name[i] = h[i]; i++; }
    name[i] = '\0';
    return 0;
}

// ── ttyname ───────────────────────────────────────────────────────────────
static inline char* ttyname(int fd) {
    (void)fd;
    return "/dev/tty0";
}
static inline int ttyname_r(int fd, char* buf, size_t buflen) {
    (void)fd;
    const char* t = "/dev/tty0";
    size_t i = 0;
    while (i + 1 < buflen && t[i]) { buf[i] = t[i]; i++; }
    buf[i] = '\0';
    return 0;
}

// ── misc stubs bash needs ─────────────────────────────────────────────────
static inline int setresuid(uid_t r, uid_t e, uid_t s) {
    (void)s; return setreuid(r, e);
}
static inline int setresgid(gid_t r, gid_t e, gid_t s) {
    (void)s; return setregid(r, e);
}

// getrandom: fill buf with random bytes
static inline ssize_t getrandom(void* buf, size_t buflen, unsigned int flags) {
    (void)flags;
    unsigned char* p = (unsigned char*)buf;
    uint64_t t = clock_ns();
    for (size_t i = 0; i < buflen; i++) {
        t = t * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = (unsigned char)(t >> 33);
    }
    return (ssize_t)buflen;
}
static inline long arc4random(void) {
    uint64_t t = clock_ns();
    t = t * 6364136223846793005ULL + 1442695040888963407ULL;
    return (long)((t >> 1) & 0x7FFFFFFF);
}

// mkstemp / mktemp / mkdtemp
int mkstemp(char* tmpl);
char* mktemp(char* tmpl);
char* mkdtemp(char* tmpl);
static inline int mkdtemp_r(char* tmpl) {
    // Replace trailing XXXXXX with random chars
    size_t len = 0; while (tmpl[len]) len++;
    if (len < 6) { errno = EINVAL; return -1; }
    uint64_t r = (uint64_t)clock_ns();
    for (int i = 0; i < 6; i++) {
        r = r * 6364136223846793005ULL + 1;
        tmpl[len - 6 + i] = "abcdefghijklmnopqrstuvwxyz012345"[r >> 59];
    }
    return 0;
}

// abort
__attribute__((noreturn)) void abort(void);

// setitimer (used by alarm())
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};
static inline int setitimer(int which, const struct itimerval* nv,
                             struct itimerval* ov) {
    (void)which; (void)nv; (void)ov; return 0;   // stub: no preemptive timer yet
}

// locale — stubs (single-locale OS)
#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
#define LC_MESSAGES 6
static inline char* setlocale(int cat, const char* loc) {
    (void)cat; (void)loc; return "C";
}
struct lconv {
    char* decimal_point;
    char* thousands_sep;
    char* grouping;
    char* int_curr_symbol;
    char* currency_symbol;
    char* mon_decimal_point;
    char* mon_thousands_sep;
    char* mon_grouping;
    char* positive_sign;
    char* negative_sign;
    char  int_frac_digits;
    char  frac_digits;
    char  p_cs_precedes;
    char  p_sep_by_space;
    char  n_cs_precedes;
    char  n_sep_by_space;
    char  p_sign_posn;
    char  n_sign_posn;
};
static inline struct lconv* localeconv(void) {
    static struct lconv lc = { ".", "", "", "", "", ".", "", "", "+", "-",
                                2, 2, 1, 1, 1, 1, 1, 1 };
    return &lc;
}

// strftime / localtime / gmtime stubs
struct tm {
    int   tm_sec, tm_min, tm_hour;
    int   tm_mday, tm_mon, tm_year;
    int   tm_wday, tm_yday, tm_isdst;
    long  tm_gmtoff;   // seconds east of UTC (GNU extension)
    const char* tm_zone; // timezone abbreviation (GNU extension)
};
struct tm* localtime(const time_t* t);
struct tm* gmtime(const time_t* t);
size_t     strftime(char* s, size_t max, const char* fmt, const struct tm* tm);
static inline void tzset(void) {}

// nl_langinfo stub
#define CODESET       14
#define RADIXCHAR     'X'
static inline char* nl_langinfo(int item) { (void)item; return "UTF-8"; }

// iconv stubs (no multibyte conversion, return identity)
typedef void* iconv_t;
static inline iconv_t iconv_open(const char* to, const char* from) {
    (void)to; (void)from; errno = EINVAL; return (iconv_t)-1;
}
static inline size_t iconv(iconv_t cd, char** in, size_t* inleft,
                            char** out, size_t* outleft) {
    (void)cd; (void)in; (void)inleft; (void)out; (void)outleft;
    errno = EILSEQ; return (size_t)-1;
}
static inline int iconv_close(iconv_t cd) { (void)cd; return 0; }

// dlopen stubs (no dynamic linking)
#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_GLOBAL 0x100
static inline void* dlopen(const char* f, int flags) {
    (void)f; (void)flags; errno = ENOSYS; return NULL;
}
static inline void* dlsym(void* h, const char* sym) {
    (void)h; (void)sym; errno = ENOSYS; return NULL;
}
static inline char* dlerror(void) { return "dlopen not supported"; }
static inline int   dlclose(void* h) { (void)h; return 0; }

// Wide-char stubs — bash only uses these for locale-aware display;
// on a C-locale OS they can be identity/stub.
typedef unsigned int wchar_t;
typedef unsigned int wint_t;
typedef unsigned int wctype_t;
#define WEOF ((wint_t)-1)

static inline size_t wcslen(const wchar_t* s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static inline wchar_t* wcschr(const wchar_t* s, wchar_t c) {
    while (*s && *s != c) s++;
    return (*s == c) ? (wchar_t*)s : NULL;
}
static inline int wcscmp(const wchar_t* a, const wchar_t* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}
static inline int wcsncmp(const wchar_t* a, const wchar_t* b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)*a - (int)*b;
}
static inline wchar_t* wcsdup(const wchar_t* s) {
    size_t n = wcslen(s) + 1;
    wchar_t* p = malloc(n * sizeof(wchar_t));
    if (p) { for (size_t i = 0; i < n; i++) p[i] = s[i]; }
    return p;
}
static inline int wcscoll(const wchar_t* a, const wchar_t* b) { return wcscmp(a, b); }
static inline wchar_t* wmemchr(const wchar_t* s, wchar_t c, size_t n) {
    while (n--) { if (*s == c) return (wchar_t*)s; s++; } return NULL;
}
static inline wint_t towlower(wint_t c) { return (c >= L'A' && c <= L'Z') ? c + 32 : c; }
static inline wint_t towupper(wint_t c) { return (c >= L'a' && c <= L'z') ? c - 32 : c; }
static inline int    iswlower(wint_t c) { return c >= L'a' && c <= L'z'; }
static inline int    iswupper(wint_t c) { return c >= L'A' && c <= L'Z'; }
static inline int    iswalnum(wint_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
}
static inline int    iswprint(wint_t c) { return c >= 0x20 && c < 0x7F; }
static inline wctype_t wctype(const char* s) { (void)s; return 0; }
static inline int    iswctype(wint_t c, wctype_t t) { (void)c; (void)t; return 0; }

// mbstowcs / wcstombs — identity for ASCII
static inline size_t mbstowcs(wchar_t* dst, const char* src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) { dst[i] = (wchar_t)(unsigned char)src[i]; i++; }
    if (i < n) dst[i] = 0;
    return i;
}
static inline size_t wcstombs(char* dst, const wchar_t* src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) { dst[i] = (char)(src[i] & 0x7F); i++; }
    if (i < n) dst[i] = '\0';
    return i;
}
static inline int mbtowc(wchar_t* pwc, const char* s, size_t n) {
    if (!s || !n) return 0;
    if (pwc) *pwc = (wchar_t)(unsigned char)*s;
    return (*s == '\0') ? 0 : 1;
}
static inline int wctomb(char* s, wchar_t wc) {
    if (s) { *s = (char)(wc & 0x7F); return 1; } return 0;
}
static inline int mblen(const char* s, size_t n) {
    if (!s) return 0;
    if (!n) return -1;
    return (*s == '\0') ? 0 : 1;
}

// mbstate stubs
typedef int mbstate_t;
static inline size_t mbrtowc(wchar_t* pwc, const char* s, size_t n, mbstate_t* ps) {
    (void)ps; return (size_t)mbtowc(pwc, s, n);
}
static inline int mbsinit(const mbstate_t* ps) { (void)ps; return 1; }
static inline size_t __mbrlen(const char* s, size_t n, mbstate_t* ps) {
    return mbrtowc(NULL, s, n, ps);
}
static inline size_t mbrlen(const char* s, size_t n, mbstate_t* ps) {
    return mbrtowc(NULL, s, n, ps);
}
static inline size_t wcrtomb(char* s, wchar_t wc, mbstate_t* ps) {
    (void)ps; return (size_t)wctomb(s, wc);
}
static inline size_t mbsrtowcs(wchar_t* dst, const char** src, size_t n, mbstate_t* ps) {
    (void)ps; size_t r = mbstowcs(dst, *src, n); if (r != (size_t)-1) *src = NULL; return r;
}
static inline size_t wcsrtombs(char* dst, const wchar_t** src, size_t n, mbstate_t* ps) {
    (void)ps; size_t r = wcstombs(dst, *src, n); if (r != (size_t)-1) *src = NULL; return r;
}
static inline size_t mbsnrtowcs(wchar_t* dst, const char** src, size_t nms,
                                 size_t n, mbstate_t* ps) {
    (void)ps; (void)nms; return mbsrtowcs(dst, src, n, ps);
}
static inline int wcwidth(wchar_t c) { return (c >= 0x20 && c < 0x7F) ? 1 : 0; }
static inline int wcswidth(const wchar_t* s, size_t n) {
    int w = 0; while (n-- && *s) { w += wcwidth(*s); s++; } return w;
}
static inline int wctob(wint_t c) { return (c < 0x80) ? (int)c : EOF; }
static inline wint_t btowc(int c) { return (c == EOF) ? WEOF : (wint_t)c; }

// __ctype_b_loc / __ctype_tolower_loc / __ctype_toupper_loc
// glibc uses these internally for ctype. We expose them as stubs.
// Returning NULL causes ctype_b() to fault; return real tables instead.
static const unsigned short __ctype_b_table[128] = {
    // Only ASCII range; everything else is 0.
    // bit layout matches glibc: _ISupper=0x100, _ISlower=0x200, _ISalpha=0x400,
    // _ISdigit=0x800, _ISxdigit=0x1000, _ISspace=0x2000, _ISblank=0x4000
    0,0,0,0,0,0,0,0,0,0x2000|0x4000,0x2000,0x2000,0x2000,0x2000,0,0, // 0-15
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 16-31
    0x2000|0x4000, // 32 space+blank
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 33-47
    0x800|0x1000,0x800|0x1000,0x800|0x1000,0x800|0x1000,0x800|0x1000, // 48-52 0-4
    0x800|0x1000,0x800|0x1000,0x800|0x1000,0x800|0x1000,0x800|0x1000, // 53-57 5-9
    0,0,0,0,0,0,0, // 58-64
    0x100|0x400|0x1000,0x100|0x400|0x1000,0x100|0x400|0x1000, // 65-67 A-C
    0x100|0x400|0x1000,0x100|0x400|0x1000,0x100|0x400|0x1000, // 68-70 D-F
    0x100|0x400,0x100|0x400,0x100|0x400,0x100|0x400,0x100|0x400,0x100|0x400, // 71-76 G-L
    0x100|0x400,0x100|0x400,0x100|0x400,0x100|0x400,0x100|0x400,0x100|0x400, // 77-82 M-R
    0x100|0x400,0x100|0x400,0x100|0x400,0x100|0x400,0x100|0x400,0x100|0x400, // 83-88 S-X
    0x100|0x400,0x100|0x400, // 89-90 Y-Z
    0,0,0,0,0,0, // 91-96
    0x200|0x400|0x1000,0x200|0x400|0x1000,0x200|0x400|0x1000, // 97-99 a-c
    0x200|0x400|0x1000,0x200|0x400|0x1000,0x200|0x400|0x1000, // 100-102 d-f
    0x200|0x400,0x200|0x400,0x200|0x400,0x200|0x400,0x200|0x400,0x200|0x400, // g-l
    0x200|0x400,0x200|0x400,0x200|0x400,0x200|0x400,0x200|0x400,0x200|0x400, // m-r
    0x200|0x400,0x200|0x400,0x200|0x400,0x200|0x400,0x200|0x400,0x200|0x400, // s-x
    0x200|0x400,0x200|0x400, // y-z
    0,0,0,0,0 // 123-127
};
static const int __ctype_tolower_table[128] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,
    58,59,60,61,62,63,64,
    97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,
    91,92,93,94,95,96,
    97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,
    123,124,125,126,127
};
static const int __ctype_toupper_table[128] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,
    58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,
    91,92,93,94,95,96,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,
    123,124,125,126,127
};
static inline const unsigned short** __ctype_b_loc(void) {
    static const unsigned short* p = __ctype_b_table;
    return (const unsigned short**)&p;
}
static inline const int** __ctype_tolower_loc(void) {
    static const int* p = __ctype_tolower_table;
    return (const int**)&p;
}
static inline const int** __ctype_toupper_loc(void) {
    static const int* p = __ctype_toupper_table;
    return (const int**)&p;
}

// __ctype_get_mb_cur_max — always 1 (single-byte locale)
static inline size_t __ctype_get_mb_cur_max(void) { return 1; }
#define MB_CUR_MAX (__ctype_get_mb_cur_max())

// ── POSIX regex ──────────────────────────────────────────────────────────
// Minimal ERE engine sufficient for bash's [[ =~ ]] and glob.
#define REG_EXTENDED  1
#define REG_ICASE     2
#define REG_NOSUB     4
#define REG_NEWLINE   8
#define REG_NOTBOL   16
#define REG_NOTEOL   32

#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13

#define REG_MAX_SUB 32

typedef struct {
    size_t rm_so;
    size_t rm_eo;
} regmatch_t;

typedef struct {
    int    re_nsub;
    void*  _internal;   // compiled NFA/DFA — opaque
} regex_t;

int regcomp(regex_t* preg, const char* pattern, int cflags);
int regexec(const regex_t* preg, const char* string, size_t nmatch,
            regmatch_t pmatch[], int eflags);
void regfree(regex_t* preg);
size_t regerror(int errcode, const regex_t* preg, char* errbuf, size_t errbuf_size);

// ── fnmatch ───────────────────────────────────────────────────────────────
#define FNM_NOMATCH   1
#define FNM_PATHNAME  1
#define FNM_NOESCAPE  2
#define FNM_PERIOD    4
#define FNM_CASEFOLD  8
int fnmatch(const char* pattern, const char* string, int flags);

// ── __libc_start_main — bash entry point bridge ───────────────────────────
// glibc's __libc_start_main is what the CRT calls before main().
// We provide our own so bash links without glibc.
int __libc_start_main(int (*main)(int, char**, char**),
                      int argc, char** argv,
                      void (*init)(void), void (*fini)(void),
                      void (*rtld_fini)(void), void* stack_end);

// __errno_location — glibc's way of getting &errno (used by -D_REENTRANT code)
static inline int* __errno_location(void) {
    extern int errno;
    return &errno;
}

// pselect — like select but with sigset mask and timespec
static inline int pselect(int nfds, fd_set* r, fd_set* w, fd_set* e,
                           const struct timespec* ts, const sigset_t* mask) {
    (void)mask;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    if (ts) { tv.tv_sec = ts->tv_sec; tv.tv_usec = (int64_t)(ts->tv_nsec / 1000); }
    return select(nfds, r, w, e, ts ? &tv : NULL);
}

// imaxdiv
typedef struct { long long quot; long long rem; } imaxdiv_t;
static inline imaxdiv_t imaxdiv(long long n, long long d) {
    imaxdiv_t r; r.quot = n / d; r.rem = n % d; return r;
}

// __isoc23_strtol / __isoc23_strtoumax — C23 aliases, same as strtol/strtoull
static inline long __isoc23_strtol(const char* s, char** e, int b) { return strtol(s, e, b); }
static inline unsigned long long __isoc23_strtoumax(const char* s, char** e, int b) {
    return strtoull(s, e, b);
}

// __libc_current_sigrtmin/max — no real-time signals on MakaOS
static inline int __libc_current_sigrtmin(void) { return 32; }
static inline int __libc_current_sigrtmax(void) { return 32; }

// __sigsetjmp / siglongjmp — extend setjmp with sigmask save/restore
// Our setjmp.h defines jmp_buf; we extend it to save the signal mask too.
#ifndef _SIGJMP_BUF_DEFINED
#define _SIGJMP_BUF_DEFINED
typedef struct {
    long long _regs[8];   // rsp, rbp, rbx, r12, r13, r14, r15, rip
    uint32_t  _mask;      // saved signal mask (if savesigs != 0)
    int       _savesigs;
} sigjmp_buf[1];
#endif

int  __sigsetjmp(sigjmp_buf env, int savesigs);
__attribute__((noreturn)) void siglongjmp(sigjmp_buf env, int val);
#define sigsetjmp(env, save) __sigsetjmp(env, save)

// lstat — defined earlier (no symlinks yet, same as stat)

// eaccess / faccessat — effective-uid access check
static inline int eaccess(const char* path, int mode) {
    return access(path, mode);
}
static inline int faccessat(int dirfd, const char* path, int mode, int flags) {
    (void)dirfd; (void)flags;
    return access(path, mode);
}

// getpeername stub
static inline int getpeername(int fd, void* addr, size_t* len) {
    (void)fd; (void)addr; (void)len; errno = ENOTCONN; return -1;
}

// setvbuf / __fpurge / fileno — defined in stdio.c; declared after stdio.h is included.
// Forward-declared here using incomplete type:
#ifndef _FILE_TYPE_FORWARD
#define _FILE_TYPE_FORWARD
typedef struct FILE FILE;
#endif
int setvbuf(FILE* f, char* buf, int mode, size_t size);
static inline int __fpurge(FILE* f) { (void)f; return 0; }

// gettimeofday — POSIX
static inline int gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    return (int)__syscall_ret(syscall2(SYS_GETTOD, (uint64_t)tv, 0));
}

// getaddrinfo / freeaddrinfo stubs (bash uses for network builtins, optional)
struct addrinfo { int ai_flags; int ai_family; int ai_socktype; int ai_protocol;
                  size_t ai_addrlen; struct sockaddr* ai_addr; char* ai_canonname;
                  struct addrinfo* ai_next; };
static inline int getaddrinfo(const char* h, const char* s,
                               const struct addrinfo* hints, struct addrinfo** res) {
    (void)h; (void)s; (void)hints; (void)res; errno = ENOSYS; return -1;
}
static inline void freeaddrinfo(struct addrinfo* ai) { (void)ai; }
static inline const char* gai_strerror(int e) { (void)e; return "name resolution not supported"; }

// ── termcap ──────────────────────────────────────────────────────────────
// Minimal but real termcap implementation for VT100/linux terminals.
// Returns actual ANSI escape sequences rather than stubs.
extern char PC;        // pad character (NUL)
extern char* BC;       // backspace-if-not-^H
extern char* UP;       // move cursor up one line

int   tgetent(char* bp, const char* name);
int   tgetflag(const char* id);
int   tgetnum(const char* id);
char* tgetstr(const char* id, char** area);
char* tgoto(const char* cap, int col, int row);
int   tputs(const char* str, int affcnt, int (*putc_fn)(int));

// (rename / unlink / chdir / mkdir / stat / lstat / fstat defined earlier)
