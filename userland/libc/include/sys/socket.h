#ifndef _MAKAOS_SYS_SOCKET_H
#define _MAKAOS_SYS_SOCKET_H 1

#include <sys/types.h>

// Address families
#define AF_UNSPEC  0
#define AF_UNIX    1
#define AF_LOCAL   AF_UNIX   // POSIX alias
#define AF_INET    2
#define AF_INET6   10
#define PF_UNSPEC  AF_UNSPEC
#define PF_UNIX    AF_UNIX
#define PF_LOCAL   AF_UNIX
#define PF_INET    AF_INET
#define PF_INET6   AF_INET6

// SO_PEERCRED — Linux socket option to retrieve peer credentials on
// an AF_UNIX stream socket.  Used by wayland-server to authenticate
// compositor clients.  See getsockopt(fd, SOL_SOCKET, SO_PEERCRED).
#define SO_PEERCRED 17
#define SO_ACCEPTCONN 30
struct ucred {
    int32_t  pid;
    uint32_t uid;
    uint32_t gid;
};

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
#define SOMAXCONN    128
// Timeouts — accepted by setsockopt, not yet enforced by the stack.
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_KEEPALIVE 9
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_LINGER    13
#define SO_BROADCAST 6
#define SO_REUSEPORT 15
#define SO_DOMAIN    39  // Linux: query socket's address family

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

// struct iovec is the canonical POSIX scatter-gather descriptor; real
// home is <sys/uio.h>.  Include it here so socket code that only pulls
// in <sys/socket.h> still gets the type without duplicating it.
#include <sys/uio.h>

struct msghdr {
    void*         msg_name;
    socklen_t     msg_namelen;
    struct iovec* msg_iov;
    size_t        msg_iovlen;
    void*         msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

struct cmsghdr {
    size_t cmsg_len;    // full header + data length
    int    cmsg_level;  // SOL_SOCKET
    int    cmsg_type;   // SCM_RIGHTS, SCM_CREDENTIALS, ...
    // followed by data, aligned to sizeof(size_t)
};

// SCM_RIGHTS: pass file descriptors over AF_UNIX sockets.
#define SCM_RIGHTS 0x01

// MSG_CMSG_CLOEXEC: received fds are created with O_CLOEXEC.
#define MSG_CMSG_CLOEXEC 0x40000000

// cmsg iteration macros (Linux-compatible layout).  size_t alignment.
#define __CMSG_ALIGN(n) (((n) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_ALIGN(n)   __CMSG_ALIGN(n)
#define CMSG_LEN(n)     (__CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_SPACE(n)   (__CMSG_ALIGN(sizeof(struct cmsghdr)) + __CMSG_ALIGN(n))
#define CMSG_DATA(cmsg) ((unsigned char*)(cmsg) + __CMSG_ALIGN(sizeof(struct cmsghdr)))

#define CMSG_FIRSTHDR(mhdr) \
    ((mhdr)->msg_controllen >= sizeof(struct cmsghdr) \
        ? (struct cmsghdr*)(mhdr)->msg_control : (struct cmsghdr*)0)

#define CMSG_NXTHDR(mhdr, cmsg) \
    ((cmsg)->cmsg_len < sizeof(struct cmsghdr) ? (struct cmsghdr*)0 : \
     ((unsigned char*)(cmsg) + __CMSG_ALIGN((cmsg)->cmsg_len) \
      + sizeof(struct cmsghdr) > \
      (unsigned char*)(mhdr)->msg_control + (mhdr)->msg_controllen) \
        ? (struct cmsghdr*)0 \
        : (struct cmsghdr*)((unsigned char*)(cmsg) + __CMSG_ALIGN((cmsg)->cmsg_len)))

int     socket(int domain, int type, int protocol);
int     bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
// accept4 — accept with SOCK_NONBLOCK/SOCK_CLOEXEC applied atomically.
int     accept4(int fd, struct sockaddr* addr, socklen_t* addrlen, int flags);
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
// Local/peer address queries — no kernel record yet; fail ENOTSUP.
int     getsockname(int fd, struct sockaddr* addr, socklen_t* len);
int     getpeername(int fd, struct sockaddr* addr, socklen_t* len);
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t recvmsg(int fd, struct msghdr* msg, int flags);

#endif
