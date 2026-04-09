#include "udp.h"
#include "ipv4.h"
#include "eth.h"
#include "net.h"
#include "socket.h"
#include "common.h"
#include "kheap.h"

void udp_recv(skbuff_t* skb) {
    if (skb->len < UDP_HDR_LEN) { skb_free(skb); return; }

    udp_hdr_t* hdr    = (udp_hdr_t*)skb->data;
    uint16_t   dst_p  = ntoh16(hdr->dst_port);
    uint16_t   udplen = ntoh16(hdr->length);

    if (udplen < UDP_HDR_LEN || udplen > skb->len) { skb_free(skb); return; }

    // Verify checksum (optional in IPv4 — skip if 0).
    if (hdr->checksum != 0) {
        uint16_t saved    = hdr->checksum;
        hdr->checksum     = 0;
        uint32_t pseudo   = inet_pseudo_partial(skb->src_ip_be, skb->dst_ip_be,
                                                 IPPROTO_UDP, udplen);
        const uint16_t* p = (const uint16_t*)skb->data;
        uint32_t sum      = pseudo;
        uint16_t l        = udplen;
        while (l > 1) { sum += *p++; l -= 2; }
        if (l) sum += *(const uint8_t*)p;
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        uint16_t calc = (uint16_t)~sum;
        hdr->checksum = saved;
        if (calc != saved) { skb_free(skb); return; }
    }

    skb->trans_hdr = hdr;
    skb_pull(skb, UDP_HDR_LEN);
    skb->len = udplen - UDP_HDR_LEN;

    // Deliver to the socket layer.
    socket_deliver_udp(dst_p, skb);
}

int udp_send(uint32_t dst_ip_be, uint16_t src_port, uint16_t dst_port,
             const void* data, uint16_t len) {
    uint16_t udplen = (uint16_t)(UDP_HDR_LEN + len);
    skbuff_t* skb = skb_alloc(udplen + IPV4_HDR_LEN);
    if (!skb) return -1;
    skb_reserve(skb, IPV4_HDR_LEN);  // leave room for IP header

    udp_hdr_t* hdr = (udp_hdr_t*)skb_put(skb, UDP_HDR_LEN);
    hdr->src_port = hton16(src_port);
    hdr->dst_port = hton16(dst_port);
    hdr->length   = hton16(udplen);
    hdr->checksum = 0;

    uint8_t* payload = (uint8_t*)skb_put(skb, len);
    const uint8_t* src = (const uint8_t*)data;
    for (uint16_t i = 0; i < len; i++) payload[i] = src[i];

    // Compute UDP checksum (optional but correct).
    uint32_t our_ip = net_our_ip();
    uint32_t pseudo = inet_pseudo_partial(our_ip, dst_ip_be, IPPROTO_UDP, udplen);
    const uint16_t* p = (const uint16_t*)hdr;
    uint32_t sum = pseudo;
    uint16_t l = udplen;
    while (l > 1) { sum += *p++; l -= 2; }
    if (l) sum += *(const uint8_t*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    hdr->checksum = (uint16_t)~sum;
    if (hdr->checksum == 0) hdr->checksum = 0xFFFF;  // RFC 768: 0 means disabled

    int r = ipv4_send(skb, dst_ip_be, IPPROTO_UDP);
    skb_free(skb);
    return r;
}
