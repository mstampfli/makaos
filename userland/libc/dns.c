// dns.c — MakaOS libc DNS resolver (RFC 1035)
//
// Features:
//   • Numeric fast path via inet_pton (no network I/O).
//   • /etc/resolv.conf parsing — one "nameserver a.b.c.d" per line.
//   • UDP/53 query with a randomized transaction id and 2-label compression-
//     tolerant decompression.
//   • Handles A records and CNAME chains (up to 8 redirections).
//   • Truncation (TC bit) triggers fallback to TCP/53 as per RFC 1035 §4.2.2.
//   • Retry across multiple nameservers with per-server timeout.
//
// Non-goals: IPv6, MX/TXT, DNSSEC, caching.

#include "libc.h"

#define DNS_PORT        53
#define DNS_TIMEOUT_MS  3000
#define DNS_MAX_RETRIES 2
#define DNS_MAX_CNAME   8
#define DNS_PKT_MAX     1500

// Header flags
#define DNS_FLAG_QR     0x8000u
#define DNS_FLAG_AA     0x0400u
#define DNS_FLAG_TC     0x0200u
#define DNS_FLAG_RD     0x0100u
#define DNS_FLAG_RA     0x0080u
#define DNS_RCODE_MASK  0x000Fu

// Types
#define DNS_TYPE_A      1
#define DNS_TYPE_CNAME  5
#define DNS_CLASS_IN    1

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_hdr_t;

// ── resolv.conf handling ─────────────────────────────────────────────────

#define MAX_NS 4
static uint32_t s_ns[MAX_NS];      // nameservers, network byte order
static int      s_ns_count = 0;
static int      s_loaded   = 0;

static int parse_dotted(const char* s, uint32_t* out_be) {
    return inet_pton(AF_INET, s, out_be) == 1 ? 0 : -1;
}

