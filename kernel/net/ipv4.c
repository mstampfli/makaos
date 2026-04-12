#include "ipv4.h"
#include "eth.h"
#include "arp.h"
#include "icmp.h"
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "common.h"
#include "kheap.h"

static uint16_t s_ip_id = 0;  // monotonically increasing packet ID

// ── Internet checksum (RFC 1071) ──────────────────────────────────────────
// Sum all 16-bit words, add carry, one's complement.
uint16_t inet_checksum(const void* data, uint32_t len) {
    const uint16_t* p = (const uint16_t*)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len) sum += *(const uint8_t*)p;  // odd byte
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t inet_pseudo_partial(uint32_t src_ip_be, uint32_t dst_ip_be,
                              uint8_t proto, uint16_t payload_len) {
    uint32_t sum = 0;
    sum += (src_ip_be >> 16) & 0xFFFF;
    sum += src_ip_be         & 0xFFFF;
    sum += (dst_ip_be >> 16) & 0xFFFF;
    sum += dst_ip_be         & 0xFFFF;
    sum += hton16((uint16_t)proto);
    sum += hton16(payload_len);
    return sum;
}

__attribute__((unused))
static uint16_t transport_checksum(uint32_t src_be, uint32_t dst_be,
                                    uint8_t proto, const void* data, uint16_t len) {
    uint32_t pseudo = inet_pseudo_partial(src_be, dst_be, proto, len);
    const uint16_t* p = (const uint16_t*)data;
    uint32_t sum = pseudo;
    uint16_t l = len;
    while (l > 1) { sum += *p++; l -= 2; }
    if (l) sum += *(const uint8_t*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// ── Receive ───────────────────────────────────────────────────────────────

void ipv4_recv(skbuff_t* skb) {
    if (skb->len < IPV4_HDR_LEN) { skb_free(skb); return; }

    ipv4_hdr_t* hdr = (ipv4_hdr_t*)skb->data;
    skb->net_hdr    = hdr;

    uint8_t  version = (hdr->ver_ihl >> 4) & 0xFu;
    uint8_t  ihl     = (hdr->ver_ihl & 0xFu) * 4u;
    uint16_t tot_len = ntoh16(hdr->total_len);

    if (version != 4 || ihl < IPV4_HDR_LEN || tot_len < ihl ||
        skb->len < tot_len) {
        skb_free(skb);
        return;
    }
    serial_puts_dbg("[ip] rx proto="); serial_hex_dbg((uint64_t)hdr->protocol);
    serial_puts_dbg("[ip] rx dst="); serial_hex_dbg((uint64_t)hdr->dst);

    // Verify checksum.
    uint16_t saved = hdr->checksum;
    hdr->checksum  = 0;
    uint16_t calc  = inet_checksum(hdr, ihl);
    hdr->checksum  = saved;
    if (calc != saved) { skb_free(skb); return; }

    // Drop packets not addressed to us (unicast to our IP or broadcast).
    // Exception: while unconfigured (our_ip == 0), accept everything so DHCP
    // bootstrap works — RFC 2131 allows the server to reply either broadcast
    // or unicast to the yiaddr, and QEMU SLIRP unicasts to yiaddr even when
    // the client sets the BROADCAST flag.
    uint32_t our_ip = net_our_ip();
    if (our_ip != 0 &&
        hdr->dst != our_ip && hdr->dst != 0xFFFFFFFFu &&
        hdr->dst != net_broadcast_ip()) {
        skb_free(skb);
        return;
    }

    // Reject fragments (no reassembly yet).
    uint16_t ff = ntoh16(hdr->flags_frag);
    if ((ff & 0x2000u) || (ff & 0x1FFFu)) { skb_free(skb); return; }

    skb->src_ip_be = hdr->src;
    skb->dst_ip_be = hdr->dst;
    skb->protocol  = hdr->protocol;

    // Trim skb to declared length and consume IP header.
    skb->len = tot_len;
    skb_pull(skb, ihl);

    switch (hdr->protocol) {
    case IPPROTO_ICMP: icmp_recv(skb); break;
    case IPPROTO_TCP:  tcp_recv(skb);  break;
    case IPPROTO_UDP:  udp_recv(skb);  break;
    default:           skb_free(skb);  break;
    }
}

// ── Transmit ──────────────────────────────────────────────────────────────

int ipv4_send_ex(skbuff_t* skb, uint32_t src_ip_be, uint32_t dst_ip_be,
                  uint8_t protocol) {
    uint32_t our_ip = net_our_ip();
    if (src_ip_be == 0 && our_ip != 0) src_ip_be = our_ip;
    uint16_t payload_len = (uint16_t)skb->len;

    // Prepend IP header.
    ipv4_hdr_t* hdr = (ipv4_hdr_t*)skb_push(skb, IPV4_HDR_LEN);
    if (!hdr) return -1;

    hdr->ver_ihl   = (4u << 4) | (IPV4_HDR_LEN / 4u);
    hdr->dscp_ecn  = 0;
    hdr->total_len = hton16((uint16_t)(IPV4_HDR_LEN + payload_len));
    hdr->id        = hton16(s_ip_id++);
    hdr->flags_frag = hton16(0x4000u);  // Don't Fragment
    hdr->ttl       = 64;
    hdr->protocol  = protocol;
    hdr->checksum  = 0;
    hdr->src       = src_ip_be;
    hdr->dst       = dst_ip_be;
    hdr->checksum  = inet_checksum(hdr, IPV4_HDR_LEN);

    // Broadcast fast path: limited broadcast (255.255.255.255) and the
    // configured subnet broadcast go straight out as an L2 broadcast.
    // This is what DHCP DISCOVER/REQUEST needs before the host has an IP.
    if (dst_ip_be == 0xFFFFFFFFu ||
        (net_broadcast_ip() != 0 && dst_ip_be == net_broadcast_ip())) {
        return eth_send(skb, eth_broadcast, ETHERTYPE_IPV4);
    }

    // Resolve next-hop MAC via ARP.
    // If the destination is on the same subnet, resolve directly.
    // Otherwise use the gateway.
    uint32_t mask = net_subnet_mask();
    uint32_t next_hop = dst_ip_be;
    if (mask && (dst_ip_be & mask) != (src_ip_be & mask))
        next_hop = net_gateway_ip();
    if (next_hop == 0) return -1;  // no route

    const uint8_t* dst_mac = arp_lookup(next_hop);
    if (!dst_mac) {
        // ARP request sent; caller should retry.
        return -1;
    }

    return eth_send(skb, dst_mac, ETHERTYPE_IPV4);
}

int ipv4_send(skbuff_t* skb, uint32_t dst_ip_be, uint8_t protocol) {
    return ipv4_send_ex(skb, 0, dst_ip_be, protocol);
}
