// sys/shm.h - System V shared memory (Linux-compatible subset).
//
// MakaOS has no SysV shared memory, so the operations fail with ENOSYS (see
// sys_shm.c). Consumers that use it as an optional acceleration (e.g. OpenJDK's
// UseSHM large-page path) detect the failure and fall back.

#ifndef _SYS_SHM_H
#define _SYS_SHM_H 1

#include <sys/ipc.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long shmatt_t;

struct shmid_ds {
    struct ipc_perm shm_perm;
    size_t          shm_segsz;
    long            shm_atime;
    long            shm_dtime;
    long            shm_ctime;
    pid_t           shm_cpid;
    pid_t           shm_lpid;
    shmatt_t        shm_nattch;
};

// shmget/shmat flags.
#define SHM_R       0400
#define SHM_W       0200
#define SHM_X       0100
#define SHM_RDONLY  010000
#define SHM_RND     020000
#define SHM_REMAP   040000
#define SHM_HUGETLB 04000

// Segment-attach address must be a multiple of SHMLBA.
#define SHMLBA 4096

int   shmget(key_t key, size_t size, int shmflg);
void* shmat(int shmid, const void* shmaddr, int shmflg);
int   shmdt(const void* shmaddr);
int   shmctl(int shmid, int cmd, struct shmid_ds* buf);

#ifdef __cplusplus
}
#endif

#endif /* sys/shm.h */
