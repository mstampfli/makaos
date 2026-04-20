// http_get — MakaOS HTTP/1.1 client (now with HTTPS via mbedTLS)
//
// Usage:
//   http_get [-v] [-k] [-o outfile] <url>
//
// Supports:
//   • Absolute URLs: http://host[:port][/path]  AND  https://...
//   • TLS via mbedTLS 3.6 (TLS 1.3 + 1.2 fallback, X25519 / P-256 /
//     P-384 / RSA, AES-GCM / ChaCha20-Poly1305).  -k to skip cert
//     verification (we don't ship a CA bundle yet).
//   • HTTP/1.1 GET with Host / User-Agent / Connection: close
//   • Chunked transfer encoding (RFC 7230 §4.1)
//   • Content-Length framing
//   • Redirect chain: 301/302/303/307/308, max 10 hops
//   • Writes body to stdout (or -o file)

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include "https_client.h"

#define HTTP_PORT_DEFAULT  80
#define HTTPS_PORT_DEFAULT 443
#define RECV_BUF_SIZE      4096
#define MAX_HEADER_SIZE    8192
#define MAX_REDIRECTS      10

// ── URL parsing ───────────────────────────────────────────────────────────

typedef struct {
    char     host[256];
    uint16_t port;
    int      is_https;          // 1 → TLS, 0 → plain HTTP
    char     path[1024];
} url_t;

static int parse_url(const char* url, url_t* out) {
    memset(out, 0, sizeof(*out));
    out->port = HTTP_PORT_DEFAULT;

    const char* p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        out->is_https = 0;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        out->is_https = 1;
        out->port     = HTTPS_PORT_DEFAULT;
    }

    // Host[:port]
    const char* host_start = p;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') p++;
    size_t hlen = (size_t)(p - host_start);
    if (hlen == 0 || hlen >= sizeof(out->host)) return -1;
    memcpy(out->host, host_start, hlen);
    out->host[hlen] = '\0';

    if (*p == ':') {
        p++;
        uint32_t port = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10u + (uint32_t)(*p - '0');
            if (port > 65535) return -1;
            p++;
        }
        out->port = (uint16_t)port;
    }

    // Path
    if (*p == '\0' || *p == '#') {
        strcpy(out->path, "/");
    } else {
        const char* path_start = p;
        const char* path_end   = path_start;
        while (*path_end && *path_end != '#') path_end++;
        size_t plen = (size_t)(path_end - path_start);
        if (plen >= sizeof(out->path)) return -1;
        memcpy(out->path, path_start, plen);
        out->path[plen] = '\0';
    }
    return 0;
}

// ── conn_t I/O helpers (transparent over HTTP / HTTPS) ───────────────────

static int send_all(conn_t* c, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = conn_send(c, p, remaining);
        if (n <= 0) return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

// Read until we've seen "\r\n\r\n" (end of response headers).
// Returns the *total* number of bytes in buf on success (which may exceed
// the header block if the server packed header+body into one segment), and
// writes the header-end position (first byte of body) to *header_end_out.
// Returns -1 on error.
static ssize_t recv_headers(conn_t* c, char* buf, size_t max, size_t* header_end_out) {
    size_t total = 0;
    while (total < max - 1) {
        ssize_t n = conn_recv(c, buf + total, max - 1 - total);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';

        // Look for \r\n\r\n
        for (size_t i = 3; i < total; i++) {
            if (buf[i-3] == '\r' && buf[i-2] == '\n' &&
                buf[i-1] == '\r' && buf[i]   == '\n') {
                if (header_end_out) *header_end_out = i + 1;
                return (ssize_t)total;
            }
        }
    }
    return -1;
}

// ── Header parsing ────────────────────────────────────────────────────────

// Case-insensitive header lookup. Writes up to out_max-1 bytes to out.
// Returns 1 if found, 0 otherwise.
static int find_header(const char* headers, size_t hlen, const char* name,
                        char* out, size_t out_max) {
    size_t nlen = strlen(name);
    const char* line = headers;
    // Skip status line
    const char* nl = memchr(line, '\n', hlen);
    if (!nl) return 0;
    nl++;
    size_t remaining = hlen - (size_t)(nl - headers);
    line = nl;

    while (remaining > 0) {
        const char* eol = memchr(line, '\n', remaining);
        if (!eol) break;
        size_t llen = (size_t)(eol - line);
        // trim \r
        if (llen > 0 && line[llen - 1] == '\r') llen--;
        if (llen == 0) { line = eol + 1; remaining -= (size_t)(eol - line + 1); break; }

        if (llen > nlen + 1 && strncasecmp(line, name, nlen) == 0 &&
            line[nlen] == ':') {
            const char* v = line + nlen + 1;
            size_t vlen = llen - nlen - 1;
            while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
            size_t copy = vlen < out_max - 1 ? vlen : out_max - 1;
            memcpy(out, v, copy);
            out[copy] = '\0';
            return 1;
        }
        line = eol + 1;
        remaining -= (size_t)(eol - line + 1) + 1;
        if (line > headers + hlen) break;
    }
    return 0;
}

// Parse "HTTP/1.x NNN Reason".  Returns the integer status code, -1 on error.
static int parse_status(const char* headers) {
    // Skip HTTP/1.x
    const char* p = headers;
    while (*p && *p != ' ') p++;
    if (*p != ' ') return -1;
    p++;
    int code = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        code = code * 10 + (*p - '0');
        digits++;
        p++;
    }
    if (digits != 3) return -1;
    return code;
}

