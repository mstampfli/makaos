// ── conn_t: TLS-aware socket wrapper built on mbedTLS ────────────────
//
// Opens a TCP connection (+ optional TLS handshake) and lets callers
// read/write through a single interface.  The point is to keep the
// HTTP state machine in http_get.c oblivious to whether bytes are
// encrypted — identical logic handles http:// and https://.

#include "https_client.h"

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>   // harmless even with MBEDTLS_NET_C off

// Glue symbols implemented in userland/libs/mbedtls/mbedtls_glue.c.
extern int mbedtls_hardware_poll(void* data, unsigned char* output,
                                 size_t len, size_t* olen);
extern int mbedtls_bio_send(void* ctx, const unsigned char* buf, size_t len);
extern int mbedtls_bio_recv(void* ctx, unsigned char* buf, size_t len);

struct conn_t {
    int fd;
    int is_tls;
    // TLS context, only populated when is_tls == 1.
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
};

static void write_err(const char* s) { write(2, s, strlen(s)); }

// Convenience — prints an mbedTLS error code with a short name.
static void write_mbedtls_err(const char* pfx, int rc) {
    char msg[160];
    int n = snprintf(msg, sizeof(msg), "* %s: mbedtls rc=-0x%x\n",
                     pfx, (unsigned)(-rc));
    if (n > 0) write(2, msg, (size_t)n);
}

static int tcp_connect(const char* host, uint16_t port, int verbose) {
    uint32_t ip_be;
    // Retry DNS with a short backoff: on a cold boot, the DHCP client
    // (/bin/net) races with our launch and may not have installed the
    // DNS servers yet.  30 attempts × 1s = 30s budget, plenty for a
    // VM DHCP lease.
    int resolved = 0;
    for (int attempt = 0; attempt < 30; attempt++) {
        if (gethostbyname_ipv4(host, &ip_be) == 0) { resolved = 1; break; }
        if (attempt == 0 && verbose) write_err("* DNS not ready, retrying...\n");
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
    }
    if (!resolved) {
        write_err("* DNS resolution failed\n");
        return -1;
    }
    if (verbose) {
        char ipstr[16];
        inet_ntop(AF_INET, &ip_be, ipstr, sizeof(ipstr));
        char line[96];
        int n = snprintf(line, sizeof(line),
                         "* connecting to %s:%u\n", ipstr, (unsigned)port);
        if (n > 0) write(2, line, (size_t)n);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { write_err("* socket() failed\n"); return -1; }

    sockaddr_in_t dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    dst.sin_addr   = ip_be;
    if (connect(fd, (struct sockaddr*)&dst, sizeof(dst)) < 0) {
        write_err("* connect() failed\n");
        close(fd);
        return -1;
    }
    return fd;
}

static int tls_handshake(conn_t* c, const char* host, int insecure, int verbose) {
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);

    // DRBG seeded from our hardware_poll hook (entropy framework calls
    // mbedtls_hardware_poll via ENTROPY_HARDWARE_ALT).
    int rc = mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func,
                                    &c->entropy,
                                    (const unsigned char*)"MakaOS-https", 12);
    if (rc != 0) { write_mbedtls_err("ctr_drbg_seed", rc); return -1; }

    rc = mbedtls_ssl_config_defaults(&c->conf,
                                     MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { write_mbedtls_err("ssl_config_defaults", rc); return -1; }

    // No trust store yet — user opts into insecure mode explicitly.
    // Once we ship a CA bundle, drop MBEDTLS_SSL_VERIFY_NONE entirely.
    mbedtls_ssl_conf_authmode(&c->conf,
        insecure ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

    rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (rc != 0) { write_mbedtls_err("ssl_setup", rc); return -1; }

    // SNI — required by virtual-host-heavy public web.
    rc = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (rc != 0) { write_mbedtls_err("ssl_set_hostname", rc); return -1; }

    // BIO bound to our socket fd.  The third argument is the opaque
    // ctx passed back to our send/recv callbacks; point it at fd so
    // the callback can do send(fd, ...) / recv(fd, ...).
    mbedtls_ssl_set_bio(&c->ssl, &c->fd,
                        mbedtls_bio_send, mbedtls_bio_recv, NULL);

    if (verbose) write_err("* TLS handshake...\n");

    // Blocking BIO ⇒ a single call drives the whole handshake.  WANT_*
    // should not appear, but handle them defensively so a future
    // non-blocking backend doesn't silently fail.
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ &&
            rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            write_mbedtls_err("ssl_handshake", rc);
            return -1;
        }
    }

    if (verbose) {
        const char* ver = mbedtls_ssl_get_version(&c->ssl);
        const char* suite = mbedtls_ssl_get_ciphersuite(&c->ssl);
        char line[160];
        int n = snprintf(line, sizeof(line), "* TLS ok: %s / %s\n",
                         ver ? ver : "?", suite ? suite : "?");
        if (n > 0) write(2, line, (size_t)n);
    }
    return 0;
}

conn_t* conn_open(const char* host, uint16_t port,
                  int use_tls, int insecure, int verbose) {
    conn_t* c = (conn_t*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->is_tls = use_tls;

    c->fd = tcp_connect(host, port, verbose);
    if (c->fd < 0) { free(c); return NULL; }

    if (use_tls) {
        if (tls_handshake(c, host, insecure, verbose) < 0) {
            mbedtls_ssl_free(&c->ssl);
            mbedtls_ssl_config_free(&c->conf);
            mbedtls_ctr_drbg_free(&c->ctr_drbg);
            mbedtls_entropy_free(&c->entropy);
            close(c->fd);
            free(c);
            return NULL;
        }
    }
    return c;
}

ssize_t conn_send(conn_t* c, const void* buf, size_t len) {
    if (!c->is_tls) return send(c->fd, buf, len, 0);
    int rc = mbedtls_ssl_write(&c->ssl, (const unsigned char*)buf, len);
    if (rc < 0) return -1;
    return (ssize_t)rc;
}

ssize_t conn_recv(conn_t* c, void* buf, size_t len) {
    if (!c->is_tls) return recv(c->fd, buf, len, 0);
    int rc = mbedtls_ssl_read(&c->ssl, (unsigned char*)buf, len);
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;   // clean TLS EOF
    if (rc == MBEDTLS_ERR_SSL_CONN_EOF)          return 0;   // TCP closed mid-record
    if (rc < 0) return -1;
    return (ssize_t)rc;
}

void conn_close(conn_t* c) {
    if (!c) return;
    if (c->is_tls) {
        (void)mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->ctr_drbg);
        mbedtls_entropy_free(&c->entropy);
    }
    if (c->fd >= 0) close(c->fd);
    free(c);
}
