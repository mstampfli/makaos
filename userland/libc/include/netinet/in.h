#ifndef _MAKAOS_NETINET_IN_H
#define _MAKAOS_NETINET_IN_H 1

#include <sys/socket.h>
#include <stdint.h>

#define INADDR_ANY       ((uint32_t)0x00000000)
#define INADDR_BROADCAST ((uint32_t)0xffffffff)
#define INADDR_LOOPBACK  ((uint32_t)0x7f000001)

#define IPPROTO_IP     0
#define IPPROTO_ICMP   1
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17
#define IPPROTO_RAW    255

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;   // big-endian
};

struct sockaddr_in {
    uint16_t        sin_family;
    in_port_t       sin_port;      // big-endian
    in_addr_t       sin_addr;      // big-endian (flattened, no nested struct)
    unsigned char   sin_zero[8];
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    uint16_t       sin6_family;
    in_port_t      sin6_port;
    uint32_t       sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t       sin6_scope_id;
};

#endif