// ── Body readers ──────────────────────────────────────────────────────────

// Read exactly `n` bytes from `c` into `out_fd`, using `initial` bytes already
// buffered (from the header read).
static int drain_content_length(conn_t* c, int out_fd, const uint8_t* initial,
                                  size_t initial_len, uint64_t content_len) {
    uint64_t remaining = content_len;

    // First, write out whatever we already have.
    if (initial_len > 0) {
        size_t w = initial_len > remaining ? (size_t)remaining : initial_len;
        if (write(out_fd, initial, w) != (ssize_t)w) return -1;
        remaining -= w;
    }

    uint8_t buf[RECV_BUF_SIZE];
    while (remaining > 0) {
        size_t want = remaining > sizeof(buf) ? sizeof(buf) : (size_t)remaining;
        ssize_t n = conn_recv(c, buf, want);
        if (n <= 0) return -1;
        if (write(out_fd, buf, (size_t)n) != n) return -1;
        remaining -= (size_t)n;
    }
    return 0;
}

// Read HTTP chunked body. `initial` contains bytes that were read past the
// end of the header block; we continue reading from `c` as needed.
typedef struct {
    conn_t* c;
    uint8_t buf[RECV_BUF_SIZE];
    size_t  pos;
    size_t  len;
} chunked_reader_t;

static int cr_fill(chunked_reader_t* cr) {
    if (cr->pos < cr->len) return 0;
    ssize_t n = conn_recv(cr->c, cr->buf, sizeof(cr->buf));
    if (n <= 0) return -1;
    cr->pos = 0;
    cr->len = (size_t)n;
    return 0;
}

// Read a single byte.  Returns 0/-1.
static int cr_readb(chunked_reader_t* cr, uint8_t* out) {
    if (cr_fill(cr) < 0) return -1;
    *out = cr->buf[cr->pos++];
    return 0;
}

// Read CRLF-terminated line. Writes up to line_max-1 bytes to `line`.
static int cr_readline(chunked_reader_t* cr, char* line, size_t line_max) {
    size_t w = 0;
    int seen_cr = 0;
    while (w < line_max - 1) {
        uint8_t c;
        if (cr_readb(cr, &c) < 0) return -1;
        if (seen_cr) {
            if (c == '\n') { line[w - 1] = '\0'; return 0; }
            seen_cr = 0;
        }
        if (c == '\r') { seen_cr = 1; }
        line[w++] = (char)c;
    }
    return -1;
}

// Read `n` bytes from cr, write to out_fd.
static int cr_readn(chunked_reader_t* cr, int out_fd, uint64_t n) {
    while (n > 0) {
        if (cr->pos >= cr->len) {
            if (cr_fill(cr) < 0) return -1;
        }
        size_t avail = cr->len - cr->pos;
        size_t take = avail < n ? avail : (size_t)n;
        if (write(out_fd, cr->buf + cr->pos, take) != (ssize_t)take) return -1;
        cr->pos += take;
        n -= take;
    }
    return 0;
}