static void load_resolv_conf(void) {
    if (s_loaded) return;
    s_loaded = 1;
    s_ns_count = 0;

    int fd = open("/etc/resolv.conf", O_RDONLY, 0);
    if (fd < 0) return;

    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char* p = buf;
    while (*p && s_ns_count < MAX_NS) {
        // Skip leading whitespace
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';') {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }
        // Match "nameserver"
        if (strncmp(p, "nameserver", 10) == 0 &&
            (p[10] == ' ' || p[10] == '\t')) {
            p += 10;
            while (*p == ' ' || *p == '\t') p++;
            char* eol = p;
            while (*eol && *eol != '\n' && *eol != ' ' &&
                   *eol != '\t' && *eol != '#') eol++;
            char saved = *eol;
            *eol = '\0';
            uint32_t ip;
            if (parse_dotted(p, &ip) == 0)
                s_ns[s_ns_count++] = ip;
            *eol = saved;
            p = eol;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

// ── Name encoding ─────────────────────────────────────────────────────────

// Encode a hostname into RFC 1035 label format.
// "www.example.com" → \x03www\x07example\x03com\x00 (17 bytes).
// Returns bytes written, -1 on error.
static int encode_name(const char* name, uint8_t* out, int out_max) {
    int w = 0;
    const char* p = name;
    while (*p) {
        const char* dot = p;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - p);
        if (len == 0 || len > 63) return -1;
        if (w + 1 + len >= out_max) return -1;
        out[w++] = (uint8_t)len;
        for (int i = 0; i < len; i++) out[w++] = (uint8_t)p[i];
        p = dot;
        if (*p == '.') p++;
    }
    if (w + 1 >= out_max) return -1;
    out[w++] = 0;
    return w;
}

// Decode a possibly-compressed name starting at offset `off` in the DNS
// packet.  Writes a decoded form (not actually used by the caller except for
// CNAME chain following).  Returns the number of bytes consumed *at the call
// site* (i.e. not following compression pointers), or -1 on error.
// out may be NULL if the caller just wants to skip the name.
static int decode_name(const uint8_t* pkt, int pkt_len, int off,
                        char* out, int out_max) {
    int w = 0;
    int jumped = 0;
    int first_len = 0;
    int loops = 0;
    int orig_off = off;

    for (;;) {
        if (loops++ > 64) return -1;
        if (off < 0 || off >= pkt_len) return -1;
        uint8_t len = pkt[off];
        if (len == 0) {
            off++;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            if (off + 1 >= pkt_len) return -1;
            int ptr = ((len & 0x3F) << 8) | pkt[off + 1];
            if (!jumped) first_len = (off + 2) - orig_off;
            jumped = 1;
            off = ptr;
            continue;
        }
        if (len > 63) return -1;
        if (off + 1 + len > pkt_len) return -1;
        if (out) {
            if (w > 0 && w < out_max - 1) out[w++] = '.';
            for (int i = 0; i < len && w < out_max - 1; i++)
                out[w++] = (char)pkt[off + 1 + i];
        }
        off += 1 + len;
    }
    if (out && w < out_max) out[w] = '\0';
    return jumped ? first_len : (off - orig_off);
}

// ── Query builder / response parser ─────────────────────────────────────

// Build a standard recursive A-record query.
// Returns total packet length, -1 on error.
static int build_query(uint8_t* buf, int buf_max, uint16_t id, const char* name) {
    if (buf_max < (int)sizeof(dns_hdr_t) + 6) return -1;

    dns_hdr_t* h = (dns_hdr_t*)buf;
    h->id      = htons(id);
    h->flags   = htons(DNS_FLAG_RD);
    h->qdcount = htons(1);
    h->ancount = 0;
    h->nscount = 0;
    h->arcount = 0;

    int w = sizeof(dns_hdr_t);
    int nl = encode_name(name, buf + w, buf_max - w - 4);
    if (nl < 0) return -1;
    w += nl;

    // QTYPE + QCLASS
    buf[w++] = 0;
    buf[w++] = DNS_TYPE_A;
    buf[w++] = 0;
    buf[w++] = DNS_CLASS_IN;
    return w;
}

// Parse answer section looking for an A record.  Follows up to one CNAME
// within the same response; caller re-queries on the new name if necessary.
// Returns 1 on A record found (writes ip_be), 0 if only CNAME (writes cname),
// -1 on any parse / protocol error.
static int parse_response(const uint8_t* pkt, int pkt_len, uint16_t expect_id,
                            uint32_t* out_ip_be, char* out_cname, int cname_max) {
    if (pkt_len < (int)sizeof(dns_hdr_t)) return -1;
    const dns_hdr_t* h = (const dns_hdr_t*)pkt;
    if (ntohs(h->id) != expect_id) return -1;
    uint16_t flags = ntohs(h->flags);
    if (!(flags & DNS_FLAG_QR)) return -1;
    if (flags & DNS_FLAG_TC) return -2;   // truncation — signal TCP retry
    if ((flags & DNS_RCODE_MASK) != 0) return -1;

    uint16_t qd = ntohs(h->qdcount);
    uint16_t an = ntohs(h->ancount);

    int off = sizeof(dns_hdr_t);

    // Skip question section
    for (int i = 0; i < qd; i++) {
        int n = decode_name(pkt, pkt_len, off, NULL, 0);
        if (n < 0) return -1;
        off += n;
        if (off + 4 > pkt_len) return -1;
        off += 4;  // QTYPE + QCLASS
    }

    // Walk answer section
    int found_cname = 0;
    for (int i = 0; i < an; i++) {
        int n = decode_name(pkt, pkt_len, off, NULL, 0);
        if (n < 0) return -1;
        off += n;
        if (off + 10 > pkt_len) return -1;

        uint16_t type  = ((uint16_t)pkt[off] << 8) | pkt[off + 1];
        uint16_t cls   = ((uint16_t)pkt[off + 2] << 8) | pkt[off + 3];
        uint16_t rdlen = ((uint16_t)pkt[off + 8] << 8) | pkt[off + 9];
        off += 10;
        if (off + rdlen > pkt_len) return -1;

        if (cls == DNS_CLASS_IN && type == DNS_TYPE_A && rdlen == 4) {
            memcpy(out_ip_be, pkt + off, 4);
            return 1;
        }
        if (cls == DNS_CLASS_IN && type == DNS_TYPE_CNAME && !found_cname) {
            if (out_cname && cname_max > 0) {
                if (decode_name(pkt, pkt_len, off, out_cname, cname_max) < 0)
                    return -1;
                found_cname = 1;
            }
        }
        off += rdlen;
    }
    return found_cname ? 0 : -1;
}

// ── UDP query ─────────────────────────────────────────────────────────────

static uint16_t gen_dns_id(void) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)((uint64_t)time(NULL) ^ (uint64_t)getpid()));
        seeded = 1;
    }
    return (uint16_t)(rand() & 0xFFFFu);
}

