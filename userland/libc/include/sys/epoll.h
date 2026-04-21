#ifndef _MAKAOS_SYS_EPOLL_H
#define _MAKAOS_SYS_EPOLL_H 1

#include <stdint.h>

#define EPOLLIN      0x00000001u
#define EPOLLPRI     0x00000002u
#define EPOLLOUT     0x00000004u
#define EPOLLERR     0x00000008u
#define EPOLLHUP     0x00000010u
#define EPOLLRDHUP   0x00002000u
#define EPOLLET      0x80000000u
#define EPOLLONESHOT 0x40000000u

// O_CLOEXEC flag for epoll_create1 — close on execve.
#define EPOLL_CLOEXEC 02000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef union epoll_data {
    void*    ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
} __attribute__((packed));

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev);
int epoll_wait(int epfd, struct epoll_event* evs, int max, int timeout);

#endif
