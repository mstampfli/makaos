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
    in_addr_t s_addr;
};

struct sockaddr_in {
    uint16_t       sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    uint8_t        sin_zero[8];
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

// Byte-order conversions — POSIX mandates these via <netinet/in.h> too.
uint16_t htons(uint16_t v);
uint16_t ntohs(uint16_t v);
uint32_t htonl(uint32_t v);
uint32_t ntohl(uint32_t v);

// ── IPv6 surface (header-level only) ────────────────────────────────
// MakaOS's stack is IPv4-only; AF_INET6 sockets fail with
// EAFNOSUPPORT at runtime.  The declarations exist because gio
// (→ pango → sway) compiles its address classes unconditionally.
#define INET_ADDRSTRLEN   16
#define INET6_ADDRSTRLEN  46
#define IPPROTO_IPV6      41

extern const struct in6_addr in6addr_any;        // ::
extern const struct in6_addr in6addr_loopback;   // ::1

#define IN6ADDR_ANY_INIT      { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } }
#define IN6ADDR_LOOPBACK_INIT { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } }

#define IN6_IS_ADDR_UNSPECIFIED(a) \
    (((const uint32_t*)(a))[0] == 0 && ((const uint32_t*)(a))[1] == 0 && \
     ((const uint32_t*)(a))[2] == 0 && ((const uint32_t*)(a))[3] == 0)
#define IN6_IS_ADDR_LOOPBACK(a) \
    (((const uint32_t*)(a))[0] == 0 && ((const uint32_t*)(a))[1] == 0 && \
     ((const uint32_t*)(a))[2] == 0 && \
     ((const uint8_t*)(a))[12] == 0 && ((const uint8_t*)(a))[13] == 0 && \
     ((const uint8_t*)(a))[14] == 0 && ((const uint8_t*)(a))[15] == 1)
#define IN6_IS_ADDR_LINKLOCAL(a) \
    (((const uint8_t*)(a))[0] == 0xFE && (((const uint8_t*)(a))[1] & 0xC0) == 0x80)
#define IN6_IS_ADDR_SITELOCAL(a) \
    (((const uint8_t*)(a))[0] == 0xFE && (((const uint8_t*)(a))[1] & 0xC0) == 0xC0)
#define IN6_IS_ADDR_MULTICAST(a)   (((const uint8_t*)(a))[0] == 0xFF)
#define IN6_IS_ADDR_MC_GLOBAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && (((const uint8_t*)(a))[1] & 0x0F) == 0x0E)
#define IN6_IS_ADDR_MC_LINKLOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && (((const uint8_t*)(a))[1] & 0x0F) == 0x02)
#define IN6_IS_ADDR_MC_NODELOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && (((const uint8_t*)(a))[1] & 0x0F) == 0x01)
#define IN6_IS_ADDR_MC_SITELOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && (((const uint8_t*)(a))[1] & 0x0F) == 0x05)
#define IN6_IS_ADDR_MC_ORGLOCAL(a) \
    (IN6_IS_ADDR_MULTICAST(a) && (((const uint8_t*)(a))[1] & 0x0F) == 0x08)
#define IN6_IS_ADDR_V4MAPPED(a) \
    (((const uint32_t*)(a))[0] == 0 && ((const uint32_t*)(a))[1] == 0 && \
     ((const uint8_t*)(a))[8] == 0 && ((const uint8_t*)(a))[9] == 0 && \
     ((const uint8_t*)(a))[10] == 0xFF && ((const uint8_t*)(a))[11] == 0xFF)

// IPv4 class/multicast tests (host byte order input).
#define IN_MULTICAST(a)  ((((in_addr_t)(a)) & 0xF0000000) == 0xE0000000)

#define IP_TTL              2
#define IPV6_UNICAST_HOPS   16

// Multicast socket options (accepted, ignored by the stack).
#define IP_MULTICAST_TTL    33
#define IP_MULTICAST_LOOP   34
#define IP_ADD_MEMBERSHIP   35
#define IP_DROP_MEMBERSHIP  36
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_JOIN_GROUP     20
#define IPV6_LEAVE_GROUP    21
#define IPV6_V6ONLY         26

struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};
struct ip_mreqn {
    struct in_addr imr_multiaddr;
    struct in_addr imr_address;
    int            imr_ifindex;
};
struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int    ipv6mr_interface;
};

#endif
