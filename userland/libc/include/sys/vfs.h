// sys/vfs.h - filesystem statistics (Linux statfs, compatible layout).
//
// struct statfs matches the Linux x86-64 field order so portable code that
// checks f_type against a filesystem magic (e.g. OpenJDK's cgroup detection)
// compiles and behaves. MakaOS has no statfs syscall yet, so statfs()/fstatfs()
// currently fail with ENOSYS; see sys_statfs.c.

#ifndef _SYS_VFS_H
#define _SYS_VFS_H 1

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int __val[2]; } __makaos_fsid_t;

struct statfs {
    long            f_type;     /* filesystem type magic */
    long            f_bsize;    /* optimal transfer block size */
    unsigned long   f_blocks;   /* total data blocks */
    unsigned long   f_bfree;    /* free blocks */
    unsigned long   f_bavail;   /* free blocks for unprivileged users */
    unsigned long   f_files;    /* total inodes */
    unsigned long   f_ffree;    /* free inodes */
    __makaos_fsid_t f_fsid;     /* filesystem id */
    long            f_namelen;  /* max filename length */
    long            f_frsize;   /* fragment size */
    long            f_flags;    /* mount flags */
    long            f_spare[4];
};

int statfs(const char* path, struct statfs* buf);
int fstatfs(int fd, struct statfs* buf);

#ifdef __cplusplus
}
#endif

#endif /* sys/vfs.h */
