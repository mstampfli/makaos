#pragma once
// ── AES-128 / AES-256 (FIPS-197) ──────────────────────────────────
//
// NOT constant-time at the S-box-lookup level (standard table-based
// implementation).  Our AEAD layer prefers ChaCha20-Poly1305 which
// IS fully constant-time; AES-GCM is offered as interop-necessary
// but AES cache-timing is a known side channel.  A production
// deployment against untrusted peers should either use only
// ChaCha20-Poly1305 or supply bitsliced/NI-accelerated AES.

#ifdef __KERNEL__
#include "common.h"
#else
#include "libc.h"
#endif

typedef struct {
    uint32_t rk[60];     // expanded round-key schedule: 44 × u32 (AES-128)
                         //                              60 × u32 (AES-256)
    int      nrounds;    // 10 or 14
} aes_key_t;

// Initialise key schedule.
void aes_setkey_128(aes_key_t* k, const uint8_t key[16]);
void aes_setkey_256(aes_key_t* k, const uint8_t key[32]);

// Single-block encrypt (no decrypt — GCM only needs the encrypt direction).
void aes_encrypt(const aes_key_t* k,
                  const uint8_t  in [16],
                  uint8_t        out[16]);
