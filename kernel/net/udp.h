#pragma once
#include "common.h"
#include "skbuff.h"

// ── UDP (RFC 768) ─────────────────────────────────────────────────────────

#define UDP_HDR_LEN  8u

// Largest UDP payload that cannot overflow EITHER 16-bit length field on the
// wire: the UDP length (UDP_HDR_LEN + payload) and the IP total length
// (IPV4_HDR_LEN + UDP_HDR_LEN + payload) must each fit in a uint16_t.
// 65535 - 20 (IP) - 8 (UDP) = 65507.  A larger payload is rejected (-EMSGSIZE)
// rather than silently truncated -- a user u32 length narrowed straight to the
// uint16_t UDP length field wraps (e.g. 65535 -> 7), under-sizing the skb and
// NULL-derefing on the unchecked skb_put.
#define UDP_MAX_PAYLOAD  65507u

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;    // header + payload
    uint16_t checksum;
} udp_hdr_t;

// Receive a UDP datagram (called by ipv4_recv).
void udp_recv(skbuff_t* skb);

// Deliver a UDP datagram to the socket bound on `port` (host byte order).
// Called internally by udp_recv.
void udp_deliver(uint16_t dst_port, skbuff_t* skb);

// Send a UDP datagram.
// `dst_ip_be` — destination IP (network byte order)
// `src_port`  — source port (host byte order)
// `dst_port`  — destination port (host byte order)
// `data`      — payload
// `len`       -- payload length (full width; > UDP_MAX_PAYLOAD -> -EMSGSIZE)
// Returns 0 on success.
int udp_send(uint32_t dst_ip_be, uint16_t src_port, uint16_t dst_port,
             const void* data, uint32_t len);

// Same as udp_send but with an explicit source IP (network byte order).
// Pass 0 for `src_ip_be` to use net_our_ip().  DHCP clients need to pass
// 0.0.0.0 explicitly before they have a lease.
int udp_send_ex(uint32_t src_ip_be, uint32_t dst_ip_be,
                 uint16_t src_port, uint16_t dst_port,
                 const void* data, uint32_t len);
