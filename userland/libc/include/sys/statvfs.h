// ── sys/statvfs.h — filesystem statistics ────────────────────────────
// Port-surface header (gio's glocalfile.c needs either statfs or
// statvfs to compile its filesystem-info path).  The kernel exposes no
// fs-stats syscall yet, so both calls fail with ENOSYS and gio reports
// "filesystem info unavailable".

#ifndef _MAKAOS_SYS_STATVFS_H
#define _MAKAOS_SYS_STATVFS_H 1

#include <sys/types.h>
// fsblkcnt_t / fsfilcnt_t come from sys/types.h.

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    fsblkcnt_t    f_blocks;
    fsblkcnt_t    f_bfree;
    fsblkcnt_t    f_bavail;
    fsfilcnt_t    f_files;
    fsfilcnt_t    f_ffree;
    fsfilcnt_t    f_favail;
    unsigned long f_fsid;
    unsigned long f_flag;
    unsigned long f_namemax;
};

#define ST_RDONLY 0x0001
#define ST_NOSUID 0x0002

int statvfs(const char* path, struct statvfs* buf);
int fstatvfs(int fd, struct statvfs* buf);

#endif
