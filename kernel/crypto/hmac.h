#pragma once
// ── HMAC (RFC 2104) + HKDF (RFC 5869) over SHA-256 and SHA-384 ────

#include "sha256.h"
#include "sha384.h"

// ── HMAC-SHA256 ────────────────────────────────────────────────────
typedef struct {
    sha256_ctx_t inner;
    sha256_ctx_t outer;
} hmac_sha256_ctx_t;

void hmac_sha256_init  (hmac_sha256_ctx_t* ctx,
                         const void* key, uint64_t klen);
void hmac_sha256_update(hmac_sha256_ctx_t* ctx,
                         const void* data, uint64_t len);
void hmac_sha256_final (hmac_sha256_ctx_t* ctx,
                         uint8_t out[SHA256_DIGEST_SZ]);
void hmac_sha256       (const void* key,  uint64_t klen,
                         const void* data, uint64_t len,
                         uint8_t out[SHA256_DIGEST_SZ]);

// ── HMAC-SHA384 ────────────────────────────────────────────────────
typedef struct {
    sha384_ctx_t inner;
    sha384_ctx_t outer;
} hmac_sha384_ctx_t;

void hmac_sha384_init  (hmac_sha384_ctx_t* ctx,
                         const void* key, uint64_t klen);
void hmac_sha384_update(hmac_sha384_ctx_t* ctx,
                         const void* data, uint64_t len);
void hmac_sha384_final (hmac_sha384_ctx_t* ctx,
                         uint8_t out[SHA384_DIGEST_SZ]);
void hmac_sha384       (const void* key,  uint64_t klen,
                         const void* data, uint64_t len,
                         uint8_t out[SHA384_DIGEST_SZ]);

// ── HKDF (RFC 5869) ────────────────────────────────────────────────
// HKDF-Extract: salt + ikm → PRK (pseudo-random key)
// HKDF-Expand:  PRK  + info → OKM (output keying material, variable len)
void hkdf_sha256_extract(const void* salt, uint64_t salt_len,
                          const void* ikm,  uint64_t ikm_len,
                          uint8_t prk[SHA256_DIGEST_SZ]);
void hkdf_sha256_expand (const void* prk,  uint64_t prk_len,
                          const void* info, uint64_t info_len,
                          uint8_t*    out,  uint64_t out_len);

void hkdf_sha384_extract(const void* salt, uint64_t salt_len,
                          const void* ikm,  uint64_t ikm_len,
                          uint8_t prk[SHA384_DIGEST_SZ]);
void hkdf_sha384_expand (const void* prk,  uint64_t prk_len,
                          const void* info, uint64_t info_len,
                          uint8_t*    out,  uint64_t out_len);

// ── TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1) ──────────────────────
// HkdfLabel = {uint16 length; opaque label<7..255>="tls13 "+label;
//              opaque context<0..255>}
// HKDF-Expand-Label(Secret, Label, Context, Len) =
//    HKDF-Expand(Secret, HkdfLabel, Len)
void tls13_expand_label_sha256(const uint8_t secret[SHA256_DIGEST_SZ],
                                 const char*   label,
                                 const void*   ctx, uint64_t ctx_len,
                                 uint8_t*      out, uint64_t out_len);
void tls13_expand_label_sha384(const uint8_t secret[SHA384_DIGEST_SZ],
                                 const char*   label,
                                 const void*   ctx, uint64_t ctx_len,
                                 uint8_t*      out, uint64_t out_len);

// Derive-Secret(Secret, Label, Messages) =
//    HKDF-Expand-Label(Secret, Label, Transcript-Hash(Messages), Hash.len)
void tls13_derive_secret_sha256(const uint8_t secret[SHA256_DIGEST_SZ],
                                  const char*   label,
                                  const void*   ctx_msgs, uint64_t ctx_len,
                                  uint8_t out[SHA256_DIGEST_SZ]);
void tls13_derive_secret_sha384(const uint8_t secret[SHA384_DIGEST_SZ],
                                  const char*   label,
                                  const void*   ctx_msgs, uint64_t ctx_len,
                                  uint8_t out[SHA384_DIGEST_SZ]);
