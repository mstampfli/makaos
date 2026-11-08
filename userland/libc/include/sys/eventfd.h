#ifndef _MAKAOS_SYS_EVENTFD_H
#define _MAKAOS_SYS_EVENTFD_H 1
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// eventfd flags (Linux-compatible)
#define EFD_SEMAPHORE 0x0001
#define EFD_CLOEXEC   0x0002
#define EFD_NONBLOCK  0x0004

typedef uint64_t eventfd_t;

int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t* value);
int eventfd_write(int fd, eventfd_t value);

#ifdef __cplusplus
}
#endif
#endif
