// net/if_arp.h - ARP definitions (minimal; portable code includes it but
// MakaOS exposes no ARP table, so only the common shapes are provided).
#ifndef _NET_IF_ARP_H
#define _NET_IF_ARP_H 1
#include <sys/types.h>
#include <sys/socket.h>
#define ARPHRD_ETHER 1
struct arpreq {
    struct sockaddr arp_pa;      // protocol address
    struct sockaddr arp_ha;      // hardware address
    int             arp_flags;
    struct sockaddr arp_netmask;
    char            arp_dev[16];
};
#endif
