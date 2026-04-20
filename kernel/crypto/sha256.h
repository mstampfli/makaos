#pragma once
// ── SHA-256 (FIPS-180-4) — portable C, context-free ────────────────
//
// Compiled into both kernel (for /dev/urandom entropy mixing) and
// userland TLS (for handshake transcript hash, HMAC, HKDF).  No
// kernel or libc dependencies — only uint32_t/uint64_t/memcpy are
// needed, provided by <stdint.h>-equivalent typedefs in whichever
// env it lands in.

#ifdef __KERNEL__
#include "common.h"         // uint32_t + uint64_t
#else
#include "libc.h"           // user build brings them via libc
#endif

#define SHA256_DIGEST_SZ  32
#define SHA256_BLOCK_SZ   64

typedef struct {
    uint32_t h[8];
    uint64_t total_bytes;   // total input length
    uint8_t  buf[SHA256_BLOCK_SZ];
    uint32_t buf_len;       // bytes in partial block
} sha256_ctx_t;

void sha256_init  (sha256_ctx_t* ctx);
void sha256_update(sha256_ctx_t* ctx, const void* data, uint64_t len);
void sha256_final (sha256_ctx_t* ctx, uint8_t out[SHA256_DIGEST_SZ]);

// One-shot convenience.
void sha256       (const void* data, uint64_t len,
                   uint8_t out[SHA256_DIGEST_SZ]);
