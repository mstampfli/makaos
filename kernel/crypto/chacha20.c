// ── ChaCha20 (RFC 8439) — portable C implementation ───────────────

#include "chacha20.h"

#define ROTL32(x, n)  (((x) << (n)) | ((x) >> (32 - (n))))

#define QR(a, b, c, d)                \
    do {                               \
        a += b; d ^= a; d = ROTL32(d, 16); \
        c += d; b ^= c; b = ROTL32(b, 12); \
        a += b; d ^= a; d = ROTL32(d,  8); \
        c += d; b ^= c; b = ROTL32(b,  7); \
    } while (0)

static uint32_t le32(const uint8_t* p) {
    return  (uint32_t)p[0]        | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) v;        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void chacha20_block(uint32_t out[16],
                     const uint8_t key[CHACHA20_KEY_SZ],
                     const uint8_t nonce[CHACHA20_NONCE_SZ],
                     uint32_t counter) {
    // Initial state: "expand 32-byte k" || key || counter || nonce.
    uint32_t s[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu;
    s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++)  s[4 + i]  = le32(key   + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++)  s[13 + i] = le32(nonce + 4 * i);

    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = s[i];

    // 20 rounds = 10 iterations of (column round + diagonal round).
    for (int i = 0; i < 10; i++) {
        // column rounds
        QR(x[ 0], x[ 4], x[ 8], x[12]);
        QR(x[ 1], x[ 5], x[ 9], x[13]);
        QR(x[ 2], x[ 6], x[10], x[14]);
        QR(x[ 3], x[ 7], x[11], x[15]);
        // diagonal rounds
        QR(x[ 0], x[ 5], x[10], x[15]);
        QR(x[ 1], x[ 6], x[11], x[12]);
        QR(x[ 2], x[ 7], x[ 8], x[13]);
        QR(x[ 3], x[ 4], x[ 9], x[14]);
    }

    for (int i = 0; i < 16; i++) out[i] = x[i] + s[i];
}

void chacha20_xor(uint8_t* out, const uint8_t* in, uint64_t len,
                   const uint8_t key[CHACHA20_KEY_SZ],
                   const uint8_t nonce[CHACHA20_NONCE_SZ],
                   uint32_t initial_counter) {
    uint32_t ks[16];
    uint8_t  ks_bytes[CHACHA20_BLOCK_SZ];
    uint32_t counter = initial_counter;

    while (len) {
        chacha20_block(ks, key, nonce, counter++);
        for (int i = 0; i < 16; i++) put_le32(ks_bytes + 4 * i, ks[i]);
        uint32_t n = (len > CHACHA20_BLOCK_SZ) ? CHACHA20_BLOCK_SZ : (uint32_t)len;
        for (uint32_t i = 0; i < n; i++) out[i] = in[i] ^ ks_bytes[i];
        out += n; in += n; len -= n;
    }
}
