#pragma once
// ── Poly1305 (RFC 8439 §2.5) — 128-bit MAC ─────────────────────────
// Portable C; 64×64→128-bit multiplications via __int128.

#ifdef __KERNEL__
#include "common.h"
#else
#include "libc.h"
#endif

#define POLY1305_KEY_SZ 32
#define POLY1305_TAG_SZ 16

typedef struct {
    uint64_t r[3];      // clamped r, 3 × 44-bit limbs
    uint64_t s[2];      // nonce/s, 2 × 64-bit limbs
    uint64_t h[3];      // accumulator, 3 × 44-bit limbs (extra bits for carry)
    uint8_t  buf[16];
    uint32_t buf_len;
} poly1305_ctx_t;

void poly1305_init  (poly1305_ctx_t* ctx,
                      const uint8_t key[POLY1305_KEY_SZ]);
void poly1305_update(poly1305_ctx_t* ctx, const void* data, uint64_t len);
void poly1305_final (poly1305_ctx_t* ctx, uint8_t tag[POLY1305_TAG_SZ]);

void poly1305       (const uint8_t key[POLY1305_KEY_SZ],
                     const void* data, uint64_t len,
                     uint8_t tag[POLY1305_TAG_SZ]);
