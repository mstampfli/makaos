#ifndef _NETINET_IP_H
#define _NETINET_IP_H 1

// IPv4 header definitions (RFC 791).  netinet/in.h already provides IPPROTO_*,
// in_addr and the IP_* socket options; this header adds the on-the-wire header
// structs (BSD `struct ip` + Linux `struct iphdr`) and the type-of-service bits.
#include <netinet/in.h>
#include <stdint.h>

struct iphdr {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    unsigned int ihl:4;
    unsigned int version:4;
#else
    unsigned int version:4;
    unsigned int ihl:4;
#endif
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
};

struct ip {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    unsigned int ip_hl:4;               // header length
    unsigned int ip_v:4;                // version
#else
    unsigned int ip_v:4;
    unsigned int ip_hl:4;
#endif
    uint8_t  ip_tos;                    // type of service
    uint16_t ip_len;                    // total length
    uint16_t ip_id;                     // identification
    uint16_t ip_off;                    // fragment offset field
    uint8_t  ip_ttl;                    // time to live
    uint8_t  ip_p;                      // protocol
    uint16_t ip_sum;                    // checksum
    struct in_addr ip_src, ip_dst;      // source and dest address
};

// Type of service bits (RFC 791).
#define IPTOS_TOS_MASK      0x1E
#define IPTOS_LOWDELAY      0x10
#define IPTOS_THROUGHPUT    0x08
#define IPTOS_RELIABILITY   0x04
#define IPTOS_MINCOST       0x02
#define IPTOS_PREC_MASK     0xE0
#define IPTOS_PREC_ROUTINE  0x00

#define IPVERSION    4
#define IPDEFTTL     64
#define IP_MAXPACKET 65535

#endif  // _NETINET_IP_H
