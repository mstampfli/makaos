// ── Poly1305 (RFC 8439) — 5×26-bit limb representation ────────────
//
// Uses 5 × 26-bit limbs for h and r (130 bits total = 5 × 26).
// 2^130 ≡ 5 (mod p), so the factor s_i = 5 * r_i cleanly folds
// terms at position 130+ back into low limbs without subtle offset
// bugs.
//
// Each limb fits in a 32-bit variable; products are 52-bit, sum of
// 5 products fits in ~55 bits (uint64_t with room to spare).
// Constant-time: no data-dependent branches.

#include "poly1305.h"

// (Re-declare the context — keeping the public struct from the
// header, we use different internals.  The 3×44 state array is
// reused here as 5×32 via casts; instead, just replace the struct.)

// Private representation (hidden behind public poly1305_ctx_t layout).
// We use the `r[3]`, `s[2]`, `h[3]`, `buf[16]`, `buf_len` fields
// repurposed: r[0..2] ← 5 × 26-bit r, s[0..1] ← pad s, h[0..2] ← 5 × 26-bit h.
// The public struct has enough storage (3*8 + 2*8 + 3*8 = 64 bytes
// + buf + buf_len).  We interpret r[] as (r[0]>>0..25, r[0]>>26..51,
// ...) — a bit grim, but avoids ABI changes.
//
// Actually simpler: use a local struct and cast.  The public
// poly1305_ctx_t is opaque other than size.

typedef struct {
    uint32_t r[5];     // 5 × 26-bit, r clamped
    uint32_t s[4];     // pad s as 4 × 32-bit little-endian words
    uint32_t h[5];     // accumulator, 5 × ~26-bit
    uint8_t  buf[16];
    uint32_t buf_len;
} p5_ctx_t;

_Static_assert(sizeof(p5_ctx_t) <= sizeof(poly1305_ctx_t),
               "private p5_ctx_t must fit in public poly1305_ctx_t");

static uint32_t le32_(const uint8_t* p) {
    return  (uint32_t)p[0]        | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void poly1305_init(poly1305_ctx_t* pub, const uint8_t key[POLY1305_KEY_SZ]) {
    p5_ctx_t* c = (p5_ctx_t*)pub;
    // Parse r, apply clamp per RFC 8439 §2.5.  Clamp mask in little-
    // endian byte order: ff ff ff 0f | fc ff ff 0f | fc ff ff 0f | fc ff ff 0f
    uint32_t t0 = le32_(key +  0);
    uint32_t t1 = le32_(key +  4);
    uint32_t t2 = le32_(key +  8);
    uint32_t t3 = le32_(key + 12);
    // Clamp: high nibble of each 32-bit word → 0x0f; low 2 bits of
    // words 1,2,3 → 00.
    t0 &= 0x0fffffff;
    t1 &= 0x0ffffffc;
    t2 &= 0x0ffffffc;
    t3 &= 0x0ffffffc;
    // Split clamped r (128 bits) into 5 × 26-bit limbs.
    c->r[0] =  t0                        & 0x3ffffff;
    c->r[1] = ((t0 >> 26) | (t1 <<  6))  & 0x3ffffff;
    c->r[2] = ((t1 >> 20) | (t2 << 12))  & 0x3ffffff;
    c->r[3] = ((t2 >> 14) | (t3 << 18))  & 0x3ffffff;
    c->r[4] =  (t3 >>  8);                // ≤ 24 bits

    // s = key[16..31] as 4 × 32-bit LE.
    c->s[0] = le32_(key + 16);
    c->s[1] = le32_(key + 20);
    c->s[2] = le32_(key + 24);
    c->s[3] = le32_(key + 28);

    for (int i = 0; i < 5; i++) c->h[i] = 0;
    c->buf_len = 0;
}

static void poly1305_block(p5_ctx_t* c, const uint8_t block[16],
                             uint32_t hibit) {
    // Parse block as 4 × 32-bit LE, then split into 5 × 26-bit limbs
    // plus the `hibit` (1 for full block, 0 for final partial).
    uint32_t t0 = le32_(block +  0);
    uint32_t t1 = le32_(block +  4);
    uint32_t t2 = le32_(block +  8);
    uint32_t t3 = le32_(block + 12);

    uint32_t m0 =  t0                        & 0x3ffffff;
    uint32_t m1 = ((t0 >> 26) | (t1 <<  6))  & 0x3ffffff;
    uint32_t m2 = ((t1 >> 20) | (t2 << 12))  & 0x3ffffff;
    uint32_t m3 = ((t2 >> 14) | (t3 << 18))  & 0x3ffffff;
    uint32_t m4 =  (t3 >>  8)                | hibit;

    // h += m
    uint32_t h0 = c->h[0] + m0;
    uint32_t h1 = c->h[1] + m1;
    uint32_t h2 = c->h[2] + m2;
    uint32_t h3 = c->h[3] + m3;
    uint32_t h4 = c->h[4] + m4;

    // h *= r, with reduction via s_i = 5 * r_i (for i=1..4).
    uint32_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], r4 = c->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

    uint64_t d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3
                + (uint64_t)h3*s2 + (uint64_t)h4*s1;
    uint64_t d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4
                + (uint64_t)h3*s3 + (uint64_t)h4*s2;
    uint64_t d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0
                + (uint64_t)h3*s4 + (uint64_t)h4*s3;
    uint64_t d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1
                + (uint64_t)h3*r0 + (uint64_t)h4*s4;
    uint64_t d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2
                + (uint64_t)h3*r1 + (uint64_t)h4*r0;

    // Partial carry propagation.
    uint64_t carry;
    carry = d0 >> 26; d0 &= 0x3ffffff; d1 += carry;
    carry = d1 >> 26; d1 &= 0x3ffffff; d2 += carry;
    carry = d2 >> 26; d2 &= 0x3ffffff; d3 += carry;
    carry = d3 >> 26; d3 &= 0x3ffffff; d4 += carry;
    carry = d4 >> 26; d4 &= 0x3ffffff;
    // Reduce: carry * 5 into d0, then one more carry from d0 to d1.
    d0 += carry * 5;
    carry = d0 >> 26; d0 &= 0x3ffffff; d1 += carry;

    c->h[0] = (uint32_t)d0; c->h[1] = (uint32_t)d1;
    c->h[2] = (uint32_t)d2; c->h[3] = (uint32_t)d3;
    c->h[4] = (uint32_t)d4;
}

