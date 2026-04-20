// ── AES-128/AES-256 — T-table encrypt ─────────────────────────────
//
// Standard 4-table approach: each column's transformation is a
// 256-entry 32-bit lookup combining SubBytes + MixColumns.  Keeps
// the round body to 4 XORs per column + AddRoundKey.  AES decrypt
// not implemented (GCM uses only encrypt direction).

#include "aes.h"

static const uint8_t SBOX[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5, 0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0, 0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc, 0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a, 0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0, 0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b, 0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85, 0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5, 0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17, 0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88, 0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c, 0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9, 0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6, 0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e, 0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94, 0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68, 0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t RCON[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

// xtime: multiplication by x in GF(2^8).
static uint8_t xt(uint8_t b) {
    return (uint8_t)((b << 1) ^ ((b >> 7) * 0x1b));
}

// Build T-tables lazily on first use.  Each Te[x][b] is:
//   [ 2*S[b], S[b], S[b], 3*S[b] ]  rotated per-column.
static uint32_t Te0[256], Te1[256], Te2[256], Te3[256];
static int t_built = 0;

static void build_tables(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t s  = SBOX[i];
        uint8_t s2 = xt(s);            // 2*S in GF(2^8)
        uint8_t s3 = (uint8_t)(s2 ^ s); // 3*S
        // Big-endian state convention: MSB = row 0.  MixColumns on
        // [S, 0, 0, 0] gives output [2S, S, S, 3S], which packs as
        // u32 with 2S in MSB.  Similarly rotated for Te1/2/3.
        Te0[i] = ((uint32_t)s2 << 24) | ((uint32_t)s  << 16) | ((uint32_t)s  <<  8) | (uint32_t)s3;
        Te1[i] = ((uint32_t)s3 << 24) | ((uint32_t)s2 << 16) | ((uint32_t)s  <<  8) | (uint32_t)s;
        Te2[i] = ((uint32_t)s  << 24) | ((uint32_t)s3 << 16) | ((uint32_t)s2 <<  8) | (uint32_t)s;
        Te3[i] = ((uint32_t)s  << 24) | ((uint32_t)s  << 16) | ((uint32_t)s3 <<  8) | (uint32_t)s2;
    }
    t_built = 1;
}

static uint32_t be32_(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static void put_be32_(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t) v;
}

static uint32_t sub_word(uint32_t w) {
    return  (uint32_t)SBOX[(w >> 24) & 0xff] << 24
          | (uint32_t)SBOX[(w >> 16) & 0xff] << 16
          | (uint32_t)SBOX[(w >>  8) & 0xff] <<  8
          |  (uint32_t)SBOX[ w        & 0xff];
}
static uint32_t rot_word(uint32_t w) {
    return (w << 8) | (w >> 24);
}

void aes_setkey_128(aes_key_t* k, const uint8_t key[16]) {
    if (!t_built) build_tables();
    for (int i = 0; i < 4; i++) k->rk[i] = be32_(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t t = k->rk[i - 1];
        if ((i & 3) == 0) t = sub_word(rot_word(t)) ^ ((uint32_t)RCON[i >> 2] << 24);
        k->rk[i] = k->rk[i - 4] ^ t;
    }
    k->nrounds = 10;
}

void aes_setkey_256(aes_key_t* k, const uint8_t key[32]) {
    if (!t_built) build_tables();
    for (int i = 0; i < 8; i++) k->rk[i] = be32_(key + 4 * i);
    for (int i = 8; i < 60; i++) {
        uint32_t t = k->rk[i - 1];
        if ((i & 7) == 0)      t = sub_word(rot_word(t)) ^ ((uint32_t)RCON[i >> 3] << 24);
        else if ((i & 7) == 4) t = sub_word(t);
        k->rk[i] = k->rk[i - 8] ^ t;
    }
    k->nrounds = 14;
}

void aes_encrypt(const aes_key_t* k,
                  const uint8_t  in [16],
                  uint8_t        out[16]) {
    uint32_t s0 = be32_(in     ) ^ k->rk[0];
    uint32_t s1 = be32_(in +  4) ^ k->rk[1];
    uint32_t s2 = be32_(in +  8) ^ k->rk[2];
    uint32_t s3 = be32_(in + 12) ^ k->rk[3];

    int rk = 4;
    int rounds_minus_1 = k->nrounds - 1;
    for (int r = 0; r < rounds_minus_1; r++) {
        uint32_t t0 = Te0[(s0 >> 24) & 0xff] ^ Te1[(s1 >> 16) & 0xff]
                    ^ Te2[(s2 >>  8) & 0xff] ^ Te3[ s3        & 0xff] ^ k->rk[rk    ];
        uint32_t t1 = Te0[(s1 >> 24) & 0xff] ^ Te1[(s2 >> 16) & 0xff]
                    ^ Te2[(s3 >>  8) & 0xff] ^ Te3[ s0        & 0xff] ^ k->rk[rk + 1];
        uint32_t t2 = Te0[(s2 >> 24) & 0xff] ^ Te1[(s3 >> 16) & 0xff]
                    ^ Te2[(s0 >>  8) & 0xff] ^ Te3[ s1        & 0xff] ^ k->rk[rk + 2];
        uint32_t t3 = Te0[(s3 >> 24) & 0xff] ^ Te1[(s0 >> 16) & 0xff]
                    ^ Te2[(s1 >>  8) & 0xff] ^ Te3[ s2        & 0xff] ^ k->rk[rk + 3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
        rk += 4;
    }

    // Final round: no MixColumns.  Manually compose from SBOX.
    // NB: ^ binds tighter than |, so we MUST parenthesise the
    // composed word before XOR-ing the round key (earlier bug).
    uint32_t t0 = (((uint32_t)SBOX[(s0 >> 24) & 0xff] << 24)
                 | ((uint32_t)SBOX[(s1 >> 16) & 0xff] << 16)
                 | ((uint32_t)SBOX[(s2 >>  8) & 0xff] <<  8)
                 |  (uint32_t)SBOX[ s3        & 0xff])
                 ^ k->rk[rk    ];
    uint32_t t1 = (((uint32_t)SBOX[(s1 >> 24) & 0xff] << 24)
                 | ((uint32_t)SBOX[(s2 >> 16) & 0xff] << 16)
                 | ((uint32_t)SBOX[(s3 >>  8) & 0xff] <<  8)
                 |  (uint32_t)SBOX[ s0        & 0xff])
                 ^ k->rk[rk + 1];
    uint32_t t2 = (((uint32_t)SBOX[(s2 >> 24) & 0xff] << 24)
                 | ((uint32_t)SBOX[(s3 >> 16) & 0xff] << 16)
                 | ((uint32_t)SBOX[(s0 >>  8) & 0xff] <<  8)
                 |  (uint32_t)SBOX[ s1        & 0xff])
                 ^ k->rk[rk + 2];
    uint32_t t3 = (((uint32_t)SBOX[(s3 >> 24) & 0xff] << 24)
                 | ((uint32_t)SBOX[(s0 >> 16) & 0xff] << 16)
                 | ((uint32_t)SBOX[(s1 >>  8) & 0xff] <<  8)
                 |  (uint32_t)SBOX[ s2        & 0xff])
                 ^ k->rk[rk + 3];

    put_be32_(out     , t0);
    put_be32_(out +  4, t1);
    put_be32_(out +  8, t2);
    put_be32_(out + 12, t3);
}
