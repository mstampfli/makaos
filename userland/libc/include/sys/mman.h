#ifndef _MAKAOS_SYS_MMAN_H
#define _MAKAOS_SYS_MMAN_H 1

#include <sys/types.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void*)-1)

#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4

// msync flags — wayland-shm uses MS_SYNC for deterministic flush.
#define MS_ASYNC       1
#define MS_INVALIDATE  2
#define MS_SYNC        4

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
int   munmap(void* addr, size_t len);
int   mprotect(void* addr, size_t len, int prot);
int   madvise(void* addr, size_t len, int advice);
int   msync(void* addr, size_t len, int flags);

// POSIX shared memory
int shm_open(const char* name, int flags, mode_t mode);
int shm_unlink(const char* name);

#endif
