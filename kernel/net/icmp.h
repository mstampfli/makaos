#pragma once
#include "skbuff.h"

// ── ICMP (RFC 792) ────────────────────────────────────────────────────────
// We implement echo request/reply (ping) only.

#define ICMP_TYPE_ECHO_REQUEST  8u
#define ICMP_TYPE_ECHO_REPLY    0u

// Receive an ICMP packet (called by ipv4_recv).
void icmp_recv(skbuff_t* skb);
