#pragma once
#include "../common.h"
#include "skbuff.h"

// ── UDP (RFC 768) ─────────────────────────────────────────────────────────

#define UDP_HDR_LEN  8u

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
// `len`       — payload length
// Returns 0 on success.
int udp_send(uint32_t dst_ip_be, uint16_t src_port, uint16_t dst_port,
             const void* data, uint16_t len);
