#ifndef _MAKAOS_SYS_STAT_H
#define _MAKAOS_SYS_STAT_H 1

#include <sys/types.h>
#include <time.h>

// File type bits
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_IFBLK  0060000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_IFSOCK 0140000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

// Permission bits
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRWXU (S_IRUSR|S_IWUSR|S_IXUSR)
#define S_IRWXG (S_IRGRP|S_IWGRP|S_IXGRP)
#define S_IRWXO (S_IROTH|S_IWOTH|S_IXOTH)

// Layout MUST match kernel stat_t in kernel/syscall/syscall.h — the
// kernel writes this struct directly via SYS_STAT / SYS_FSTAT, so any
// reorder silently breaks S_ISDIR / S_ISREG checks on valid inodes.
// (Hit this once: dwl's xkbcommon read st_mode from the wrong offset,
// saw "not a directory" for /usr/share/X11/xkb, and crashed.)
// Explicit widths here: POSIX type aliases are wrong size for nlink.
struct stat {
    unsigned long long  st_ino;       // off 0
    unsigned long long  st_nlink;     // off 8   (NOT nlink_t — kernel emits u64)
    unsigned int        st_mode;      // off 16
    unsigned int        st_uid;       // off 20
    unsigned int        st_gid;       // off 24
    unsigned int        _pad0;        // off 28
    long long           st_size;      // off 32
    struct timespec     st_atim;      // off 40  (16 bytes)
    struct timespec     st_mtim;      // off 56
    struct timespec     st_ctim;      // off 72
    unsigned long long  st_blksize;   // off 88
    long long           st_blocks;    // off 96
    int                 st_dev;       // off 104
    int                 st_rdev;      // off 108
};
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

int stat(const char* path, struct stat* buf);
int lstat(const char* path, struct stat* buf);
int fstat(int fd, struct stat* buf);
int fstatat(int dirfd, const char* path, struct stat* buf, int flags);
int mkdir(const char* path, mode_t mode);
int chmod(const char* path, mode_t mode);
int fchmod(int fd, mode_t mode);
mode_t umask(mode_t mask);

#endif