static int drain_chunked(conn_t* c, int out_fd,
                          const uint8_t* initial, size_t initial_len) {
    chunked_reader_t cr;
    cr.c   = c;
    cr.pos = 0;
    cr.len = 0;
    if (initial_len > sizeof(cr.buf)) return -1;
    if (initial_len > 0) {
        memcpy(cr.buf, initial, initial_len);
        cr.len = initial_len;
    }

    for (;;) {
        char line[64];
        if (cr_readline(&cr, line, sizeof(line)) < 0) return -1;
        // Ignore chunk extensions after ";"
        char* semi = strchr(line, ';');
        if (semi) *semi = '\0';
        uint64_t sz = (uint64_t)strtoul(line, NULL, 16);
        if (sz == 0) {
            // Trailer: read lines until blank
            while (cr_readline(&cr, line, sizeof(line)) == 0 && line[0])
                ;
            return 0;
        }
        if (cr_readn(&cr, out_fd, sz) < 0) return -1;
        // CRLF after chunk data
        char crlf[4];
        if (cr_readline(&cr, crlf, sizeof(crlf)) < 0) return -1;
    }
}

// Read body until the server closes the connection (no C-L, no chunked).
static int drain_until_close(conn_t* c, int out_fd,
                              const uint8_t* initial, size_t initial_len) {
    if (initial_len > 0) {
        if (write(out_fd, initial, initial_len) != (ssize_t)initial_len)
            return -1;
    }
    uint8_t buf[RECV_BUF_SIZE];
    for (;;) {
        ssize_t n = conn_recv(c, buf, sizeof(buf));
        if (n == 0) return 0;
        if (n < 0)  return -1;
        if (write(out_fd, buf, (size_t)n) != n) return -1;
    }
}

// ── HTTP transaction ──────────────────────────────────────────────────────

// Result of one GET request.
typedef struct {
    int  status;
    char location[1024];
} http_result_t;

static int http_get_one(const url_t* url, int verbose, int insecure,
                         int out_fd, int raw_fd, http_result_t* r) {
    if (verbose) {
        char line[512];
        int n = snprintf(line, sizeof(line),
                         "* resolving %s...\n", url->host);
        if (n > 0) write(2, line, (size_t)n);
    }

    conn_t* c = conn_open(url->host, url->port, url->is_https, insecure, verbose);
    if (!c) return -1;

    // Build request
    char req[2048];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: MakaOS-http_get/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        url->path, url->host);
    if (rlen < 0 || rlen >= (int)sizeof(req)) {
        conn_close(c);
        return -1;
    }

    if (verbose) {
        write(2, "> ", 2);
        write(2, req, (size_t)rlen);
    }

    if (send_all(c, req, (size_t)rlen) < 0) {
        write(2, "* send failed\n", 14);
        conn_close(c);
        return -1;
    }

    // Receive response headers (recv_headers returns total bytes read,
    // which may include peeked body bytes; header_end points at body start).
    static char hdr[MAX_HEADER_SIZE];
    size_t header_end = 0;
    ssize_t total = recv_headers(c, hdr, sizeof(hdr), &header_end);
    if (total < 0 || header_end == 0) {
        write(2, "* failed to read headers\n", 25);
        conn_close(c);
        return -1;
    }

    // Sidecar log: append raw response bytes (headers + any peeked body) so
    // callers get to see every hop even if the redirect chain aborts later.
    if (raw_fd >= 0 && total > 0) (void)write(raw_fd, hdr, (size_t)total);

    // Parse status line
    int status = parse_status(hdr);
    if (status < 0) { conn_close(c); return -1; }
    r->status = status;
    r->location[0] = '\0';

    if (verbose) {
        char line[80];
        int n = snprintf(line, sizeof(line), "< HTTP %d\n", status);
        if (n > 0) write(2, line, (size_t)n);
    }

    // Redirects: capture Location and return without reading body.
    if (status == 301 || status == 302 || status == 303 ||
        status == 307 || status == 308) {
        find_header(hdr, header_end, "Location", r->location, sizeof(r->location));
        // Tee any body already read past the header block, then drain a
        // bit more into the sidecar so callers can see the redirect page.
        if (raw_fd >= 0) {
            size_t initial_len = (size_t)total - header_end;
            if (initial_len > 0)
                (void)write(raw_fd, hdr + header_end, initial_len);
            uint8_t buf[RECV_BUF_SIZE];
            for (int i = 0; i < 16; i++) {
                ssize_t n = conn_recv(c, buf, sizeof(buf));
                if (n <= 0) break;
                (void)write(raw_fd, buf, (size_t)n);
            }
        }
        conn_close(c);
        return 0;
    }

    // Non-200-range: still drain body to stdout for diagnostic purposes.

    // How is the body framed?
    char te[64]  = {0};
    char cl[64]  = {0};
    int have_chunked = find_header(hdr, header_end, "Transfer-Encoding",
                                   te, sizeof(te)) &&
                      strcasecmp(te, "chunked") == 0;
    int have_cl = find_header(hdr, header_end, "Content-Length", cl, sizeof(cl));

    // Buffered body bytes already read past the header block
    const uint8_t* initial    = (const uint8_t*)hdr + header_end;
    size_t         initial_len = (size_t)total - header_end;

    int drain_rc;
    if (have_chunked) {
        drain_rc = drain_chunked(c, out_fd, initial, initial_len);
    } else if (have_cl) {
        uint64_t len = strtoul(cl, NULL, 10);
        drain_rc = drain_content_length(c, out_fd, initial, initial_len, len);
    } else {
        drain_rc = drain_until_close(c, out_fd, initial, initial_len);
    }

    conn_close(c);
    return drain_rc < 0 ? -1 : 0;
}

