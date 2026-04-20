#pragma once
// ── AEAD constructions for TLS 1.2 / 1.3 ──────────────────────────
//
// - chacha20_poly1305_*  (RFC 8439)
// - aes128_gcm_*         (NIST SP 800-38D + FIPS-197)
// - aes256_gcm_*
//
// All three have the same interface:
//   seal:  key + nonce(12) + aad + plaintext → ciphertext ‖ tag(16)
//   open:  key + nonce(12) + aad + ciphertext(with tag) → plaintext OR failure
//
// Constant-time where operating on secrets.  Tag verification is
// always constant-time (tls_ct_memeq).

#ifdef __KERNEL__
#include "common.h"
#else
#include "libc.h"
#endif

#define AEAD_NONCE_SZ     12
#define AEAD_TAG_SZ       16
#define AEAD_CHACHA_KEY   32
#define AEAD_AES128_KEY   16
#define AEAD_AES256_KEY   32

// ── ChaCha20-Poly1305 (RFC 8439) ───────────────────────────────────
void chacha20_poly1305_seal(const uint8_t key[AEAD_CHACHA_KEY],
                              const uint8_t nonce[AEAD_NONCE_SZ],
                              const void*   aad, uint64_t aad_len,
                              const void*   pt,  uint64_t pt_len,
                              uint8_t*      ct,                      // pt_len bytes
                              uint8_t       tag[AEAD_TAG_SZ]);

// Returns 0 on success, -1 on tag mismatch (authentication failure).
int  chacha20_poly1305_open(const uint8_t key[AEAD_CHACHA_KEY],
                              const uint8_t nonce[AEAD_NONCE_SZ],
                              const void*   aad, uint64_t aad_len,
                              const void*   ct,  uint64_t ct_len,
                              const uint8_t tag[AEAD_TAG_SZ],
                              uint8_t*      pt);                     // ct_len bytes

// ── AES-128-GCM ────────────────────────────────────────────────────
void aes128_gcm_seal(const uint8_t key[AEAD_AES128_KEY],
                      const uint8_t nonce[AEAD_NONCE_SZ],
                      const void*   aad, uint64_t aad_len,
                      const void*   pt,  uint64_t pt_len,
                      uint8_t*      ct,
                      uint8_t       tag[AEAD_TAG_SZ]);
int  aes128_gcm_open(const uint8_t key[AEAD_AES128_KEY],
                      const uint8_t nonce[AEAD_NONCE_SZ],
                      const void*   aad, uint64_t aad_len,
                      const void*   ct,  uint64_t ct_len,
                      const uint8_t tag[AEAD_TAG_SZ],
                      uint8_t*      pt);

// ── AES-256-GCM ────────────────────────────────────────────────────
void aes256_gcm_seal(const uint8_t key[AEAD_AES256_KEY],
                      const uint8_t nonce[AEAD_NONCE_SZ],
                      const void*   aad, uint64_t aad_len,
                      const void*   pt,  uint64_t pt_len,
                      uint8_t*      ct,
                      uint8_t       tag[AEAD_TAG_SZ]);
int  aes256_gcm_open(const uint8_t key[AEAD_AES256_KEY],
                      const uint8_t nonce[AEAD_NONCE_SZ],
                      const void*   aad, uint64_t aad_len,
                      const void*   ct,  uint64_t ct_len,
                      const uint8_t tag[AEAD_TAG_SZ],
                      uint8_t*      pt);

// Constant-time 16-byte compare — returns 0 if equal, nonzero otherwise.
int ct_memeq16(const uint8_t a[16], const uint8_t b[16]);
