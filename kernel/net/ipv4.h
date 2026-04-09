#pragma once
#include "common.h"
#include "skbuff.h"

// ── IPv4 (RFC 791) ────────────────────────────────────────────────────────

#define IPV4_HDR_LEN   20u   // minimum header length (no options)

// IP protocol numbers.
#define IPPROTO_ICMP   1u
#define IPPROTO_TCP    6u
#define IPPROTO_UDP   17u

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;    // [7:4]=version(4), [3:0]=IHL in 32-bit words
    uint8_t  dscp_ecn;
    uint16_t total_len;  // total length including header (network byte order)
    uint16_t id;
    uint16_t flags_frag; // [15:13]=flags, [12:0]=fragment offset
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;        // network byte order
    uint32_t dst;        // network byte order
} ipv4_hdr_t;

// Receive an IPv4 datagram (called by eth_recv after stripping Ethernet header).
void ipv4_recv(skbuff_t* skb);

// Transmit an IPv4 datagram.
// `skb`      — skbuff containing the payload (TCP/UDP/ICMP); IP header will
//              be prepended.
// `dst_ip`   — destination IP (network byte order)
// `protocol` — IPPROTO_TCP / IPPROTO_UDP / IPPROTO_ICMP
// Returns 0 on success, -1 on failure (no ARP entry, no headroom, etc.).
int ipv4_send(skbuff_t* skb, uint32_t dst_ip_be, uint8_t protocol);

// Compute the Internet checksum (RFC 1071) over `len` bytes starting at `data`.
// `len` must be even; callers pad to even if needed.
uint16_t inet_checksum(const void* data, uint32_t len);

// Compute the TCP/UDP pseudo-header checksum contribution.
uint32_t inet_pseudo_partial(uint32_t src_ip_be, uint32_t dst_ip_be,
                              uint8_t proto, uint16_t payload_len);
