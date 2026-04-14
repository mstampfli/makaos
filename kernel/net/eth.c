#include "eth.h"
#include "arp.h"
#include "ipv4.h"
#include "virtio_net.h"
#include "common.h"

const uint8_t eth_broadcast[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

void eth_recv(skbuff_t* skb) {
    serial_puts_dbg("[eth]rx\n");
    if (skb->len < ETH_HDR_LEN) { skb_free(skb); return; }

    eth_hdr_t* hdr = (eth_hdr_t*)skb->data;
    skb->eth_hdr   = hdr;
    uint16_t etype = ntoh16(hdr->ethertype);

    // Consume Ethernet header — upper layers work on the payload.
    skb_pull(skb, ETH_HDR_LEN);

    switch (etype) {
    case ETHERTYPE_ARP:
        arp_recv(skb);
        break;
    case ETHERTYPE_IPV4:
        ipv4_recv(skb);
        break;
    default:
        // Unknown EtherType — drop silently.
        skb_free(skb);
        break;
    }
}

int eth_send(skbuff_t* skb, const uint8_t* dst, uint16_t ethertype) {
    eth_hdr_t* hdr = (eth_hdr_t*)skb_push(skb, ETH_HDR_LEN);
    if (!hdr) return -1;

    __builtin_memcpy(hdr->dst, dst, ETH_ALEN);
    __builtin_memcpy(hdr->src, virtio_net_mac(), ETH_ALEN);
    hdr->ethertype = hton16(ethertype);

    return virtio_net_tx(skb->data, (uint16_t)skb->len);
}
