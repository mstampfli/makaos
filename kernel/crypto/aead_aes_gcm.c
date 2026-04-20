// ── AES-128-GCM / AES-256-GCM AEAD (NIST SP 800-38D) ──────────────
//
// Standard GCM with 12-byte nonce → J0 = IV ‖ 0x00000001.
// GHASH over (AAD ‖ pad16 ‖ CT ‖ pad16 ‖ u64_be(aad_len*8) ‖ u64_be(ct_len*8)).
// Tag = AES_E(K, J0) XOR GHASH(H, mac_input).
//
// H is computed as AES_E(K, 0^128).  GHASH multiplies in GF(2^128)
// with the reduction polynomial x^128 + x^7 + x^2 + x + 1.  We do
// this bit-by-bit — not the fastest (real impls use PCLMULQDQ or
// a per-byte table), but portable and correct.

#include "aead.h"
#include "aes.h"

// XOR two 16-byte blocks.
static void xor16(uint8_t dst[16], const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 16; i++) dst[i] = a[i] ^ b[i];
}

// Multiply X by Y in GF(2^128), bit-by-bit.  Constant time.
static void gf128_mul(uint8_t out[16], const uint8_t X[16], const uint8_t Y[16]) {
    uint8_t Z[16] = {0};
    uint8_t V[16];
    for (int i = 0; i < 16; i++) V[i] = Y[i];

    for (int i = 0; i < 128; i++) {
        // If bit i of X is set (scan big-endian, MSB first), Z ^= V.
        uint8_t bit = (X[i >> 3] >> (7 - (i & 7))) & 1;
        uint8_t mask = (uint8_t)(-bit);  // 0xFF or 0x00
        for (int j = 0; j < 16; j++) Z[j] ^= (uint8_t)(V[j] & mask);

        // V = V >> 1; if (V_was_odd) V ^= R  where R = 0xE1 ‖ 0^120
        uint8_t lsb = (uint8_t)(V[15] & 1);
        for (int j = 15; j > 0; j--) V[j] = (uint8_t)((V[j] >> 1) | (V[j - 1] << 7));
        V[0] = (uint8_t)(V[0] >> 1);
        // Reduction: if lsb set, XOR R where R[0] = 0xE1.
        V[0] ^= (uint8_t)(lsb * 0xE1);
    }
    for (int i = 0; i < 16; i++) out[i] = Z[i];
}

static void ghash_update(uint8_t Y[16], const uint8_t H[16],
                          const uint8_t* block, uint64_t nbytes) {
    uint8_t tmp[16];
    while (nbytes >= 16) {
        xor16(tmp, Y, block);
        gf128_mul(Y, tmp, H);
        block += 16; nbytes -= 16;
    }
    if (nbytes) {
        uint8_t last[16] = {0};
        for (uint64_t i = 0; i < nbytes; i++) last[i] = block[i];
        xor16(tmp, Y, last);
        gf128_mul(Y, tmp, H);
    }
}

static void inc32_be(uint8_t ctr[16]) {
    // Increment the last 32 bits as big-endian.
    for (int i = 15; i >= 12; i--) {
        if (++ctr[i]) break;
    }
}

