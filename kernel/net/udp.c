#include "udp.h"
#include "ipv4.h"
#include "eth.h"
#include "net.h"
#include "socket.h"
#include "common.h"
#include "kheap.h"
#include "errno.h"

void udp_recv(skbuff_t* skb) {
    if (skb->len < UDP_HDR_LEN) { skb_free(skb); return; }

    udp_hdr_t* hdr    = (udp_hdr_t*)skb->data;
    uint16_t   dst_p  = ntoh16(hdr->dst_port);
    uint16_t   udplen = ntoh16(hdr->length);
    serial_puts_dbg("[udp] rx src="); serial_hex_dbg((uint64_t)ntoh16(hdr->src_port));
    serial_puts_dbg("[udp] rx dst="); serial_hex_dbg((uint64_t)dst_p);

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
    skb->src_port_be = hdr->src_port;  // stash for socket_recvfrom()
    skb_pull(skb, UDP_HDR_LEN);
    skb->len = udplen - UDP_HDR_LEN;

    // Deliver to the socket layer.
    socket_deliver_udp(dst_p, skb);
}

int udp_send_ex(uint32_t src_ip_be, uint32_t dst_ip_be,
                 uint16_t src_port, uint16_t dst_port,
                 const void* data, uint32_t len) {
    // Reject an over-large datagram BEFORE narrowing into the 16-bit UDP length
    // field.  `len` arrives full-width from the socket layer; without this a
    // user len in [65528,65535] makes UDP_HDR_LEN + len wrap the uint16_t udplen
    // (e.g. 65535 -> 7), under-sizing the skb so the very first skb_put returns
    // NULL and the header write below #PF-panics the kernel (unprivileged DoS).
    if (len > UDP_MAX_PAYLOAD) return -EMSGSIZE;
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
    __builtin_memcpy(payload, data, len);

    // Compute UDP checksum over the real source IP (what will go on the wire).
    uint32_t eff_src = src_ip_be ? src_ip_be : net_our_ip();
    uint32_t pseudo = inet_pseudo_partial(eff_src, dst_ip_be, IPPROTO_UDP, udplen);
    const uint16_t* p = (const uint16_t*)hdr;
    uint32_t sum = pseudo;
    uint16_t l = udplen;
    while (l > 1) { sum += *p++; l -= 2; }
    if (l) sum += *(const uint8_t*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    hdr->checksum = (uint16_t)~sum;
    if (hdr->checksum == 0) hdr->checksum = 0xFFFF;  // RFC 768: 0 means disabled

    int r = ipv4_send_ex(skb, src_ip_be, dst_ip_be, IPPROTO_UDP);
    skb_free(skb);
    return r;
}

int udp_send(uint32_t dst_ip_be, uint16_t src_port, uint16_t dst_port,
             const void* data, uint32_t len) {
    return udp_send_ex(0, dst_ip_be, src_port, dst_port, data, len);
}

// Deterministic check of the UDP send-size guard: every oversize length is
// rejected with -EMSGSIZE BEFORE any alloc/send, so the old user-u32 -> u16
// UDP-length wrap (e.g. 65535 -> udplen 7 -> under-sized skb -> NULL skb_put
// deref) can no longer panic the kernel.  The reject path returns at the first
// statement, so it never touches the network or the dummy buffer.
void udp_send_size_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    static uint8_t dummy[4];

    // The cap must be the exact no-wrap boundary for BOTH 16-bit length fields.
    if ((uint32_t)UDP_HDR_LEN + UDP_MAX_PAYLOAD > 0xFFFFu) fails++;
    if ((uint32_t)IPV4_HDR_LEN + UDP_HDR_LEN + UDP_MAX_PAYLOAD > 0xFFFFu) fails++;

    uint32_t bad[] = { UDP_MAX_PAYLOAD + 1u, 65528u, 65535u, 100000u, 0xFFFFFFFFu };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int r = udp_send_ex(0, 0x08080808u, 1234, 53, dummy, bad[i]);
        if (r != -EMSGSIZE) {
            kprintf("[udp_size] FAIL len=%lu ret=%d (want -EMSGSIZE)\n",
                    (unsigned long)bad[i], r);
            fails++;
        }
    }
    kprintf(fails ? "[udp_size] SELF-TEST FAILED\n"
                  : "[udp_size] SELF-TEST PASSED (oversize UDP rejected, no u16 wrap)\n");
}
