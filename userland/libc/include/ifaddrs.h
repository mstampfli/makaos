// ifaddrs.h - interface address enumeration (Linux-compatible).
//
// MakaOS does not yet enumerate interface addresses, so getifaddrs() returns an
// empty list (see ifaddrs.c). Consumers that iterate the list (e.g. OpenJDK's
// per-interface network counters) then simply find nothing.

#ifndef _IFADDRS_H
#define _IFADDRS_H 1

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs {
    struct ifaddrs*  ifa_next;
    char*            ifa_name;
    unsigned int     ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
    union {
        struct sockaddr* ifu_broadaddr;
        struct sockaddr* ifu_dstaddr;
    } ifa_ifu;
    void*            ifa_data;
};

#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr   ifa_ifu.ifu_dstaddr

int  getifaddrs(struct ifaddrs** ifap);
void freeifaddrs(struct ifaddrs* ifa);

#ifdef __cplusplus
}
#endif

#endif /* ifaddrs.h */