void poly1305_update(poly1305_ctx_t* pub, const void* data, uint64_t len) {
    p5_ctx_t* c = (p5_ctx_t*)pub;
    const uint8_t* p = (const uint8_t*)data;

    if (c->buf_len) {
        uint32_t want = 16 - c->buf_len;
        uint32_t take = (len < want) ? (uint32_t)len : want;
        for (uint32_t i = 0; i < take; i++) c->buf[c->buf_len + i] = p[i];
        c->buf_len += take;
        p += take; len -= take;
        if (c->buf_len == 16) {
            poly1305_block(c, c->buf, 1u << 24);   // bit 128 of 130-bit = bit 24 of limb 4
            c->buf_len = 0;
        }
    }
    while (len >= 16) {
        poly1305_block(c, p, 1u << 24);
        p += 16; len -= 16;
    }
    if (len) {
        for (uint32_t i = 0; i < len; i++) c->buf[i] = p[i];
        c->buf_len = (uint32_t)len;
    }
}

void poly1305_final(poly1305_ctx_t* pub, uint8_t tag[POLY1305_TAG_SZ]) {
    p5_ctx_t* c = (p5_ctx_t*)pub;

    if (c->buf_len) {
        c->buf[c->buf_len++] = 0x01;
        while (c->buf_len < 16) c->buf[c->buf_len++] = 0;
        poly1305_block(c, c->buf, 0);
    }

    uint32_t h0 = c->h[0], h1 = c->h[1], h2 = c->h[2], h3 = c->h[3], h4 = c->h[4];

    // Full carry propagation.
    uint32_t carry;
    carry = h1 >> 26; h1 &= 0x3ffffff; h2 += carry;
    carry = h2 >> 26; h2 &= 0x3ffffff; h3 += carry;
    carry = h3 >> 26; h3 &= 0x3ffffff; h4 += carry;
    carry = h4 >> 26; h4 &= 0x3ffffff; h0 += carry * 5;
    carry = h0 >> 26; h0 &= 0x3ffffff; h1 += carry;

    // Compute h + -(2^130 - 5) = h + 5 - 2^130.  If no borrow, h ≥ p → use result.
    uint32_t g0 = h0 + 5;  carry = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + carry;  carry = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + carry;  carry = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + carry;  carry = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + carry - (1u << 26);    // subtract 2^130 (= bit 26 of limb 4)

    // Constant-time select: if top bit of g4 is set (borrow, h < p), use h.
    // Otherwise use g.
    uint32_t mask = (g4 >> 31) - 1;   // 0xFF... if NO borrow (pick g), 0 if borrow (pick h)
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);

    // Flatten 5×26 → 4×32.
    uint32_t f0 = h0        | (h1 << 26);
    uint32_t f1 = (h1 >> 6) | (h2 << 20);
    uint32_t f2 = (h2 >> 12)| (h3 << 14);
    uint32_t f3 = (h3 >> 18)| (h4 <<  8);

    // Add s (the pad, as 128-bit little-endian).
    uint64_t sum0 = (uint64_t)f0 + c->s[0];
    uint32_t o0 = (uint32_t)sum0;
    uint64_t cy  = sum0 >> 32;
    uint64_t sum1 = (uint64_t)f1 + c->s[1] + cy;
    uint32_t o1 = (uint32_t)sum1; cy = sum1 >> 32;
    uint64_t sum2 = (uint64_t)f2 + c->s[2] + cy;
    uint32_t o2 = (uint32_t)sum2; cy = sum2 >> 32;
    uint64_t sum3 = (uint64_t)f3 + c->s[3] + cy;
    uint32_t o3 = (uint32_t)sum3;

    // Write tag little-endian.
    tag[ 0] = (uint8_t)o0;        tag[ 1] = (uint8_t)(o0 >> 8);
    tag[ 2] = (uint8_t)(o0 >> 16); tag[ 3] = (uint8_t)(o0 >> 24);
    tag[ 4] = (uint8_t)o1;        tag[ 5] = (uint8_t)(o1 >> 8);
    tag[ 6] = (uint8_t)(o1 >> 16); tag[ 7] = (uint8_t)(o1 >> 24);
    tag[ 8] = (uint8_t)o2;        tag[ 9] = (uint8_t)(o2 >> 8);
    tag[10] = (uint8_t)(o2 >> 16); tag[11] = (uint8_t)(o2 >> 24);
    tag[12] = (uint8_t)o3;        tag[13] = (uint8_t)(o3 >> 8);
    tag[14] = (uint8_t)(o3 >> 16); tag[15] = (uint8_t)(o3 >> 24);
}

void poly1305(const uint8_t key[POLY1305_KEY_SZ],
              const void* data, uint64_t len,
              uint8_t tag[POLY1305_TAG_SZ]) {
    poly1305_ctx_t ctx;
    poly1305_init(&ctx, key);
    poly1305_update(&ctx, data, len);
    poly1305_final(&ctx, tag);
}
