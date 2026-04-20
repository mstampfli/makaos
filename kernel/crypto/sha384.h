#pragma once
// ── SHA-384 (FIPS-180-4, based on SHA-512 internals) ───────────────
// Portable C, context-free (kernel + userland builds).

#ifdef __KERNEL__
#include "common.h"
#else
#include "libc.h"
#endif

#define SHA384_DIGEST_SZ  48
#define SHA384_BLOCK_SZ   128

typedef struct {
    uint64_t h[8];
    uint64_t total_lo, total_hi;    // 128-bit message length
    uint8_t  buf[SHA384_BLOCK_SZ];
    uint32_t buf_len;
} sha384_ctx_t;

void sha384_init  (sha384_ctx_t* ctx);
void sha384_update(sha384_ctx_t* ctx, const void* data, uint64_t len);
void sha384_final (sha384_ctx_t* ctx, uint8_t out[SHA384_DIGEST_SZ]);
void sha384       (const void* data, uint64_t len,
                    uint8_t out[SHA384_DIGEST_SZ]);
