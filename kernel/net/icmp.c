#include "icmp.h"
#include "ipv4.h"
#include "skbuff.h"
#include "common.h"
#include "kheap.h"

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} icmp_hdr_t;

void icmp_recv(skbuff_t* skb) {
    if (skb->len < sizeof(icmp_hdr_t)) { skb_free(skb); return; }

    icmp_hdr_t* hdr = (icmp_hdr_t*)skb->data;

    // Verify ICMP checksum over entire ICMP message.
    uint16_t saved = hdr->checksum;
    hdr->checksum  = 0;
    uint16_t calc  = inet_checksum(skb->data, skb->len);
    hdr->checksum  = saved;
    if (calc != saved) { skb_free(skb); return; }

    if (hdr->type == ICMP_TYPE_ECHO_REQUEST) {
        // Build echo reply by reusing the same payload.
        skbuff_t* reply = skb_alloc(skb->len + IPV4_HDR_LEN);
        if (!reply) { skb_free(skb); return; }
        skb_reserve(reply, IPV4_HDR_LEN);  // reserve room for IP header

        uint8_t* out = (uint8_t*)skb_put(reply, skb->len);
        if (!out) { skb_free(reply); skb_free(skb); return; }

        // Copy entire ICMP message.
        __builtin_memcpy(out, skb->data, skb->len);

        // Change type to reply.
        icmp_hdr_t* rhdr = (icmp_hdr_t*)out;
        rhdr->type     = ICMP_TYPE_ECHO_REPLY;
        rhdr->checksum = 0;
        rhdr->checksum = inet_checksum(out, skb->len);

        ipv4_send(reply, skb->src_ip_be, IPPROTO_ICMP);
        skb_free(reply);
    }

    skb_free(skb);
}
