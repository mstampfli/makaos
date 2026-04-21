// ── arpa_inet.c — <arpa/inet.h> byte-order conversions ──────────────
//
// x86_64 is little-endian; the network order is big-endian.  The
// compiler recognizes the idiomatic shift+mask patterns below and
// emits `bswap` or `movbe` where available, so these are zero-overhead.

#include <stdint.h>

uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
uint16_t ntohs(uint16_t v) { return htons(v); }

uint32_t htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) | (((v >> 16) & 0xFF) << 8)
         | (((v >> 8) & 0xFF) << 16) | ((v & 0xFF) << 24);
}
uint32_t ntohl(uint32_t v) { return htonl(v); }
