#pragma once
#include "common.h"
#include "kheap.h"

// ── Socket Buffer (skbuff) ────────────────────────────────────────────────
// A socket buffer is the universal packet container used at every layer of
// the network stack.  It holds a single packet (or fragment) and carries
// metadata alongside the raw bytes.
//
// Layout:
//   [ headroom | data ... | tailroom ]
//         ^     ^         ^
//         buf   data      tail
//
// Headroom lets lower layers prepend headers without copying.
// Tailroom lets upper layers append trailers without copying.
//
// Ownership:
//   skb_alloc()  → caller owns the skb
//   skb_free()   → releases the skb and its data buffer
//   skb_clone()  → increments refcount; skb_free on each clone independently
//
// All fields are in HOST byte order unless suffixed _be (big-endian / network).

#define SKB_HEADROOM  128u   // bytes reserved before data for header prepending
#define SKB_TAILROOM  0u     // bytes reserved after data (not commonly needed)

typedef struct skbuff {
    uint8_t* buf;        // raw allocation base (free this)
    uint8_t* data;       // pointer to start of valid data
    uint8_t* tail;       // pointer one past end of valid data
    uint8_t* end;        // pointer one past end of allocation

    uint32_t len;        // data length in bytes (tail - data)
    uint32_t buf_size;   // total allocation size

    // Layer header pointers (set by each layer as the packet is parsed).
    // These point into data[] — do NOT free them separately.
    void*    eth_hdr;   // Ethernet header
    void*    net_hdr;   // IP header
    void*    trans_hdr; // TCP/UDP/ICMP header

    // Routing / interface metadata.
    uint32_t src_ip_be;  // source IPv4 address (network byte order)
    uint32_t dst_ip_be;  // destination IPv4 address (network byte order)
    uint16_t src_port_be;// source L4 port for UDP (stamped at udp_recv), net order
    uint8_t  protocol;   // IP protocol (IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP)

    uint32_t refcount;   // reference count (1 after alloc, skb_free when 0)

    // Intrusive list link — used by socket layer to queue datagrams.
    // Not touched by the network stack itself.
    struct skbuff* next;
} skbuff_t;

// Allocate an skbuff with `data_len` bytes of data capacity plus headroom.
// Returns NULL on allocation failure.
static inline skbuff_t* skb_alloc(uint32_t data_len) {
    skbuff_t* skb = (skbuff_t*)kmalloc(sizeof(skbuff_t));
    if (!skb) return NULL;

    uint32_t total = SKB_HEADROOM + data_len + SKB_TAILROOM;
    uint8_t* raw   = (uint8_t*)kmalloc(total);
    if (!raw) { kfree(skb); return NULL; }

    skb->buf       = raw;
    skb->buf_size  = total;
    skb->data      = raw + SKB_HEADROOM;
    skb->tail      = skb->data;
    skb->end       = raw + total;
    skb->len       = 0;
    skb->eth_hdr   = NULL;
    skb->net_hdr   = NULL;
    skb->trans_hdr = NULL;
    skb->src_ip_be = 0;
    skb->dst_ip_be = 0;
    skb->src_port_be = 0;
    skb->protocol  = 0;
    skb->refcount  = 1;
    skb->next      = NULL;
    return skb;
}

// Wrap an existing flat buffer (e.g. a DMA-received packet) in an skbuff.
// The skbuff does NOT own the buffer — caller must not free `data` before
// calling skb_free on this skbuff.
static inline skbuff_t* skb_wrap(uint8_t* data, uint32_t len) {
    skbuff_t* skb = (skbuff_t*)kmalloc(sizeof(skbuff_t));
    if (!skb) return NULL;
    skb->buf       = NULL;   // not owned
    skb->buf_size  = 0;
    skb->data      = data;
    skb->tail      = data + len;
    skb->end       = data + len;
    skb->len       = len;
    skb->eth_hdr   = NULL;
    skb->net_hdr   = NULL;
    skb->trans_hdr = NULL;
    skb->src_ip_be = 0;
    skb->dst_ip_be = 0;
    skb->src_port_be = 0;
    skb->protocol  = 0;
    skb->refcount  = 1;
    skb->next      = NULL;
    return skb;
}

// Free an skbuff.  Decrements refcount; frees when it reaches 0.
static inline void skb_free(skbuff_t* skb) {
    if (!skb) return;
    if (--skb->refcount > 0) return;
    if (skb->buf) kfree(skb->buf);
    kfree(skb);
}

// Increment refcount (clone).
static inline skbuff_t* skb_ref(skbuff_t* skb) {
    if (skb) skb->refcount++;
    return skb;
}

// Reserve `n` bytes of headroom by advancing data pointer.
// Call before writing any data — resets len to 0.
static inline void skb_reserve(skbuff_t* skb, uint32_t n) {
    skb->data += n;
    skb->tail  = skb->data;
    skb->len   = 0;
}

// Push `n` bytes at the front of the packet (prepend a header).
// Returns pointer to the new header region, or NULL if no headroom.
static inline void* skb_push(skbuff_t* skb, uint32_t n) {
    if (skb->data - skb->buf < (int64_t)n) return NULL;
    skb->data -= n;
    skb->len  += n;
    return skb->data;
}

// Put `n` bytes at the end of the packet (append data).
// Returns pointer to the appended region, or NULL if no tailroom.
static inline void* skb_put(skbuff_t* skb, uint32_t n) {
    if (skb->tail + n > skb->end) return NULL;
    void* p    = skb->tail;
    skb->tail += n;
    skb->len  += n;
    return p;
}

// Pull `n` bytes from the front (consume a header).
// Returns pointer to old data start, or NULL if not enough data.
static inline void* skb_pull(skbuff_t* skb, uint32_t n) {
    if (skb->len < n) return NULL;
    void* p    = skb->data;
    skb->data += n;
    skb->len  -= n;
    return p;
}
