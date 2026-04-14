#pragma once
#include "common.h"
#include "skbuff.h"

// ── ARP — Address Resolution Protocol (RFC 826) ──────────────────────────
// Resolves IPv4 addresses to Ethernet MAC addresses.
//
// Cache: a fixed-size table mapping IPv4 → MAC.
// On miss: broadcasts an ARP request and blocks until a reply arrives
//          (or times out after ~1 second).

// ARP cache is a dynamic hash table (see arp.c) — no fixed cap.

// Receive and process an ARP packet (called by eth_recv).
void arp_recv(skbuff_t* skb);

// Look up `ip_be` (network byte order) in the ARP cache.
// Returns a pointer to the 6-byte MAC on hit, NULL on miss.
// On miss, sends an ARP request and returns NULL immediately —
// the caller should retry after the net thread processes the reply.
const uint8_t* arp_lookup(uint32_t ip_be);

// Announce our own IPv4/MAC mapping (gratuitous ARP).
// Called once during net_init() to prime neighbouring hosts' caches.
void arp_announce(void);
