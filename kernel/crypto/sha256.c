// ── SHA-256 (FIPS-180-4) reference implementation ──────────────────
// Constant-time: data-independent control flow, no table lookups
// that index by secret bytes.  Uses the standard round function.

#include "sha256.h"

static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROR32(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)    (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)   (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)     (ROR32(x, 2)  ^ ROR32(x, 13) ^ ROR32(x, 22))
#define BSIG1(x)     (ROR32(x, 6)  ^ ROR32(x, 11) ^ ROR32(x, 25))
#define SSIG0(x)     (ROR32(x, 7)  ^ ROR32(x, 18) ^ ((x) >> 3))
#define SSIG1(x)     (ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t) v;
}

static void put_be64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

static void sha256_compress(sha256_ctx_t* ctx, const uint8_t block[64]) {
    uint32_t W[64];
    for (int t = 0; t < 16; t++) W[t] = be32(block + 4 * t);
    for (int t = 16; t < 64; t++)
        W[t] = SSIG1(W[t-2]) + W[t-7] + SSIG0(W[t-15]) + W[t-16];

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (int t = 0; t < 64; t++) {
        uint32_t T1 = h + BSIG1(e) + CH(e, f, g) + K[t] + W[t];
        uint32_t T2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void sha256_init(sha256_ctx_t* ctx) {
    ctx->h[0] = 0x6a09e667u; ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u; ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu; ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu; ctx->h[7] = 0x5be0cd19u;
    ctx->total_bytes = 0;
    ctx->buf_len     = 0;
}

void sha256_update(sha256_ctx_t* ctx, const void* data, uint64_t len) {
    const uint8_t* p = (const uint8_t*)data;
    ctx->total_bytes += len;

    // Finish any partial block.
    if (ctx->buf_len) {
        uint32_t want = SHA256_BLOCK_SZ - ctx->buf_len;
        uint32_t take = (len < want) ? (uint32_t)len : want;
        for (uint32_t i = 0; i < take; i++) ctx->buf[ctx->buf_len + i] = p[i];
        ctx->buf_len += take;
        p += take; len -= take;
        if (ctx->buf_len == SHA256_BLOCK_SZ) {
            sha256_compress(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }

    // Process whole blocks directly from input.
    while (len >= SHA256_BLOCK_SZ) {
        sha256_compress(ctx, p);
        p += SHA256_BLOCK_SZ; len -= SHA256_BLOCK_SZ;
    }

    // Stash tail.
    if (len) {
        for (uint32_t i = 0; i < len; i++) ctx->buf[i] = p[i];
        ctx->buf_len = (uint32_t)len;
    }
}

void sha256_final(sha256_ctx_t* ctx, uint8_t out[SHA256_DIGEST_SZ]) {
    uint64_t bit_len = ctx->total_bytes * 8;

    // Append 0x80, pad to 56 mod 64, append 64-bit big-endian length.
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        while (ctx->buf_len < SHA256_BLOCK_SZ) ctx->buf[ctx->buf_len++] = 0;
        sha256_compress(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56) ctx->buf[ctx->buf_len++] = 0;
    put_be64(ctx->buf + 56, bit_len);
    sha256_compress(ctx, ctx->buf);

    for (int i = 0; i < 8; i++) put_be32(out + 4 * i, ctx->h[i]);
}

void sha256(const void* data, uint64_t len, uint8_t out[SHA256_DIGEST_SZ]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}
