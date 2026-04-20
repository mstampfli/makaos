// ── SHA-384 (FIPS-180-4) — uses SHA-512 round function ────────────

#include "sha384.h"

static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

#define ROR64(x, n)   (((x) >> (n)) | ((x) << (64 - (n))))
#define CH(x,y,z)     (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)    (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)      (ROR64(x, 28) ^ ROR64(x, 34) ^ ROR64(x, 39))
#define BSIG1(x)      (ROR64(x, 14) ^ ROR64(x, 18) ^ ROR64(x, 41))
#define SSIG0(x)      (ROR64(x,  1) ^ ROR64(x,  8) ^ ((x) >> 7))
#define SSIG1(x)      (ROR64(x, 19) ^ ROR64(x, 61) ^ ((x) >> 6))

static uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void put_be64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

static void sha384_compress(sha384_ctx_t* ctx, const uint8_t block[128]) {
    uint64_t W[80];
    for (int t = 0; t < 16; t++) W[t] = be64(block + 8 * t);
    for (int t = 16; t < 80; t++)
        W[t] = SSIG1(W[t-2]) + W[t-7] + SSIG0(W[t-15]) + W[t-16];

    uint64_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint64_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (int t = 0; t < 80; t++) {
        uint64_t T1 = h + BSIG1(e) + CH(e, f, g) + K[t] + W[t];
        uint64_t T2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void sha384_init(sha384_ctx_t* ctx) {
    // SHA-384 initial hash values (differ from SHA-512).
    ctx->h[0] = 0xcbbb9d5dc1059ed8ULL; ctx->h[1] = 0x629a292a367cd507ULL;
    ctx->h[2] = 0x9159015a3070dd17ULL; ctx->h[3] = 0x152fecd8f70e5939ULL;
    ctx->h[4] = 0x67332667ffc00b31ULL; ctx->h[5] = 0x8eb44a8768581511ULL;
    ctx->h[6] = 0xdb0c2e0d64f98fa7ULL; ctx->h[7] = 0x47b5481dbefa4fa4ULL;
    ctx->total_lo = 0; ctx->total_hi = 0;
    ctx->buf_len  = 0;
}

void sha384_update(sha384_ctx_t* ctx, const void* data, uint64_t len) {
    const uint8_t* p = (const uint8_t*)data;
    // 128-bit length accumulation.
    uint64_t old_lo = ctx->total_lo;
    ctx->total_lo += len;
    if (ctx->total_lo < old_lo) ctx->total_hi++;

    if (ctx->buf_len) {
        uint32_t want = SHA384_BLOCK_SZ - ctx->buf_len;
        uint32_t take = (len < want) ? (uint32_t)len : want;
        for (uint32_t i = 0; i < take; i++) ctx->buf[ctx->buf_len + i] = p[i];
        ctx->buf_len += take;
        p += take; len -= take;
        if (ctx->buf_len == SHA384_BLOCK_SZ) {
            sha384_compress(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
    while (len >= SHA384_BLOCK_SZ) {
        sha384_compress(ctx, p);
        p += SHA384_BLOCK_SZ; len -= SHA384_BLOCK_SZ;
    }
    if (len) {
        for (uint32_t i = 0; i < len; i++) ctx->buf[i] = p[i];
        ctx->buf_len = (uint32_t)len;
    }
}

void sha384_final(sha384_ctx_t* ctx, uint8_t out[SHA384_DIGEST_SZ]) {
    // Bit length: lo*8 + overflow to hi.
    uint64_t bit_lo = ctx->total_lo << 3;
    uint64_t bit_hi = (ctx->total_hi << 3) | (ctx->total_lo >> 61);

    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 112) {
        while (ctx->buf_len < SHA384_BLOCK_SZ) ctx->buf[ctx->buf_len++] = 0;
        sha384_compress(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 112) ctx->buf[ctx->buf_len++] = 0;
    put_be64(ctx->buf + 112, bit_hi);
    put_be64(ctx->buf + 120, bit_lo);
    sha384_compress(ctx, ctx->buf);

    // SHA-384 output is the first 6 × 64 bits.
    for (int i = 0; i < 6; i++) put_be64(out + 8 * i, ctx->h[i]);
}

void sha384(const void* data, uint64_t len, uint8_t out[SHA384_DIGEST_SZ]) {
    sha384_ctx_t ctx;
    sha384_init(&ctx);
    sha384_update(&ctx, data, len);
    sha384_final(&ctx, out);
}
