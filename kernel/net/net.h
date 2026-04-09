#pragma once
#include "common.h"

// ── Network subsystem ─────────────────────────────────────────────────────
// Initialises the full network stack:
//   1. virtio-net hardware
//   2. IP configuration (static, DHCP TODO)
//   3. Spawns the net-rx kernel thread (polls NIC and dispatches packets)
//
// All addresses are in network byte order (big-endian) to match the wire.

// Initialise the network subsystem.  Called once from main.c.
// Returns 1 on success, 0 if no NIC found.
int net_init(void);

// ── IP configuration accessors ────────────────────────────────────────────
// All return addresses in network byte order.

uint32_t net_our_ip(void);
uint32_t net_gateway_ip(void);
uint32_t net_subnet_mask(void);
uint32_t net_broadcast_ip(void);

// Configure IP settings (called internally or by future DHCP client).
void net_set_config(uint32_t our_ip_be, uint32_t gw_be,
                    uint32_t mask_be);

// Returns 1 if the network stack has been successfully initialised.
int net_ready(void);
