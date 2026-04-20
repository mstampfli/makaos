// ── MakaOS mbedTLS build config ───────────────────────────────────────
//
// Enables TLS 1.3 (+ 1.2 fallback), modern AEADs, and both RSA- and
// ECDSA-signed X.509 chains.  Disables features that depend on a hosted
// POSIX environment (file I/O, BSD sockets, platform entropy sources)
// since MakaOS provides its own glue via mbedtls_hardware_poll + custom
// BIO callbacks.
//
// Trim goals:
//   * No DTLS (we only do TCP).
//   * No server mode (we're a client for now — http_get, DNSSEC,
//     update fetchers).  Re-enable MBEDTLS_SSL_SRV_C when we add a
//     TLS-terminating daemon.
//   * No legacy cipher suites: no 3DES, no RC4, no MD5 in MAC, no
//     ciphers with < 128-bit security.
//   * No PSA Crypto adapter — stick with the classic API for now; the
//     port script skips psa_*.c sources anyway.
//
// If mbedTLS fails to compile with this config, the first thing to
// check is whether we turned off a transitive dependency.  mbedTLS's
// include/mbedtls/check_config.h documents all the "FOO_C requires
// BAR_C" relationships.

#ifndef MAKAOS_MBEDTLS_CONFIG_H
#define MAKAOS_MBEDTLS_CONFIG_H

// ── Platform abstraction ──────────────────────────────────────────────

#define MBEDTLS_PLATFORM_C              // required by MS_TIME_ALT, etc.

// We have a real malloc/free in userland/libc/libc.c — let mbedtls
// call them directly (the default).  No MBEDTLS_PLATFORM_MEMORY needed.

// No stdio-backed fprintf/exit hooks — keep the defaults.
// (mbedtls uses printf only in debug paths we don't compile in.)

// We have time() in libc.  Certificate validity checks need it.
#define MBEDTLS_HAVE_TIME
// Skip MBEDTLS_HAVE_TIME_DATE — that needs gmtime_r which our libc lacks.
// mbedTLS falls back to a simpler epoch-seconds comparison without it.

// We provide our own millisecond clock in mbedtls_glue.c (libc lacks
// clock_gettime; just multiply time() by 1000 there).
#define MBEDTLS_PLATFORM_MS_TIME_ALT

// Let strstr etc. be the libc ones.  They're correct.
#define MBEDTLS_HAVE_ASM            // allow inline asm optimisations

// ── Entropy: MakaOS-provided only ─────────────────────────────────────
// Kernel CSPRNG (RDRAND + RDSEED + TSC jitter + per-IRQ timing + DRAM
// noise, SHA-256 pool, ChaCha20 DRBG) is exposed via /dev/urandom.
// The glue layer reads from there inside mbedtls_hardware_poll.

#define MBEDTLS_NO_PLATFORM_ENTROPY     // don't try /dev/urandom directly
#define MBEDTLS_ENTROPY_HARDWARE_ALT    // use our mbedtls_hardware_poll
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C              // counter-mode DRBG on top of entropy
#define MBEDTLS_HMAC_DRBG_C             // alt DRBG for EC key gen etc.

// ── PSA Crypto (required for TLS 1.3 in mbedTLS 3.x) ──────────────────
// TLS 1.3 in 3.x routes everything through PSA.  Enabling this + not
// defining MBEDTLS_PSA_CRYPTO_CONFIG makes mbedTLS auto-derive PSA_WANT_*
// symbols from our legacy MBEDTLS_*_C defines (see
// config_adjust_psa_from_legacy.h).  That's the path of least resistance.

#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_USE_PSA_CRYPTO
// No PSA persistent storage — we don't need stored keys (the handshake
// derives everything ephemerally).  This keeps us off the filesystem.

// ── Disable features that require a hosted POSIX env ──────────────────
// MBEDTLS_FS_IO   — fopen/fread; we load certs from memory
// MBEDTLS_NET_C   — socket() wrapper; custom BIO instead
// MBEDTLS_TIMING_C — POSIX gettimeofday loops; we stub via our clock
// No DTLS → no timing needed anyway.

// ── Cryptographic primitives ──────────────────────────────────────────

// Hashes
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

// HMAC / HKDF
#define MBEDTLS_MD_C
#define MBEDTLS_HKDF_C

// AEAD / block ciphers
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CCM_C                   // cheap to include; used by some TLS suites
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC         // needed for CBC cipher suites
#define MBEDTLS_CIPHER_PADDING_PKCS7

// Public-key crypto
#define MBEDTLS_BIGNUM_C                // mpi arithmetic
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21               // RSA-PSS (TLS 1.3 requires this)
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED

// PK abstraction (wraps RSA + ECDSA in one handle)
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C              // needed by ECDH ephemeral keygen

// ASN.1 / OID / Base64 — all needed by X.509
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C

// X.509 — certificate chain parsing + validation
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
// Skip MBEDTLS_X509_CRL_PARSE_C — revocation via OCSP / online only, not CRL.
// Skip MBEDTLS_X509_CSR_PARSE_C — no CA functionality.

// ── TLS ───────────────────────────────────────────────────────────────

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C               // client only for now
// #define MBEDTLS_SSL_SRV_C            // enable when adding TLS-terminating daemons

#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE       // required by TLS 1.3

// TLS 1.3 key-exchange modes — we support ECDHE + PSK (none restricts
// handshake to pure PSK, which we don't need).
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED

// Required for ECDHE-* cipher suites under TLS 1.2.
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

// Fragmentation + renegotiation — both off (modern servers don't need
// either, and renegotiation has a CVE history).
// MBEDTLS_SSL_RENEGOTIATION: off by default; leave it.
// MBEDTLS_SSL_MAX_FRAGMENT_LENGTH: off.

#define MBEDTLS_SSL_SERVER_NAME_INDICATION     // SNI for virtual hosting
#define MBEDTLS_SSL_ALPN                       // negotiate http/1.1, h2, etc.

// ── Sanity: trim unused areas ─────────────────────────────────────────

// These are enabled by default in mbedtls — leave the defaults alone
// except where freestanding-incompatible.

// Debug output goes to our custom hook (set via mbedtls_ssl_conf_dbg).
// MBEDTLS_DEBUG_C is off by default; enable only when actively tracing.
// #define MBEDTLS_DEBUG_C

#endif // MAKAOS_MBEDTLS_CONFIG_H
