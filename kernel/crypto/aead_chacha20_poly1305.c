// ── ChaCha20-Poly1305 AEAD (RFC 8439 §2.8) ────────────────────────
//
// Construction:
//   one_time_key = first 32 bytes of chacha20_block(key, nonce, 0)
//   ciphertext   = chacha20_xor(pt, key, nonce, counter=1)
//   mac_data     = aad ‖ pad16 ‖ ct ‖ pad16 ‖ u64(aad_len) ‖ u64(ct_len)
//   tag          = poly1305(one_time_key, mac_data)

#include "aead.h"
#include "chacha20.h"
#include "poly1305.h"

int ct_memeq16(const uint8_t a[16], const uint8_t b[16]) {
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return (int)diff;   // 0 if equal
}

static void put_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void derive_otk(uint8_t otk[32],
                        const uint8_t key[32], const uint8_t nonce[12]) {
    uint32_t blk[16];
    chacha20_block(blk, key, nonce, 0);
    // Serialise blk as little-endian; take first 32 bytes.
    for (int i = 0; i < 8; i++) {
        uint32_t w = blk[i];
        otk[4*i    ] = (uint8_t) w;
        otk[4*i + 1] = (uint8_t)(w >> 8);
        otk[4*i + 2] = (uint8_t)(w >> 16);
        otk[4*i + 3] = (uint8_t)(w >> 24);
    }
}

static void poly_update_with_pad16(poly1305_ctx_t* p,
                                     const void* data, uint64_t len) {
    poly1305_update(p, data, len);
    uint64_t rem = len & 15;
    if (rem) {
        static const uint8_t zeros[16] = {0};
        poly1305_update(p, zeros, 16 - rem);
    }
}

void chacha20_poly1305_seal(const uint8_t key[AEAD_CHACHA_KEY],
                              const uint8_t nonce[AEAD_NONCE_SZ],
                              const void*   aad, uint64_t aad_len,
                              const void*   pt,  uint64_t pt_len,
                              uint8_t*      ct,
                              uint8_t       tag[AEAD_TAG_SZ]) {
    uint8_t otk[32];
    derive_otk(otk, key, nonce);

    // Encrypt.  Counter starts at 1 (0 was used for OTK derivation).
    chacha20_xor(ct, (const uint8_t*)pt, pt_len, key, nonce, 1);

    // MAC the AAD and ciphertext with Poly1305.
    poly1305_ctx_t pctx;
    poly1305_init(&pctx, otk);
    poly_update_with_pad16(&pctx, aad, aad_len);
    poly_update_with_pad16(&pctx, ct,  pt_len);
    uint8_t lengths[16];
    put_le64(lengths + 0, aad_len);
    put_le64(lengths + 8, pt_len);
    poly1305_update(&pctx, lengths, 16);
    poly1305_final(&pctx, tag);

    // Wipe OTK.
    for (int i = 0; i < 32; i++) otk[i] = 0;
}

int chacha20_poly1305_open(const uint8_t key[AEAD_CHACHA_KEY],
                             const uint8_t nonce[AEAD_NONCE_SZ],
                             const void*   aad, uint64_t aad_len,
                             const void*   ct,  uint64_t ct_len,
                             const uint8_t tag[AEAD_TAG_SZ],
                             uint8_t*      pt) {
    uint8_t otk[32];
    derive_otk(otk, key, nonce);

    // Verify tag BEFORE decrypting (don't leak plaintext on bad tag).
    poly1305_ctx_t pctx;
    poly1305_init(&pctx, otk);
    poly_update_with_pad16(&pctx, aad, aad_len);
    poly_update_with_pad16(&pctx, ct,  ct_len);
    uint8_t lengths[16];
    put_le64(lengths + 0, aad_len);
    put_le64(lengths + 8, ct_len);
    poly1305_update(&pctx, lengths, 16);
    uint8_t computed_tag[16];
    poly1305_final(&pctx, computed_tag);

    // Wipe OTK whatever the result.
    for (int i = 0; i < 32; i++) otk[i] = 0;

    if (ct_memeq16(tag, computed_tag) != 0) return -1;

    // Tag OK — decrypt.
    chacha20_xor(pt, (const uint8_t*)ct, ct_len, key, nonce, 1);
    return 0;
}