// Resolve a redirect location (may be absolute or path-relative).
static int resolve_redirect(const url_t* base, const char* loc, url_t* out) {
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
        return parse_url(loc, out);
    }
    // Path-relative: keep host and port, replace path.
    *out = *base;
    if (loc[0] == '/') {
        size_t pl = strlen(loc);
        if (pl >= sizeof(out->path)) return -1;
        memcpy(out->path, loc, pl + 1);
    } else {
        // Strip last path segment then append
        char* slash = strrchr(out->path, '/');
        if (slash) *(slash + 1) = '\0'; else strcpy(out->path, "/");
        size_t cur = strlen(out->path);
        size_t pl = strlen(loc);
        if (cur + pl >= sizeof(out->path)) return -1;
        memcpy(out->path + cur, loc, pl + 1);
    }
    return 0;
}

// ── main ──────────────────────────────────────────────────────────────────

static void usage(void) {
    const char* msg =
        "usage: http_get [-v] [-k] [-o outfile] [-e errfile] [-r rawfile] <url>\n"
        "  -k   skip TLS certificate verification (no CA bundle yet)\n";
    write(2, msg, strlen(msg));
}

int main(int argc, char** argv) {
    int verbose = 0;
    int insecure = 0;
    const char* out_path = NULL;
    const char* err_path = NULL;
    const char* raw_path = NULL;
    const char* url_arg  = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "-k") == 0) insecure = 1;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            err_path = argv[++i];
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            raw_path = argv[++i];
        } else if (argv[i][0] != '-' && !url_arg) {
            url_arg = argv[i];
        } else {
            usage();
            return 1;
        }
    }

    // Redirect stderr to err_path so the kernel can tail diagnostics.
    if (err_path) {
        int efd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (efd >= 0 && efd != 2) { dup2(efd, 2); close(efd); }
    }
    if (!url_arg) { usage(); return 1; }

    int out_fd = 1;
    if (out_path) {
        out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            write(2, "* cannot open output file\n", 26);
            return 1;
        }
    }

    // Sidecar raw-response log (append-only).  Every hop's full response
    // header block + any body bytes peeked in the same recv() land here
    // — a read-only witness of what the server actually said, preserved
    // across redirect follow-ups.
    int raw_fd = -1;
    if (raw_path) {
        raw_fd = open(raw_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }

    url_t url;
    if (parse_url(url_arg, &url) < 0) {
        write(2, "* malformed URL (or https not supported)\n", 41);
        if (out_fd > 2) close(out_fd);
        return 1;
    }

    for (int hop = 0; hop < MAX_REDIRECTS; hop++) {
        http_result_t r;
        memset(&r, 0, sizeof(r));
        if (http_get_one(&url, verbose, insecure, out_fd, raw_fd, &r) < 0) {
            if (out_fd > 2) close(out_fd);
            return 1;
        }
        if (r.location[0] == '\0') {
            // Final response, body written.
            if (out_fd > 2) close(out_fd);
            return (r.status >= 200 && r.status < 300) ? 0 : 2;
        }
        if (verbose) {
            char line[1100];
            int n = snprintf(line, sizeof(line), "* redirect → %s\n", r.location);
            if (n > 0) write(2, line, (size_t)n);
        }
        url_t next;
        if (resolve_redirect(&url, r.location, &next) < 0) {
            write(2, "* bad redirect target\n", 22);
            if (out_fd > 2) close(out_fd);
            return 1;
        }
        url = next;
    }

    write(2, "* too many redirects\n", 21);
    if (out_fd > 2) close(out_fd);
    return 1;
}
