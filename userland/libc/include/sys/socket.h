#ifndef _MAKAOS_SYS_SOCKET_H
#define _MAKAOS_SYS_SOCKET_H 1

#include <sys/types.h>

// Address families
#define AF_UNSPEC  0
#define AF_UNIX    1
#define AF_INET    2
#define AF_INET6   10
#define PF_UNSPEC  AF_UNSPEC
#define PF_UNIX    AF_UNIX
#define PF_INET    AF_INET
#define PF_INET6   AF_INET6

// Socket types
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5
#define SOCK_CLOEXEC   0x80000
#define SOCK_NONBLOCK  0x00800

// Socket options
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_KEEPALIVE 9
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_LINGER    13
#define SO_BROADCAST 6
#define SO_REUSEPORT 15

// Shutdown
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

// send/recv flags
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define MSG_PEEK     0x02

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct sockaddr_storage {
    uint16_t ss_family;
    char     __pad[128 - 2];
};

struct iovec {
    void*  iov_base;
    size_t iov_len;
};

struct msghdr {
    void*         msg_name;
    socklen_t     msg_namelen;
    struct iovec* msg_iov;
    size_t        msg_iovlen;
    void*         msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

int     socket(int domain, int type, int protocol);
int     bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
int     connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
ssize_t send(int fd, const void* buf, size_t len, int flags);
ssize_t recv(int fd, void* buf, size_t len, int flags);
ssize_t sendto(int fd, const void* buf, size_t len, int flags,
                const struct sockaddr* addr, socklen_t addrlen);
ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                  struct sockaddr* addr, socklen_t* addrlen);
int     setsockopt(int fd, int level, int opt, const void* val, socklen_t n);
int     getsockopt(int fd, int level, int opt, void* val, socklen_t* n);
int     shutdown(int fd, int how);
int     socketpair(int domain, int type, int protocol, int fds[2]);

#endif
