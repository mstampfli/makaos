// ── MakaOS ↔ mbedTLS glue ─────────────────────────────────────────────
//
// The minimum wiring mbedTLS needs to run on MakaOS userland:
//
//   1. mbedtls_hardware_poll  — feed mbedtls_entropy with bytes from
//      the kernel CSPRNG (/dev/urandom).  The kernel pool already mixes
//      RDRAND + RDSEED + TSC jitter + per-IRQ timing + boot-DRAM noise
//      (see kernel/crypto/random.c).
//
//   2. mbedtls_ms_time        — millisecond monotonic-ish clock.  libc
//      lacks clock_gettime; we multiply time() seconds by 1000.
//
//   3. bio_send / bio_recv     — BIO callbacks that wrap a plain TCP
//      socket fd.  Handed to mbedtls_ssl_set_bio by the caller after
//      connect(); the TLS state machine calls these for every record.
//
// Everything else (malloc, memcpy, time, snprintf) is provided by
// userland/libc/libc.c and pulled in via the shim headers during the
// library build.

#include "libc.h"
#include <mbedtls/entropy.h>

// ── 1. Hardware entropy poll ──────────────────────────────────────────
// Config enables MBEDTLS_ENTROPY_HARDWARE_ALT so mbedtls_entropy_add_source
// won't try /dev/urandom on its own (MBEDTLS_NO_PLATFORM_ENTROPY).  All
// seed bytes flow through this one hook.
int mbedtls_hardware_poll(void* data, unsigned char* output,
                          size_t len, size_t* olen) {
    (void)data;
    int fd = open("/dev/urandom", 0 /*O_RDONLY*/, 0);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, output + got, len - got);
        if (n <= 0) { close(fd); return -1; }
        got += (size_t)n;
    }
    close(fd);
    *olen = got;
    return 0;
}

// ── 2. Millisecond clock ──────────────────────────────────────────────
// Config enables MBEDTLS_PLATFORM_MS_TIME_ALT because libc lacks
// clock_gettime(CLOCK_MONOTONIC).  time() resolution is seconds; that's
// fine — mbedTLS only uses ms_time for non-critical timing (handshake
// timeouts, not security-relevant).

typedef int64_t mbedtls_ms_time_t;

mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)time(NULL) * 1000;
}

// ── 3. BIO callbacks over a TCP socket fd ─────────────────────────────
// Caller does:
//     int fd = socket(...); connect(fd, ...);
//     mbedtls_ssl_set_bio(&ssl, &fd, mbedtls_bio_send, mbedtls_bio_recv, NULL);
// The ctx pointer we receive *is* the address of the int fd.

#include <mbedtls/ssl.h>

int mbedtls_bio_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    ssize_t n = send(fd, buf, len, 0);
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}

int mbedtls_bio_recv(void* ctx, unsigned char* buf, size_t len) {
    int fd = *(int*)ctx;
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    return (int)n;
}
