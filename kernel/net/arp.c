#include "arp.h"
#include "eth.h"
#include "net.h"
#include "virtio_net.h"
#include "skbuff.h"
#include "../common.h"
#include "../kheap.h"

// ── ARP packet layout (RFC 826, §3) ──────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t htype;      // hardware type: 1 = Ethernet
    uint16_t ptype;      // protocol type: 0x0800 = IPv4
    uint8_t  hlen;       // hardware address length: 6
    uint8_t  plen;       // protocol address length: 4
    uint16_t oper;       // operation: 1 = request, 2 = reply
    uint8_t  sha[6];     // sender hardware address
    uint32_t spa;        // sender protocol address (IPv4, network byte order)
    uint8_t  tha[6];     // target hardware address
    uint32_t tpa;        // target protocol address (IPv4, network byte order)
} arp_pkt_t;

#define ARP_HTYPE_ETH  1u
#define ARP_PTYPE_IPV4 0x0800u
#define ARP_OP_REQUEST 1u
#define ARP_OP_REPLY   2u

// ── ARP cache ─────────────────────────────────────────────────────────────

typedef struct {
    uint32_t ip_be;       // 0 = empty slot
    uint8_t  mac[ETH_ALEN];
} arp_entry_t;

static arp_entry_t s_cache[ARP_CACHE_SIZE];
static uint8_t     s_cache_next = 0;   // round-robin eviction

static void cache_insert(uint32_t ip_be, const uint8_t* mac) {
    // Update existing entry if present.
    for (uint8_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (s_cache[i].ip_be == ip_be) {
            for (int j = 0; j < ETH_ALEN; j++) s_cache[i].mac[j] = mac[j];
            return;
        }
    }
    // Insert into next eviction slot.
    s_cache[s_cache_next].ip_be = ip_be;
    for (int j = 0; j < ETH_ALEN; j++) s_cache[s_cache_next].mac[j] = mac[j];
    s_cache_next = (uint8_t)((s_cache_next + 1u) % ARP_CACHE_SIZE);
}

// ── ARP send helpers ──────────────────────────────────────────────────────

static void arp_send(uint16_t oper, const uint8_t* tha,
                     uint32_t tpa_be, const uint8_t* dst_mac) {
    skbuff_t* skb = skb_alloc(sizeof(arp_pkt_t));
    if (!skb) return;
    skb_reserve(skb, ETH_HDR_LEN);  // reserve room for Ethernet header

    arp_pkt_t* pkt = (arp_pkt_t*)skb_put(skb, sizeof(arp_pkt_t));
    if (!pkt) { skb_free(skb); return; }

    const uint8_t* our_mac = virtio_net_mac();
    uint32_t       our_ip  = net_our_ip();

    pkt->htype = hton16(ARP_HTYPE_ETH);
    pkt->ptype = hton16(ARP_PTYPE_IPV4);
    pkt->hlen  = ETH_ALEN;
    pkt->plen  = 4;
    pkt->oper  = hton16(oper);
    for (int i = 0; i < ETH_ALEN; i++) pkt->sha[i] = our_mac[i];
    pkt->spa = our_ip;
    for (int i = 0; i < ETH_ALEN; i++) pkt->tha[i] = tha[i];
    pkt->tpa = tpa_be;

    eth_send(skb, dst_mac, ETHERTYPE_ARP);
    skb_free(skb);
}

// ── Public API ────────────────────────────────────────────────────────────

void arp_recv(skbuff_t* skb) {
    if (skb->len < sizeof(arp_pkt_t)) { skb_free(skb); return; }

    arp_pkt_t* pkt = (arp_pkt_t*)skb->data;

    if (ntoh16(pkt->htype) != ARP_HTYPE_ETH ||
        ntoh16(pkt->ptype) != ARP_PTYPE_IPV4 ||
        pkt->hlen != ETH_ALEN || pkt->plen != 4) {
        skb_free(skb);
        return;
    }

    // Always cache the sender's mapping.
    cache_insert(pkt->spa, pkt->sha);

    uint16_t oper = ntoh16(pkt->oper);
    if (oper == ARP_OP_REQUEST && pkt->tpa == net_our_ip()) {
        // Someone is asking for our MAC — send a unicast reply.
        arp_send(ARP_OP_REPLY, pkt->sha, pkt->spa, pkt->sha);
    }

    skb_free(skb);
}

const uint8_t* arp_lookup(uint32_t ip_be) {
    for (uint8_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (s_cache[i].ip_be == ip_be)
            return s_cache[i].mac;
    }
    // Miss: send a broadcast request.  Caller must retry.
    uint8_t zero[ETH_ALEN] = {0};
    arp_send(ARP_OP_REQUEST, zero, ip_be, eth_broadcast);
    return NULL;
}

void arp_announce(void) {
    // Gratuitous ARP: broadcast a reply with our own IP as both sender and
    // target.  This updates the ARP caches of other hosts on the LAN.
    uint32_t our_ip = net_our_ip();
    arp_send(ARP_OP_REPLY, (const uint8_t*)eth_broadcast, our_ip, eth_broadcast);
}