static void aes_gcm_ctr(const aes_key_t* k,
                         uint8_t ctr[16],
                         const uint8_t* in, uint8_t* out, uint64_t len) {
    uint8_t ks[16];
    while (len) {
        aes_encrypt(k, ctr, ks);
        inc32_be(ctr);
        uint64_t n = (len > 16) ? 16 : len;
        for (uint64_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n; out += n; len -= n;
    }
}

// Core seal — parameterised over key size via a pre-initialised
// aes_key_t.
static void gcm_seal_with_key(const aes_key_t* k,
                                const uint8_t nonce[AEAD_NONCE_SZ],
                                const void* aad, uint64_t aad_len,
                                const void* pt,  uint64_t pt_len,
                                uint8_t* ct,
                                uint8_t tag[AEAD_TAG_SZ]) {
    uint8_t H[16] = {0};
    aes_encrypt(k, H, H);   // H = AES_E(K, 0^128)

    // J0 = nonce ‖ 0x00000001 (12-byte nonce path)
    uint8_t J0[16];
    for (int i = 0; i < 12; i++) J0[i] = nonce[i];
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    // Encrypt plaintext in CTR mode starting at J0 + 1.
    uint8_t ctr[16];
    for (int i = 0; i < 16; i++) ctr[i] = J0[i];
    inc32_be(ctr);
    aes_gcm_ctr(k, ctr, (const uint8_t*)pt, ct, pt_len);

    // GHASH.
    uint8_t Y[16] = {0};
    ghash_update(Y, H, (const uint8_t*)aad, aad_len);
    ghash_update(Y, H, ct, pt_len);
    // Lengths in bits (big-endian 64-bit each).
    uint8_t L[16];
    uint64_t aad_bits = aad_len * 8;
    uint64_t ct_bits  = pt_len  * 8;
    for (int i = 0; i < 8; i++) L[i]     = (uint8_t)(aad_bits >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) L[8 + i] = (uint8_t)(ct_bits  >> (56 - 8 * i));
    ghash_update(Y, H, L, 16);

    // Tag = AES_E(K, J0) XOR Y
    uint8_t ej0[16];
    aes_encrypt(k, J0, ej0);
    for (int i = 0; i < 16; i++) tag[i] = Y[i] ^ ej0[i];
}

static int gcm_open_with_key(const aes_key_t* k,
                               const uint8_t nonce[AEAD_NONCE_SZ],
                               const void* aad, uint64_t aad_len,
                               const void* ct,  uint64_t ct_len,
                               const uint8_t tag[AEAD_TAG_SZ],
                               uint8_t* pt) {
    uint8_t H[16] = {0};
    aes_encrypt(k, H, H);
    uint8_t J0[16];
    for (int i = 0; i < 12; i++) J0[i] = nonce[i];
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    // GHASH over (AAD ‖ CT ‖ lengths) — over CIPHERTEXT, not plaintext.
    uint8_t Y[16] = {0};
    ghash_update(Y, H, (const uint8_t*)aad, aad_len);
    ghash_update(Y, H, (const uint8_t*)ct,  ct_len);
    uint8_t L[16];
    uint64_t aad_bits = aad_len * 8;
    uint64_t ct_bits  = ct_len  * 8;
    for (int i = 0; i < 8; i++) L[i]     = (uint8_t)(aad_bits >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) L[8 + i] = (uint8_t)(ct_bits  >> (56 - 8 * i));
    ghash_update(Y, H, L, 16);

    uint8_t ej0[16], computed_tag[16];
    aes_encrypt(k, J0, ej0);
    for (int i = 0; i < 16; i++) computed_tag[i] = Y[i] ^ ej0[i];

    if (ct_memeq16(tag, computed_tag) != 0) return -1;

    // Tag OK — decrypt.
    uint8_t ctr[16];
    for (int i = 0; i < 16; i++) ctr[i] = J0[i];
    inc32_be(ctr);
    aes_gcm_ctr(k, ctr, (const uint8_t*)ct, pt, ct_len);
    return 0;
}

void aes128_gcm_seal(const uint8_t key[AEAD_AES128_KEY],
                      const uint8_t nonce[AEAD_NONCE_SZ],
                      const void*   aad, uint64_t aad_len,
                      const void*   pt,  uint64_t pt_len,
                      uint8_t*      ct,
                      uint8_t       tag[AEAD_TAG_SZ]) {
    aes_key_t k; aes_setkey_128(&k, key);
    gcm_seal_with_key(&k, nonce, aad, aad_len, pt, pt_len, ct, tag);
}

int aes128_gcm_open(const uint8_t key[AEAD_AES128_KEY],
                     const uint8_t nonce[AEAD_NONCE_SZ],
                     const void*   aad, uint64_t aad_len,
                     const void*   ct,  uint64_t ct_len,
                     const uint8_t tag[AEAD_TAG_SZ],
                     uint8_t*      pt) {
    aes_key_t k; aes_setkey_128(&k, key);
    return gcm_open_with_key(&k, nonce, aad, aad_len, ct, ct_len, tag, pt);
}

void aes256_gcm_seal(const uint8_t key[AEAD_AES256_KEY],
                      const uint8_t nonce[AEAD_NONCE_SZ],
                      const void*   aad, uint64_t aad_len,
                      const void*   pt,  uint64_t pt_len,
                      uint8_t*      ct,
                      uint8_t       tag[AEAD_TAG_SZ]) {
    aes_key_t k; aes_setkey_256(&k, key);
    gcm_seal_with_key(&k, nonce, aad, aad_len, pt, pt_len, ct, tag);
}

int aes256_gcm_open(const uint8_t key[AEAD_AES256_KEY],
                     const uint8_t nonce[AEAD_NONCE_SZ],
                     const void*   aad, uint64_t aad_len,
                     const void*   ct,  uint64_t ct_len,
                     const uint8_t tag[AEAD_TAG_SZ],
                     uint8_t*      pt) {
    aes_key_t k; aes_setkey_256(&k, key);
    return gcm_open_with_key(&k, nonce, aad, aad_len, ct, ct_len, tag, pt);
}
