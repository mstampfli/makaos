#include "arp.h"
#include "eth.h"
#include "net.h"
#include "virtio_net.h"
#include "skbuff.h"
#include "common.h"
#include "kheap.h"
#include "smp.h"
#include "rcu.h"

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

// ── ARP cache (RCU-protected) ────────────────────────────────────────────
// Open-addressing hash table keyed by ip_be.  The whole table is a single
// object (arp_table_t) published via rcu_assign_pointer.  Readers take
// zero locks: rcu_read_lock() + walk + memcpy(6) of the MAC into a caller
// buffer + rcu_read_unlock().  Zero atomics, zero cache-line bouncing on
// the outgoing-packet hot path.
//
// Writers (arp_recv only) serialise against each other under s_arp_wlock,
// build a new table via copy-on-write, rcu_assign_pointer the new one in,
// and call_rcu the old one for deferred free.  This makes inserts
// O(cap) but insert is a control-plane event (once per new host) so the
// cost is amortised away.

#define ARP_HT_INIT_CAP 32u

typedef struct {
    uint32_t ip_be;       // 0 = empty slot
    uint8_t  mac[ETH_ALEN];
} arp_entry_t;

typedef struct {
    uint32_t    cap;      // power of two
    uint32_t    cnt;
    arp_entry_t slots[];  // flexible
} arp_table_t;

static arp_table_t* s_arp        = NULL;   // RCU-protected
static spinlock_t   s_arp_wlock  = SPINLOCK_INIT;

static uint32_t arp_hash(uint32_t ip_be, uint32_t cap) {
    return (ip_be * 2654435761u) & (cap - 1u);
}

static arp_table_t* arp_table_alloc(uint32_t cap) {
    arp_table_t* t = (arp_table_t*)kmalloc(sizeof(arp_table_t) +
                                           (uint64_t)cap * sizeof(arp_entry_t));
    if (!t) return NULL;
    t->cap = cap;
    t->cnt = 0;
    __builtin_memset(t->slots, 0, (uint64_t)cap * sizeof(arp_entry_t));
    return t;
}

static void arp_table_raw_insert(arp_table_t* t, uint32_t ip_be, const uint8_t* mac) {
    uint32_t i = arp_hash(ip_be, t->cap);
    for (;;) {
        if (!t->slots[i].ip_be) {
            t->slots[i].ip_be = ip_be;
            __builtin_memcpy(t->slots[i].mac, mac, ETH_ALEN);
            t->cnt++;
            return;
        }
        if (t->slots[i].ip_be == ip_be) {
            __builtin_memcpy(t->slots[i].mac, mac, ETH_ALEN);
            return;
        }
        i = (i + 1u) & (t->cap - 1u);
    }
}

static void arp_table_free_rcu(void* p) { kfree(p); }

// Writer: called from arp_recv only.  Must hold s_arp_wlock.
// Builds a fresh table, copies existing entries (or grows), inserts the
// new mapping, publishes via rcu_assign_pointer, defers the old free.
static void cache_insert_locked(uint32_t ip_be, const uint8_t* mac) {
    arp_table_t* old = s_arp;
    uint32_t old_cap = old ? old->cap : 0;
    uint32_t old_cnt = old ? old->cnt : 0;

    // Decide target capacity: init or 75%-load grow.
    uint32_t new_cap = old_cap ? old_cap : ARP_HT_INIT_CAP;
    if ((old_cnt + 1u) * 4u >= new_cap * 3u) new_cap = old_cap ? old_cap * 2u : ARP_HT_INIT_CAP;

    arp_table_t* neu = arp_table_alloc(new_cap);
    if (!neu) return;

    // Copy existing entries.
    if (old) {
        for (uint32_t i = 0; i < old->cap; i++)
            if (old->slots[i].ip_be)
                arp_table_raw_insert(neu, old->slots[i].ip_be, old->slots[i].mac);
    }
    // Insert (or overwrite) the new mapping.
    arp_table_raw_insert(neu, ip_be, mac);

    rcu_assign_pointer(s_arp, neu);
    if (old) call_rcu(arp_table_free_rcu, old);
}

static void cache_insert(uint32_t ip_be, const uint8_t* mac) {
    if (!ip_be) return;
    uint64_t flags = spin_lock_irqsave(&s_arp_wlock);
    cache_insert_locked(ip_be, mac);
    spin_unlock_irqrestore(&s_arp_wlock, flags);
}

// ── ARP send helpers ──────────────────────────────────────────────────────

static void arp_send(uint16_t oper, const uint8_t* tha,
                     uint32_t tpa_be, const uint8_t* dst_mac) {
    skbuff_t* skb = skb_alloc(sizeof(arp_pkt_t) + ETH_HDR_LEN);
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
    __builtin_memcpy(pkt->sha, our_mac, ETH_ALEN);
    pkt->spa = our_ip;
    __builtin_memcpy(pkt->tha, tha, ETH_ALEN);
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

int arp_lookup(uint32_t ip_be, uint8_t out_mac[ETH_ALEN]) {
    // RCU reader fast path — zero atomics, just preempt_disable and a
    // pointer load.  The reader takes a local copy of the 6-byte MAC,
    // so the table object can be reclaimed as soon as the grace period
    // elapses.
    int hit = 0;
    rcu_read_lock();
    arp_table_t* t = rcu_dereference(s_arp);
    if (t) {
        uint32_t cap = t->cap;
        uint32_t i = arp_hash(ip_be, cap);
        for (uint32_t n = 0; n < cap; n++) {
            uint32_t k = t->slots[i].ip_be;
            if (!k) break;
            if (k == ip_be) {
                __builtin_memcpy(out_mac, t->slots[i].mac, ETH_ALEN);
                hit = 1;
                break;
            }
            i = (i + 1u) & (cap - 1u);
        }
    }
    rcu_read_unlock();
    if (hit) return 1;

    // Miss: broadcast request, caller retries on the next send.
    uint8_t zero[ETH_ALEN] = {0};
    arp_send(ARP_OP_REQUEST, zero, ip_be, eth_broadcast);
    return 0;
}

void arp_announce(void) {
    // Gratuitous ARP: broadcast a reply with our own IP as both sender and
    // target.  This updates the ARP caches of other hosts on the LAN.
    uint32_t our_ip = net_our_ip();
    arp_send(ARP_OP_REPLY, (const uint8_t*)eth_broadcast, our_ip, eth_broadcast);
}
