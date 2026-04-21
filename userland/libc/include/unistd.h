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
int     isatty(int fd);
int     ftruncate(int fd, off_t len);
int     truncate(const char* path, off_t len);
int     fsync(int fd);
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
pid_t  getpid(void);
pid_t  getppid(void);
int    execve(const char* path, char* const argv[], char* const envp[]);
int    execv(const char* path, char* const argv[]);
int    execvp(const char* file, char* const argv[]);
int    execl(const char* path, const char* arg, ...);
int    execlp(const char* file, const char* arg, ...);
int    execle(const char* path, const char* arg, ...);
__attribute__((noreturn)) void _exit(int status);

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

#endif
