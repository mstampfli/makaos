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
// Fills out_mac (6 bytes) and returns 1 on hit.
// On miss, sends an ARP request and returns 0 immediately — the
// caller should retry after the net thread processes the reply.
// Taking a copy keeps the caller safe against concurrent cache grow
// / eviction on another CPU.
int arp_lookup(uint32_t ip_be, uint8_t out_mac[6]);

// Announce our own IPv4/MAC mapping (gratuitous ARP).
// Called once during net_init() to prime neighbouring hosts' caches.
void arp_announce(void);
