#ifndef _MAKAOS_UNISTD_H
#define _MAKAOS_UNISTD_H 1

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <stddef.h>

// POSIX feature marker — we provide the .2001 subset that matters to
// portable code (clock_gettime, nanosleep, pthread types not included).
#define _POSIX_VERSION      200112L
#define _POSIX2_VERSION     200112L
#define _POSIX_TIMERS       1
#define _POSIX_MONOTONIC_CLOCK 1

// whence
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// std fds
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// access() mode
#define F_OK  0
#define X_OK  1
#define W_OK  2
#define R_OK  4

// I/O
ssize_t read(int fd, void* buf, size_t n);
ssize_t write(int fd, const void* buf, size_t n);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     dup(int fd);
int     dup2(int old_fd, int new_fd);
int     pipe(int fds[2]);
int     pipe2(int fds[2], int flags);   // Linux ext — O_CLOEXEC | O_NONBLOCK
int     isatty(int fd);

// sysconf — runtime queries.  MakaOS returns fixed values for the
// handful of names ports actually look up; anything else returns -1.
long   sysconf(int name);
// confstr — string-valued runtime queries (just _CS_PATH for foot).
size_t confstr(int name, char* buf, size_t len);
// pathconf/fpathconf — per-path configurable limits.
long pathconf(const char* path, int name);
long fpathconf(int fd, int name);
#define _PC_LINK_MAX          0
#define _PC_MAX_CANON         1
#define _PC_MAX_INPUT         2
#define _PC_NAME_MAX          3
#define _PC_PATH_MAX          4
#define _PC_PIPE_BUF          5
#define _PC_CHOWN_RESTRICTED  6
#define _PC_NO_TRUNC          7
#define _PC_VDISABLE          8
#define _CS_PATH 0
#define _CS_GNU_LIBC_VERSION       2
#define _CS_GNU_LIBPTHREAD_VERSION 3
// _SC_* names — only the subset foot / harfbuzz probe today.
#define _SC_NPROCESSORS_CONF  10
#define _SC_NPROCESSORS_ONLN  11
#define _SC_PAGESIZE          30
#define _SC_PAGE_SIZE         _SC_PAGESIZE
#define _SC_PHYS_PAGES        85
#define _SC_AVPHYS_PAGES      86
#define _SC_CLK_TCK           2
#define _SC_GETPW_R_SIZE_MAX  70
#define _SC_GETGR_R_SIZE_MAX  71
#define _SC_OPEN_MAX          4
#define _SC_IOV_MAX           60
int     ftruncate(int fd, off_t len);
int     truncate(const char* path, off_t len);
int     fsync(int fd);
void    sync(void);
int     fdatasync(int fd);

// Path ops
int     access(const char* path, int mode);
int     chdir(const char* path);
int     fchdir(int fd);
char*   getcwd(char* buf, size_t n);
int     chown(const char* path, uid_t uid, gid_t gid);
int     fchown(int fd, uid_t uid, gid_t gid);
int     lchown(const char* path, uid_t uid, gid_t gid);
int     link(const char* existing, const char* newpath);
int     unlink(const char* path);
int     rmdir(const char* path);
int     symlink(const char* target, const char* linkpath);
ssize_t readlink(const char* path, char* buf, size_t n);
ssize_t pread(int fd, void* buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void* buf, size_t n, off_t off);

// Process
pid_t  fork(void);
pid_t  vfork(void);
pid_t  getpid(void);
pid_t  getppid(void);
int    gettid(void);               // calling thread's kernel task id
long   syscall(long number, ...);  // Linux-number compat shim (see sys/syscall.h)
int    execve(const char* path, char* const argv[], char* const envp[]);
int    execv(const char* path, char* const argv[]);
int    execvp(const char* file, char* const argv[]);
int    execl(const char* path, const char* arg, ...);
int    execlp(const char* file, const char* arg, ...);
int    execle(const char* path, const char* arg, ...);
__attribute__((__noreturn__)) void _exit(int status);

// Identity
uid_t  getuid(void);
uid_t  geteuid(void);
gid_t  getgid(void);
gid_t  getegid(void);
int    setuid(uid_t uid);
int    setgid(gid_t gid);
int    seteuid(uid_t uid);
int    setegid(gid_t gid);
int    setreuid(uid_t r, uid_t e);
int    setregid(gid_t r, gid_t e);

// Sessions / process groups
pid_t  setsid(void);
pid_t  getsid(pid_t pid);
int    setpgid(pid_t pid, pid_t pgid);
pid_t  getpgid(pid_t pid);
pid_t  getpgrp(void);
pid_t  tcgetpgrp(int fd);
int    tcsetpgrp(int fd, pid_t pgid);

// Host
int    gethostname(char* name, size_t n);
char*  ttyname(int fd);
int    ttyname_r(int fd, char* buf, size_t n);

// Memory
void*  sbrk(intptr_t incr);
void*  brk(void* addr);

// Timing
unsigned sleep(unsigned seconds);
int      usleep(unsigned us);

// fork-safe entropy snapshot
void alarm(unsigned seconds);

// Page size (fixed 4 KiB on x86_64).
int getpagesize(void);

// Device-node creation — stub, returns -ENOSYS.  libdrm calls it for
// /dev/dri node setup but we pre-populate in virtfs.
int mknod(const char* path, unsigned mode, unsigned dev);

#ifdef __cplusplus
}
#endif


// LFS (*64) aliases -- MakaOS is LP64 with 64-bit off_t/ino_t,
// so the large-file variants are the same as the plain functions.
#if defined(_LARGEFILE64_SOURCE) || defined(_GNU_SOURCE)
#define lseek64     lseek
#define pread64     pread
#define pwrite64    pwrite
#define ftruncate64 ftruncate
#endif

#endif