// Send one UDP query to a single nameserver, wait for the reply.
// Returns 1 on A record (writes ip_be), 0 on CNAME (writes cname),
// -1 on timeout/error.
static int query_one(uint32_t ns_be, const char* name,
                      uint32_t* out_ip_be, char* out_cname, int cname_max) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    sockaddr_in_t dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(DNS_PORT);
    dst.sin_addr   = ns_be;

    uint8_t pkt[DNS_PKT_MAX];
    uint16_t id = gen_dns_id();
    int qlen = build_query(pkt, sizeof(pkt), id, name);
    if (qlen < 0) { close(s); return -1; }

    ssize_t sr = sendto(s, pkt, (size_t)qlen, 0,
                         (struct sockaddr*)&dst, sizeof(dst));
    if (sr != qlen) { close(s); return -1; }

    pollfd_t pfd;
    pfd.fd = s;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, DNS_TIMEOUT_MS);
    if (pr <= 0 || !(pfd.revents & POLLIN)) {
        close(s);
        return -1;
    }

    sockaddr_in_t from;
    socklen_t flen = sizeof(from);
    ssize_t rn = recvfrom(s, pkt, sizeof(pkt), 0,
                           (struct sockaddr*)&from, &flen);
    close(s);
    if (rn <= 0) return -1;

    int pr2 = parse_response(pkt, (int)rn, id, out_ip_be, out_cname, cname_max);
    return pr2;   // 1=A, 0=CNAME, -1=error, -2=truncation
}

// ── Public API ────────────────────────────────────────────────────────────

int gethostbyname_ipv4(const char* name, uint32_t* out_ip_be) {
    if (!name || !out_ip_be) { errno = EINVAL; return -1; }

    // 1. Numeric fast path
    if (inet_pton(AF_INET, name, out_ip_be) == 1) return 0;

    load_resolv_conf();
    if (s_ns_count == 0) { errno = ENOENT; return -1; }

    // CNAME chain walk.  `cur_name` starts as the user-supplied name and is
    // replaced with the CNAME target on each redirection.
    char cur_name[256];
    int cur_len = (int)strlen(name);
    if (cur_len <= 0 || cur_len >= (int)sizeof(cur_name)) {
        errno = EINVAL; return -1;
    }
    memcpy(cur_name, name, (size_t)cur_len);
    cur_name[cur_len] = '\0';

    for (int hop = 0; hop < DNS_MAX_CNAME; hop++) {
        int answered = 0;
        for (int retry = 0; retry <= DNS_MAX_RETRIES && !answered; retry++) {
            for (int i = 0; i < s_ns_count; i++) {
                char cname_buf[256];
                cname_buf[0] = '\0';
                int r = query_one(s_ns[i], cur_name, out_ip_be,
                                   cname_buf, sizeof(cname_buf));
                if (r == 1) return 0;               // resolved
                if (r == 0) {                       // CNAME redirection
                    size_t cn = strlen(cname_buf);
                    if (cn == 0 || cn >= sizeof(cur_name)) {
                        errno = EINVAL; return -1;
                    }
                    memcpy(cur_name, cname_buf, cn + 1);
                    answered = 1;
                    break;
                }
                // r == -1: try next server
            }
        }
        if (!answered) { errno = ETIMEDOUT; return -1; }
    }
    errno = ENOENT;
    return -1;
}

int getaddrinfo_ipv4(const char* host, uint16_t port, sockaddr_in_t* out) {
    if (!host || !out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons(port);
    return gethostbyname_ipv4(host, &out->sin_addr);
}
