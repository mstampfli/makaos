#pragma once
#include "common.h"
#include "skbuff.h"

// ── Ethernet (IEEE 802.3) ─────────────────────────────────────────────────

#define ETH_ALEN        6u      // MAC address length in bytes
#define ETH_HDR_LEN     14u     // dst(6) + src(6) + ethertype(2)

// EtherType values (network byte order).
#define ETHERTYPE_IPV4  0x0800u
#define ETHERTYPE_ARP   0x0806u
#define ETHERTYPE_IPV6  0x86DDu

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;     // network byte order
} eth_hdr_t;

// Byte-order helpers (no compiler builtins — freestanding).
static inline uint16_t hton16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint32_t hton32(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x000000FFu) << 24);
}
static inline uint16_t ntoh16(uint16_t v) { return hton16(v); }
static inline uint32_t ntoh32(uint32_t v) { return hton32(v); }

// Broadcast MAC address.
extern const uint8_t eth_broadcast[ETH_ALEN];

// Receive an Ethernet frame (called by the net thread for each received skb).
// Dispatches to ARP or IP layer based on EtherType.
void eth_recv(skbuff_t* skb);

// Transmit an Ethernet frame.
// Prepends an Ethernet header to `skb` and sends it via the NIC.
// `dst`       — destination MAC address (6 bytes)
// `ethertype` — host byte order EtherType (e.g. ETHERTYPE_IPV4)
int eth_send(skbuff_t* skb, const uint8_t* dst, uint16_t ethertype);
