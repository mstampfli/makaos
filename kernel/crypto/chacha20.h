#pragma once
// ── ChaCha20 (RFC 8439) — portable C, context-free ─────────────────
//
// Used in two roles in MakaOS:
//   1. TLS AEAD: paired with Poly1305 (RFC 8439 §2.8)
//   2. Kernel CSPRNG output stage: ChaCha20 in CTR mode stretches
//      a hash-derived seed into an unbounded pseudo-random stream.

#ifdef __KERNEL__
#include "common.h"
#else
#include "libc.h"
#endif

#define CHACHA20_KEY_SZ   32
#define CHACHA20_NONCE_SZ 12
#define CHACHA20_BLOCK_SZ 64

// Low-level: compute one 64-byte keystream block for (key, nonce, counter).
// Constant-time: 20 rounds of fixed ops, no data-dependent branches.
void chacha20_block(uint32_t state_out[16],
                     const uint8_t key[CHACHA20_KEY_SZ],
                     const uint8_t nonce[CHACHA20_NONCE_SZ],
                     uint32_t counter);

// Encrypt/decrypt (symmetric — same op).  `out` may alias `in`.
// Counter starts at `initial_counter` (typically 0 or 1 per RFC).
void chacha20_xor(uint8_t* out, const uint8_t* in, uint64_t len,
                   const uint8_t key[CHACHA20_KEY_SZ],
                   const uint8_t nonce[CHACHA20_NONCE_SZ],
                   uint32_t initial_counter);
