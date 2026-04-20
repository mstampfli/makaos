// ── HMAC + HKDF + TLS 1.3 Expand-Label / Derive-Secret ───────────

#include "hmac.h"

// ── HMAC-SHA256 ────────────────────────────────────────────────────

void hmac_sha256_init(hmac_sha256_ctx_t* ctx,
                       const void* key, uint64_t klen) {
    uint8_t k0[SHA256_BLOCK_SZ];
    if (klen > SHA256_BLOCK_SZ) {
        sha256(key, klen, k0);
        for (int i = SHA256_DIGEST_SZ; i < SHA256_BLOCK_SZ; i++) k0[i] = 0;
    } else {
        for (uint64_t i = 0; i < klen; i++) k0[i] = ((const uint8_t*)key)[i];
        for (uint64_t i = klen; i < SHA256_BLOCK_SZ; i++) k0[i] = 0;
    }

    uint8_t ipad[SHA256_BLOCK_SZ], opad[SHA256_BLOCK_SZ];
    for (int i = 0; i < SHA256_BLOCK_SZ; i++) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }
    sha256_init(&ctx->inner); sha256_update(&ctx->inner, ipad, SHA256_BLOCK_SZ);
    sha256_init(&ctx->outer); sha256_update(&ctx->outer, opad, SHA256_BLOCK_SZ);
}

void hmac_sha256_update(hmac_sha256_ctx_t* ctx, const void* data, uint64_t len) {
    sha256_update(&ctx->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx_t* ctx, uint8_t out[SHA256_DIGEST_SZ]) {
    uint8_t inner_digest[SHA256_DIGEST_SZ];
    sha256_final(&ctx->inner, inner_digest);
    sha256_update(&ctx->outer, inner_digest, SHA256_DIGEST_SZ);
    sha256_final(&ctx->outer, out);
}

void hmac_sha256(const void* key,  uint64_t klen,
                  const void* data, uint64_t len,
                  uint8_t out[SHA256_DIGEST_SZ]) {
    hmac_sha256_ctx_t ctx;
    hmac_sha256_init(&ctx, key, klen);
    hmac_sha256_update(&ctx, data, len);
    hmac_sha256_final(&ctx, out);
}

// ── HMAC-SHA384 ────────────────────────────────────────────────────

void hmac_sha384_init(hmac_sha384_ctx_t* ctx,
                       const void* key, uint64_t klen) {
    uint8_t k0[SHA384_BLOCK_SZ];
    if (klen > SHA384_BLOCK_SZ) {
        sha384(key, klen, k0);
        for (int i = SHA384_DIGEST_SZ; i < SHA384_BLOCK_SZ; i++) k0[i] = 0;
    } else {
        for (uint64_t i = 0; i < klen; i++) k0[i] = ((const uint8_t*)key)[i];
        for (uint64_t i = klen; i < SHA384_BLOCK_SZ; i++) k0[i] = 0;
    }

    uint8_t ipad[SHA384_BLOCK_SZ], opad[SHA384_BLOCK_SZ];
    for (int i = 0; i < SHA384_BLOCK_SZ; i++) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }
    sha384_init(&ctx->inner); sha384_update(&ctx->inner, ipad, SHA384_BLOCK_SZ);
    sha384_init(&ctx->outer); sha384_update(&ctx->outer, opad, SHA384_BLOCK_SZ);
}

void hmac_sha384_update(hmac_sha384_ctx_t* ctx, const void* data, uint64_t len) {
    sha384_update(&ctx->inner, data, len);
}

void hmac_sha384_final(hmac_sha384_ctx_t* ctx, uint8_t out[SHA384_DIGEST_SZ]) {
    uint8_t inner_digest[SHA384_DIGEST_SZ];
    sha384_final(&ctx->inner, inner_digest);
    sha384_update(&ctx->outer, inner_digest, SHA384_DIGEST_SZ);
    sha384_final(&ctx->outer, out);
}

void hmac_sha384(const void* key,  uint64_t klen,
                  const void* data, uint64_t len,
                  uint8_t out[SHA384_DIGEST_SZ]) {
    hmac_sha384_ctx_t ctx;
    hmac_sha384_init(&ctx, key, klen);
    hmac_sha384_update(&ctx, data, len);
    hmac_sha384_final(&ctx, out);
}

// ── HKDF (RFC 5869) ────────────────────────────────────────────────

