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
