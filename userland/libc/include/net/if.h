// ── net/if.h — network interface definitions ────────────────────────
// Provides interface naming plus the request structures, flags and ioctl
// numbers portable code (e.g. OpenJDK's NetworkInterface) uses to enumerate
// interfaces. MakaOS answers what its stack can and fails the rest.

#ifndef _MAKAOS_NET_IF_H
#define _MAKAOS_NET_IF_H 1
#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>

#define IF_NAMESIZE 16
#define IFNAMSIZ    IF_NAMESIZE
#define IFHWADDRLEN 6

// Interface flags (ifr_flags / SIOCGIFFLAGS).
#define IFF_UP          0x1
#define IFF_BROADCAST   0x2
#define IFF_DEBUG       0x4
#define IFF_LOOPBACK    0x8
#define IFF_POINTOPOINT 0x10
#define IFF_NOTRAILERS  0x20
#define IFF_RUNNING     0x40
#define IFF_NOARP       0x80
#define IFF_PROMISC     0x100
#define IFF_ALLMULTI    0x200
#define IFF_MULTICAST   0x1000

struct ifreq {
    union {
        char ifrn_name[IFNAMSIZ];
    } ifr_ifrn;
    union {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short           ifru_flags;
        int             ifru_ivalue;   // index / metric
        int             ifru_mtu;
        char            ifru_slave[IFNAMSIZ];
        char            ifru_newname[IFNAMSIZ];
        char*           ifru_data;
    } ifr_ifru;
};
#define ifr_name      ifr_ifrn.ifrn_name
#define ifr_addr      ifr_ifru.ifru_addr
#define ifr_dstaddr   ifr_ifru.ifru_dstaddr
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_netmask   ifr_ifru.ifru_netmask
#define ifr_hwaddr    ifr_ifru.ifru_hwaddr
#define ifr_flags     ifr_ifru.ifru_flags
#define ifr_ifindex   ifr_ifru.ifru_ivalue
#define ifr_index     ifr_ifru.ifru_ivalue
#define ifr_metric    ifr_ifru.ifru_ivalue
#define ifr_mtu       ifr_ifru.ifru_mtu
#define ifr_data      ifr_ifru.ifru_data

struct ifconf {
    int ifc_len;
    union {
        char*         ifcu_buf;
        struct ifreq* ifcu_req;
    } ifc_ifcu;
};
#define ifc_buf ifc_ifcu.ifcu_buf
#define ifc_req ifc_ifcu.ifcu_req

// Interface ioctls (Linux values).
#define SIOCGIFNAME     0x8910
#define SIOCGIFCONF     0x8912
#define SIOCGIFFLAGS    0x8913
#define SIOCGIFADDR     0x8915
#define SIOCGIFDSTADDR  0x8917
#define SIOCGIFBRDADDR  0x8919
#define SIOCGIFNETMASK  0x891b
#define SIOCGIFMETRIC   0x891d
#define SIOCGIFMTU      0x8921
#define SIOCGIFHWADDR   0x8927
#define SIOCGIFINDEX    0x8933

unsigned int if_nametoindex(const char* name);
char*        if_indextoname(unsigned int index, char* name);

#ifdef __cplusplus
}
#endif
#endif
