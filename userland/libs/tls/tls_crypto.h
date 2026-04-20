#pragma once
// ── MakaOS TLS crypto primitives (Phase 9) ──────────────────────────
//
// All primitives here are constant-time where operating on secrets.
// Hand-rolled, no assembly (we compile with -O2 -fno-strict-aliasing
// and get good codegen for these uniform loop bodies).
//
// Layout:
//   tls_crypto.h      — public API for every primitive
//   tls_hash.c        — SHA-256, SHA-384, HMAC, HKDF
//   tls_aead.c        — ChaCha20-Poly1305, AES-128/256-GCM
//   tls_pk.c          — X25519, P-256, Ed25519, ECDSA, RSA verify
//   tls_random.c      — CSPRNG (seeded from /dev/urandom + RDRAND)
//   tls_der.c         — ASN.1 DER parser
//   tls_x509.c        — X.509 v3 cert parser + name match
//   tls_trust.c       — CA bundle + chain validation
//   tls_record.c      — TLS record layer (AEAD wrap/unwrap)
//   tls_handshake.c   — TLS 1.3 + 1.2 handshake state machines
//   tls_client.c      — tls_connect + tls_read/write (socket bridge)
//   ca_bundle.c       — generated: embedded PEM → DER array

#include "libc.h"    // uint32_t etc via userland libc

// Hash output sizes.
#define TLS_SHA256_SZ  32
#define TLS_SHA384_SZ  48
#define TLS_SHA512_SZ  64

// ── SHA-256 (FIPS-180-4) ────────────────────────────────────────────
typedef struct {
    uint32_t h[8];
    uint64_t len;            // total bytes hashed
    uint8_t  buf[64];
    uint32_t buf_len;
} tls_sha256_t;

void tls_sha256_init  (tls_sha256_t* ctx);
void tls_sha256_update(tls_sha256_t* ctx, const void* data, uint64_t len);
void tls_sha256_final (tls_sha256_t* ctx, uint8_t out[TLS_SHA256_SZ]);
void tls_sha256       (const void* data, uint64_t len,
                        uint8_t out[TLS_SHA256_SZ]);

// ── SHA-384 (FIPS-180-4, uses SHA-512 internals) ────────────────────
typedef struct {
    uint64_t h[8];
    uint64_t len_lo, len_hi;
    uint8_t  buf[128];
    uint32_t buf_len;
} tls_sha384_t;

void tls_sha384_init  (tls_sha384_t* ctx);
void tls_sha384_update(tls_sha384_t* ctx, const void* data, uint64_t len);
void tls_sha384_final (tls_sha384_t* ctx, uint8_t out[TLS_SHA384_SZ]);
void tls_sha384       (const void* data, uint64_t len,
                        uint8_t out[TLS_SHA384_SZ]);

// ── HMAC (RFC 2104) — generic over SHA-256 / SHA-384 ────────────────
// One context per hash algorithm for type safety.
typedef struct {
    tls_sha256_t inner, outer;
    uint8_t      ipad_key[64];
} tls_hmac_sha256_t;

typedef struct {
    tls_sha384_t inner, outer;
    uint8_t      ipad_key[128];
} tls_hmac_sha384_t;

void tls_hmac_sha256_init  (tls_hmac_sha256_t* ctx,
                             const void* key, uint64_t klen);
void tls_hmac_sha256_update(tls_hmac_sha256_t* ctx,
                             const void* data, uint64_t len);
void tls_hmac_sha256_final (tls_hmac_sha256_t* ctx,
                             uint8_t out[TLS_SHA256_SZ]);
void tls_hmac_sha256       (const void* key,  uint64_t klen,
                             const void* data, uint64_t len,
                             uint8_t out[TLS_SHA256_SZ]);

void tls_hmac_sha384_init  (tls_hmac_sha384_t* ctx,
                             const void* key, uint64_t klen);
void tls_hmac_sha384_update(tls_hmac_sha384_t* ctx,
                             const void* data, uint64_t len);
void tls_hmac_sha384_final (tls_hmac_sha384_t* ctx,
                             uint8_t out[TLS_SHA384_SZ]);
void tls_hmac_sha384       (const void* key,  uint64_t klen,
                             const void* data, uint64_t len,
                             uint8_t out[TLS_SHA384_SZ]);

// ── HKDF (RFC 5869) — Extract + Expand + TLS 1.3 Expand-Label ───────
void tls_hkdf_extract_sha256(const void* salt, uint64_t salt_len,
                              const void* ikm,  uint64_t ikm_len,
                              uint8_t out[TLS_SHA256_SZ]);
void tls_hkdf_expand_sha256(const void* prk,  uint64_t prk_len,
                             const void* info, uint64_t info_len,
                             uint8_t*    out,  uint64_t out_len);
void tls_hkdf_extract_sha384(const void* salt, uint64_t salt_len,
                              const void* ikm,  uint64_t ikm_len,
                              uint8_t out[TLS_SHA384_SZ]);
void tls_hkdf_expand_sha384(const void* prk,  uint64_t prk_len,
                             const void* info, uint64_t info_len,
                             uint8_t*    out,  uint64_t out_len);

// TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1) — builds the structured
// label "tls13 <label>" + context and calls HKDF-Expand.
void tls13_hkdf_expand_label_sha256(const uint8_t  secret[TLS_SHA256_SZ],
                                      const char*    label,
                                      const void*    context,
                                      uint64_t       context_len,
                                      uint8_t*       out,
                                      uint64_t       out_len);
void tls13_hkdf_expand_label_sha384(const uint8_t  secret[TLS_SHA384_SZ],
                                      const char*    label,
                                      const void*    context,
                                      uint64_t       context_len,
                                      uint8_t*       out,
                                      uint64_t       out_len);

// TLS 1.3 Derive-Secret: HKDF-Expand-Label(secret, label, hash(context)).
void tls13_derive_secret_sha256(const uint8_t  secret[TLS_SHA256_SZ],
                                  const char*    label,
                                  const void*    ctx_msgs,
                                  uint64_t       ctx_len,
                                  uint8_t        out[TLS_SHA256_SZ]);
void tls13_derive_secret_sha384(const uint8_t  secret[TLS_SHA384_SZ],
                                  const char*    label,
                                  const void*    ctx_msgs,
                                  uint64_t       ctx_len,
                                  uint8_t        out[TLS_SHA384_SZ]);

// ── CSPRNG ─────────────────────────────────────────────────────────
// Seeds from /dev/urandom (once at first call) and mixes in RDRAND
// on each draw.  Not exposing an explicit seed API — kernel entropy
// is the root of trust.  Constant-time consumers (key gen) should
// use tls_random_bytes directly.
void tls_random_bytes(void* out, uint64_t len);

// Timing-safe byte compare — returns 0 if equal, nonzero otherwise.
// Branch-free; constant-time.
int tls_ct_memeq(const void* a, const void* b, uint64_t len);