void hkdf_sha256_extract(const void* salt, uint64_t salt_len,
                          const void* ikm,  uint64_t ikm_len,
                          uint8_t prk[SHA256_DIGEST_SZ]) {
    // If salt is NULL / 0-len, use a string of zeros of hash length.
    uint8_t zero_salt[SHA256_DIGEST_SZ];
    if (!salt || salt_len == 0) {
        for (int i = 0; i < SHA256_DIGEST_SZ; i++) zero_salt[i] = 0;
        salt = zero_salt; salt_len = SHA256_DIGEST_SZ;
    }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

void hkdf_sha256_expand(const void* prk,  uint64_t prk_len,
                         const void* info, uint64_t info_len,
                         uint8_t*    out,  uint64_t out_len) {
    uint8_t T[SHA256_DIGEST_SZ];
    uint64_t n = (out_len + SHA256_DIGEST_SZ - 1) / SHA256_DIGEST_SZ;
    uint64_t off = 0;
    uint8_t ctr = 1;

    for (uint64_t i = 0; i < n; i++) {
        hmac_sha256_ctx_t hctx;
        hmac_sha256_init(&hctx, prk, prk_len);
        if (i > 0) hmac_sha256_update(&hctx, T, SHA256_DIGEST_SZ);
        if (info_len) hmac_sha256_update(&hctx, info, info_len);
        hmac_sha256_update(&hctx, &ctr, 1);
        hmac_sha256_final(&hctx, T);
        ctr++;

        uint64_t take = out_len - off;
        if (take > SHA256_DIGEST_SZ) take = SHA256_DIGEST_SZ;
        for (uint64_t j = 0; j < take; j++) out[off + j] = T[j];
        off += take;
    }
}

void hkdf_sha384_extract(const void* salt, uint64_t salt_len,
                          const void* ikm,  uint64_t ikm_len,
                          uint8_t prk[SHA384_DIGEST_SZ]) {
    uint8_t zero_salt[SHA384_DIGEST_SZ];
    if (!salt || salt_len == 0) {
        for (int i = 0; i < SHA384_DIGEST_SZ; i++) zero_salt[i] = 0;
        salt = zero_salt; salt_len = SHA384_DIGEST_SZ;
    }
    hmac_sha384(salt, salt_len, ikm, ikm_len, prk);
}

void hkdf_sha384_expand(const void* prk,  uint64_t prk_len,
                         const void* info, uint64_t info_len,
                         uint8_t*    out,  uint64_t out_len) {
    uint8_t T[SHA384_DIGEST_SZ];
    uint64_t n = (out_len + SHA384_DIGEST_SZ - 1) / SHA384_DIGEST_SZ;
    uint64_t off = 0;
    uint8_t ctr = 1;

    for (uint64_t i = 0; i < n; i++) {
        hmac_sha384_ctx_t hctx;
        hmac_sha384_init(&hctx, prk, prk_len);
        if (i > 0) hmac_sha384_update(&hctx, T, SHA384_DIGEST_SZ);
        if (info_len) hmac_sha384_update(&hctx, info, info_len);
        hmac_sha384_update(&hctx, &ctr, 1);
        hmac_sha384_final(&hctx, T);
        ctr++;

        uint64_t take = out_len - off;
        if (take > SHA384_DIGEST_SZ) take = SHA384_DIGEST_SZ;
        for (uint64_t j = 0; j < take; j++) out[off + j] = T[j];
        off += take;
    }
}

// ── TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1) ─────────────────────
// HkdfLabel = struct {
//     uint16 length = out_len;
//     opaque label<7..255>   = "tls13 " || label;
//     opaque context<0..255> = ctx;
// }

static uint64_t build_hkdf_label(uint8_t* buf, uint16_t out_len,
                                  const char* label,
                                  const void* ctx, uint64_t ctx_len) {
    // length (uint16 BE)
    buf[0] = (uint8_t)(out_len >> 8);
    buf[1] = (uint8_t) out_len;
    // label (prefixed with 1 byte length)
    const char* pfx = "tls13 ";
    uint64_t pfx_len = 6;
    uint64_t lbl_len = 0;
    while (label[lbl_len]) lbl_len++;
    uint64_t total_label = pfx_len + lbl_len;
    buf[2] = (uint8_t)total_label;
    for (uint64_t i = 0; i < pfx_len; i++) buf[3 + i] = (uint8_t)pfx[i];
    for (uint64_t i = 0; i < lbl_len; i++) buf[3 + pfx_len + i] = (uint8_t)label[i];
    uint64_t off = 3 + total_label;
    // context (prefixed with 1 byte length)
    buf[off++] = (uint8_t)ctx_len;
    for (uint64_t i = 0; i < ctx_len; i++) buf[off++] = ((const uint8_t*)ctx)[i];
    return off;
}

void tls13_expand_label_sha256(const uint8_t secret[SHA256_DIGEST_SZ],
                                 const char*   label,
                                 const void*   ctx, uint64_t ctx_len,
                                 uint8_t*      out, uint64_t out_len) {
    uint8_t hkdf_label[512];  // plenty — spec caps at ~520 bytes total
    uint64_t hl = build_hkdf_label(hkdf_label, (uint16_t)out_len,
                                     label, ctx, ctx_len);
    hkdf_sha256_expand(secret, SHA256_DIGEST_SZ, hkdf_label, hl, out, out_len);
}

void tls13_expand_label_sha384(const uint8_t secret[SHA384_DIGEST_SZ],
                                 const char*   label,
                                 const void*   ctx, uint64_t ctx_len,
                                 uint8_t*      out, uint64_t out_len) {
    uint8_t hkdf_label[512];
    uint64_t hl = build_hkdf_label(hkdf_label, (uint16_t)out_len,
                                     label, ctx, ctx_len);
    hkdf_sha384_expand(secret, SHA384_DIGEST_SZ, hkdf_label, hl, out, out_len);
}

void tls13_derive_secret_sha256(const uint8_t secret[SHA256_DIGEST_SZ],
                                  const char*   label,
                                  const void*   ctx_msgs, uint64_t ctx_len,
                                  uint8_t out[SHA256_DIGEST_SZ]) {
    uint8_t transcript[SHA256_DIGEST_SZ];
    sha256(ctx_msgs, ctx_len, transcript);
    tls13_expand_label_sha256(secret, label, transcript,
                                SHA256_DIGEST_SZ, out, SHA256_DIGEST_SZ);
}

void tls13_derive_secret_sha384(const uint8_t secret[SHA384_DIGEST_SZ],
                                  const char*   label,
                                  const void*   ctx_msgs, uint64_t ctx_len,
                                  uint8_t out[SHA384_DIGEST_SZ]) {
    uint8_t transcript[SHA384_DIGEST_SZ];
    sha384(ctx_msgs, ctx_len, transcript);
    tls13_expand_label_sha384(secret, label, transcript,
                                SHA384_DIGEST_SZ, out, SHA384_DIGEST_SZ);
}
