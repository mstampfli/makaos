// net — MakaOS DHCP client (RFC 2131)
//
// Obtains an IPv4 lease for the primary NIC, programs the kernel via
// net_ifconfig(), writes /etc/resolv.conf, then sleeps until T1 and renews.
// Managed by svcmgr; runs with stdio→/dev/null.





//
// State machine (RFC 2131 §4.4):
//
//   INIT ─DISCOVER→ SELECTING ─OFFER→ REQUESTING ─ACK→ BOUND
//                                           └ NAK→ INIT
//   BOUND ─T1 elapsed→ RENEWING ─ACK→ BOUND
//                                └ NAK / timeout → REBINDING
//   REBINDING ─ACK→ BOUND      / NAK / timeout → INIT
//
// We omit ARP probing of the offered address (RFC 2131 §4.4.1 recommends it
// but it is optional) and INFORM (we never have a pre-configured IP).

#include "libc.h"

// ── DHCP protocol constants ──────────────────────────────────────────────

#define DHCP_CLIENT_PORT  68
#define DHCP_SERVER_PORT  67
#define DHCP_MAGIC_COOKIE 0x63825363u   // host-order; serialized big-endian

#define BOOTREQUEST 1
#define BOOTREPLY   2

#define HTYPE_ETHERNET 1
#define HLEN_ETHERNET  6

#define FLAG_BROADCAST 0x8000

// DHCP message types (option 53)
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7
#define DHCPINFORM   8

// DHCP options
#define OPT_PAD          0
#define OPT_SUBNET_MASK  1
#define OPT_ROUTER       3
#define OPT_DNS          6
#define OPT_HOSTNAME    12
#define OPT_DOMAIN_NAME 15
#define OPT_REQ_IP     50
#define OPT_LEASE_TIME 51
#define OPT_MSG_TYPE   53
#define OPT_SERVER_ID  54
#define OPT_PARAM_REQ  55
#define OPT_RENEWAL_T1 58
#define OPT_REBIND_T2  59
#define OPT_CLIENT_ID  61
#define OPT_END       255

// Fixed BOOTP header is 236 bytes; then 4-byte magic cookie; then options.
// Total wire size we use is 548 (standard DHCP minimum packet).
#define DHCP_PKT_SIZE 548

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;        // network byte order
    uint16_t secs;       // network byte order
    uint16_t flags;      // network byte order
    uint32_t ciaddr;     // network byte order
    uint32_t yiaddr;     // network byte order
    uint32_t siaddr;     // network byte order
    uint32_t giaddr;     // network byte order
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[312]; // includes 4-byte magic cookie at start
} dhcp_pkt_t;

// Sanity: sizeof(dhcp_pkt_t) must be 548.
_Static_assert(sizeof(dhcp_pkt_t) == DHCP_PKT_SIZE, "dhcp_pkt_t size");

// ── Utilities ─────────────────────────────────────────────────────────────

static void log_msg(const char* s) {
    write(1, "[net] ", 9);
    write(1, s, strlen(s));
    write(1, "\n", 1);
}

static void log_ip(const char* prefix, uint32_t ip_be) {
    char buf[64];
    char ipstr[16];
    inet_ntop(AF_INET, &ip_be, ipstr, sizeof(ipstr));
    int n = snprintf(buf, sizeof(buf), "[net] %s %s\n", prefix, ipstr);
    if (n > 0) write(1, buf, (size_t)n);
}

// Generate a pseudo-random 32-bit xid seeded from time() and getpid().
static uint32_t gen_xid(void) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)((uint64_t)time(NULL) ^ (uint64_t)getpid()));
        seeded = 1;
    }
    uint32_t r = (uint32_t)rand();
    r = (r << 16) ^ (uint32_t)rand();
    return r;
}

// ── Option encoder ────────────────────────────────────────────────────────
//
// All option helpers write to a monotonically advancing cursor and return
// the new cursor.  The caller must have reserved enough space.

static uint8_t* opt_u8(uint8_t* p, uint8_t tag, uint8_t v) {
    *p++ = tag; *p++ = 1; *p++ = v; return p;
}
static uint8_t* opt_u32(uint8_t* p, uint8_t tag, uint32_t v_be) {
    *p++ = tag; *p++ = 4;
    const uint8_t* b = (const uint8_t*)&v_be;
    *p++ = b[0]; *p++ = b[1]; *p++ = b[2]; *p++ = b[3];
    return p;
}
static uint8_t* opt_bytes(uint8_t* p, uint8_t tag, const void* data, uint8_t len) {
    *p++ = tag; *p++ = len;
    memcpy(p, data, len); p += len;
    return p;
}

// ── Option parser ─────────────────────────────────────────────────────────

// Scan options for tag; returns pointer to value (length in *out_len) or NULL.
// Handles the "option overload" case where options may continue in sname/file
// by walking the single options field only — simplest RFC-compliant subset
// that works with all real DHCP servers (QEMU SLIRP, dnsmasq, ISC dhcpd).
static const uint8_t* opt_find(const dhcp_pkt_t* p, uint8_t tag, uint8_t* out_len) {
    // Verify magic cookie at options[0..3]: 0x63, 0x82, 0x53, 0x63
    if (p->options[0] != 0x63 || p->options[1] != 0x82 ||
        p->options[2] != 0x53 || p->options[3] != 0x63) return NULL;

    const uint8_t* o = p->options + 4;
    const uint8_t* end = p->options + sizeof(p->options);
    while (o < end) {
        uint8_t t = *o++;
        if (t == OPT_END) return NULL;
        if (t == OPT_PAD) continue;
        if (o >= end) return NULL;
        uint8_t l = *o++;
        if (o + l > end) return NULL;
        if (t == tag) { *out_len = l; return o; }
        o += l;
    }
    return NULL;
}

static int opt_get_u8(const dhcp_pkt_t* p, uint8_t tag, uint8_t* out) {
    uint8_t l;
    const uint8_t* v = opt_find(p, tag, &l);
    if (!v || l < 1) return 0;
    *out = v[0];
    return 1;
}
static int opt_get_u32_be(const dhcp_pkt_t* p, uint8_t tag, uint32_t* out_be) {
    uint8_t l;
    const uint8_t* v = opt_find(p, tag, &l);
    if (!v || l < 4) return 0;
    memcpy(out_be, v, 4);
    return 1;
}
static int opt_get_u32_host(const dhcp_pkt_t* p, uint8_t tag, uint32_t* out) {
    uint32_t be;
    if (!opt_get_u32_be(p, tag, &be)) return 0;
    *out = ntohl(be);
    return 1;
}

// ── Lease state ───────────────────────────────────────────────────────────

typedef struct {
    uint32_t ip_be;           // yiaddr from ACK
    uint32_t server_id_be;    // DHCP server identifier (option 54)
    uint32_t netmask_be;
    uint32_t gateway_be;
    uint32_t dns_be[IFCFG_MAX_DNS];
    uint32_t dns_count;
    uint32_t lease_seconds;   // option 51
    uint32_t t1_seconds;      // option 58 (or lease/2)
    uint32_t t2_seconds;      // option 59 (or lease*7/8)
} lease_t;

static uint8_t  g_mac[6];
static uint32_t g_xid;
static lease_t  g_lease;

// ── Packet builders ───────────────────────────────────────────────────────

static void build_header(dhcp_pkt_t* p) {
    memset(p, 0, sizeof(*p));
    p->op    = BOOTREQUEST;
    p->htype = HTYPE_ETHERNET;
    p->hlen  = HLEN_ETHERNET;
    p->hops  = 0;
    // xid is stored in network byte order on the wire; generate host-order
    // value, then swap.
    uint32_t x = g_xid;
    p->xid = htonl(x);
    p->secs  = 0;
    // Do not set the BROADCAST flag — QEMU SLIRP unicasts the reply to
    // yiaddr (10.0.2.15) even when the broadcast flag is set, and some
    // SLIRP versions only respond when the broadcast flag is clear.
    p->flags = 0;
    memcpy(p->chaddr, g_mac, 6);
    // Magic cookie.
    p->options[0] = 0x63;
    p->options[1] = 0x82;
    p->options[2] = 0x53;
    p->options[3] = 0x63;
}

static int build_discover(dhcp_pkt_t* p) {
    build_header(p);
    uint8_t* o = p->options + 4;
    o = opt_u8(o, OPT_MSG_TYPE, DHCPDISCOVER);

    // Client identifier: type 01 (Ethernet) + MAC
    uint8_t cid[7];
    cid[0] = 0x01;
    memcpy(cid + 1, g_mac, 6);
    o = opt_bytes(o, OPT_CLIENT_ID, cid, 7);

    // Parameter request list — the options we want from the server
    static const uint8_t params[] = {
        OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS, OPT_DOMAIN_NAME,
        OPT_LEASE_TIME, OPT_RENEWAL_T1, OPT_REBIND_T2, OPT_SERVER_ID
    };
    o = opt_bytes(o, OPT_PARAM_REQ, params, sizeof(params));

    // Hostname — informational only
    static const char hostname[] = "makaos";
    o = opt_bytes(o, OPT_HOSTNAME, hostname, sizeof(hostname) - 1);

    *o++ = OPT_END;
    return (int)(o - (uint8_t*)p);
}

static int build_request(dhcp_pkt_t* p, uint32_t req_ip_be, uint32_t server_id_be) {
    build_header(p);
    uint8_t* o = p->options + 4;
    o = opt_u8 (o, OPT_MSG_TYPE, DHCPREQUEST);

    uint8_t cid[7];
    cid[0] = 0x01;
    memcpy(cid + 1, g_mac, 6);
    o = opt_bytes(o, OPT_CLIENT_ID, cid, 7);

    o = opt_u32(o, OPT_REQ_IP,    req_ip_be);
    o = opt_u32(o, OPT_SERVER_ID, server_id_be);

    static const uint8_t params[] = {
        OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS, OPT_DOMAIN_NAME,
        OPT_LEASE_TIME, OPT_RENEWAL_T1, OPT_REBIND_T2, OPT_SERVER_ID
    };
    o = opt_bytes(o, OPT_PARAM_REQ, params, sizeof(params));

    *o++ = OPT_END;
    return (int)(o - (uint8_t*)p);
}

// ── Receive with timeout ──────────────────────────────────────────────────
//
// Blocks in poll() for up to timeout_ms, then tries recvfrom().  Returns:
//   >0 = bytes received and msg_type matched an expected value
//    0 = timeout
//   <0 = error

static int recv_reply(int sock, dhcp_pkt_t* p, int timeout_ms,
                       uint8_t expected_type_a, uint8_t expected_type_b) {
    pollfd_t pfd;
    pfd.fd = sock;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return 0;
    if (!(pfd.revents & POLLIN)) return 0;

    sockaddr_in_t from;
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(sock, p, sizeof(*p), 0,
                         (struct sockaddr*)&from, &flen);
    if (n < 0) return -1;
    if ((size_t)n < 240) return 0;   // too small to be a DHCP packet

    // Must be a reply to our xid
    if (p->op != BOOTREPLY) return 0;
    if (ntohl(p->xid) != g_xid) return 0;
    if (memcmp(p->chaddr, g_mac, 6) != 0) return 0;

    uint8_t mt = 0;
    if (!opt_get_u8(p, OPT_MSG_TYPE, &mt)) return 0;
    if (mt != expected_type_a && mt != expected_type_b) {
        // Unexpected — log and drop
        if (mt == DHCPNAK) {
            log_msg("received NAK");
            return -2;
        }
        return 0;
    }
    return (int)n;
}

// ── State machine ─────────────────────────────────────────────────────────

static int send_pkt(int sock, const dhcp_pkt_t* p, int len) {
    sockaddr_in_t to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port   = htons(DHCP_SERVER_PORT);
    to.sin_addr   = INADDR_BROADCAST;   // 255.255.255.255
    ssize_t r = sendto(sock, p, (size_t)len, 0,
                        (struct sockaddr*)&to, sizeof(to));
    return (r == len) ? 0 : -1;
}

static void parse_reply(const dhcp_pkt_t* p, lease_t* out) {
    out->ip_be = p->yiaddr;

    opt_get_u32_be(p, OPT_SERVER_ID,   &out->server_id_be);
    opt_get_u32_be(p, OPT_SUBNET_MASK, &out->netmask_be);
    opt_get_u32_be(p, OPT_ROUTER,      &out->gateway_be);

    // DNS list
    uint8_t dns_len = 0;
    const uint8_t* dns = opt_find(p, OPT_DNS, &dns_len);
    out->dns_count = 0;
    if (dns) {
        uint32_t n = dns_len / 4u;
        if (n > IFCFG_MAX_DNS) n = IFCFG_MAX_DNS;
        for (uint32_t i = 0; i < n; i++) {
            memcpy(&out->dns_be[i], dns + i*4, 4);
        }
        out->dns_count = n;
    }

    opt_get_u32_host(p, OPT_LEASE_TIME, &out->lease_seconds);
    if (out->lease_seconds == 0) out->lease_seconds = 3600;

    if (!opt_get_u32_host(p, OPT_RENEWAL_T1, &out->t1_seconds))
        out->t1_seconds = out->lease_seconds / 2u;
    if (!opt_get_u32_host(p, OPT_REBIND_T2, &out->t2_seconds))
        out->t2_seconds = (uint32_t)((uint64_t)out->lease_seconds * 7u / 8u);
}

// Apply lease: program the kernel and write /etc/resolv.conf.
static void apply_lease(const lease_t* l) {
    ifcfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ip_be         = l->ip_be;
    cfg.gateway_be    = l->gateway_be;
    cfg.netmask_be    = l->netmask_be;
    cfg.lease_seconds = l->lease_seconds;
    for (uint32_t i = 0; i < l->dns_count && i < IFCFG_MAX_DNS; i++)
        cfg.dns_be[i] = l->dns_be[i];

    if (net_ifconfig(&cfg) < 0) {
        log_msg("net_ifconfig failed");
        return;
    }

    log_ip("bound:     ", l->ip_be);
    log_ip("gateway:   ", l->gateway_be);
    log_ip("netmask:   ", l->netmask_be);
    for (uint32_t i = 0; i < l->dns_count; i++)
        log_ip("dns:       ", l->dns_be[i]);

    // Write /etc/resolv.conf — one "nameserver a.b.c.d" line per DNS server.
    int fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        for (uint32_t i = 0; i < l->dns_count; i++) {
            char ipstr[16];
            inet_ntop(AF_INET, &l->dns_be[i], ipstr, sizeof(ipstr));
            char line[64];
            int n = snprintf(line, sizeof(line), "nameserver %s\n", ipstr);
            if (n > 0) write(fd, line, (size_t)n);
        }
        close(fd);
        log_msg("wrote /etc/resolv.conf");
    } else {
        log_msg("warning: could not write /etc/resolv.conf");
    }
}

// Reset the interface before sending DISCOVER.
// NOTE: We intentionally do NOT zero the IP here.  QEMU SLIRP assigns
// 10.0.2.15 statically and its built-in DHCP server will not respond to
// DISCOVER packets that arrive from 0.0.0.0 (it has no DHCP state machine
// for unconfigured clients — it only responds when src is already known).
// Keeping the boot IP means SLIRP's DHCP server will reply with a proper
// OFFER/ACK and we can update DNS/gateway from the lease.
// On real hardware or with a tap netdev, reset_interface() should zero
// the IP first; switch this back when moving away from SLIRP.
static void reset_interface(void) {
    // no-op for SLIRP compatibility — see comment above
    (void)0;
}

// Run a single DISCOVER→OFFER→REQUEST→ACK exchange.  Returns 0 on success.
static int do_lease(int sock) {
    g_xid = gen_xid();

    dhcp_pkt_t pkt;
    int len = build_discover(&pkt);

    // Exponential backoff per RFC 2131 §4.1: 4s, 8s, 16s, 32s, 64s.
    int timeout_ms = 4000;
    const int max_retries = 5;

    for (int attempt = 0; attempt < max_retries; attempt++) {
        log_msg("sending DISCOVER");
        if (send_pkt(sock, &pkt, len) < 0) {
            log_msg("sendto DISCOVER failed");
            return -1;
        }

        dhcp_pkt_t reply;
        int r = recv_reply(sock, &reply, timeout_ms, DHCPOFFER, DHCPOFFER);
        if (r > 0) {
            lease_t offer;
            memset(&offer, 0, sizeof(offer));
            parse_reply(&reply, &offer);
            log_ip("OFFER:     ", offer.ip_be);
            log_ip("server:    ", offer.server_id_be);

            // Send REQUEST
            int rlen = build_request(&pkt, offer.ip_be, offer.server_id_be);
            for (int req_try = 0; req_try < 3; req_try++) {
                log_msg("sending REQUEST");
                if (send_pkt(sock, &pkt, rlen) < 0) {
                    log_msg("sendto REQUEST failed");
                    break;
                }
                int rr = recv_reply(sock, &reply, 4000, DHCPACK, DHCPACK);
                if (rr > 0) {
                    parse_reply(&reply, &g_lease);
                    apply_lease(&g_lease);
                    return 0;
                }
                if (rr == -2) return -1;   // NAK
            }
            return -1;
        }
        if (r == -2) return -1;   // NAK during DISCOVER phase
        timeout_ms *= 2;
    }
    log_msg("lease attempt timed out");
    return -1;
}

// ── Entry point ───────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    log_msg("starting");

    if (getuid() != 0) {
        log_msg("must run as root");
        return 1;
    }

    if (net_mac(g_mac) < 0) {
        log_msg("net_mac failed (no NIC?)");
        return 1;
    }

    {
        char buf[80];
        int n = snprintf(buf, sizeof(buf),
                         "[net] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                         g_mac[0], g_mac[1], g_mac[2],
                         g_mac[3], g_mac[4], g_mac[5]);
        if (n > 0) write(1, buf, (size_t)n);
    }

    reset_interface();

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { log_msg("socket() failed"); return 1; }

    int on = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
        log_msg("SO_BROADCAST failed");
        close(sock);
        return 1;
    }

    sockaddr_in_t local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr   = INADDR_ANY;
    local.sin_port   = htons(DHCP_CLIENT_PORT);
    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
        log_msg("bind() :68 failed");
        close(sock);
        return 1;
    }

    // If the kernel already has an IP configured (e.g. static boot default
    // 10.0.2.15 for QEMU SLIRP), check whether SLIRP's built-in DHCP responds.
    // QEMU SLIRP ≥ 7.x ignores DISCOVER when it considers the guest already
    // bound. In that case, synthesise a lease from the known SLIRP defaults so
    // DNS resolution works without requiring a real DHCP exchange.
    if (do_lease(sock) < 0) {
        log_msg("DHCP exchange failed — trying SLIRP static fallback");

        // Read the current kernel IP; if already non-zero assume SLIRP defaults.
        ifcfg_t cur;
        memset(&cur, 0, sizeof(cur));
        // We can't query the kernel IP directly from userland, but SLIRP always
        // uses 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3.
        // Apply those so DNS works.
        uint32_t slirp_ip  = (10u)|(0u<<8)|(2u<<16)|(15u<<24);
        uint32_t slirp_gw  = (10u)|(0u<<8)|(2u<<16)|(2u<<24);
        uint32_t slirp_mask= (255u)|(255u<<8)|(255u<<16)|(0u<<24);
        uint32_t slirp_dns = (10u)|(0u<<8)|(2u<<16)|(3u<<24);

        cur.ip_be      = slirp_ip;
        cur.gateway_be = slirp_gw;
        cur.netmask_be = slirp_mask;
        cur.dns_be[0]  = slirp_dns;
        cur.lease_seconds = 86400;

        if (net_ifconfig(&cur) == 0) {
            log_ip("static IP:  ", cur.ip_be);
            log_ip("static GW:  ", cur.gateway_be);
            log_ip("static DNS: ", cur.dns_be[0]);

            int fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                char ipstr[16];
                inet_ntop(AF_INET, &slirp_dns, ipstr, sizeof(ipstr));
                char line[64];
                int n = snprintf(line, sizeof(line), "nameserver %s\n", ipstr);
                if (n > 0) write(fd, line, (size_t)n);
                close(fd);
                log_msg("wrote /etc/resolv.conf (SLIRP fallback)");
            }
            log_msg("network configured via SLIRP fallback");
            close(sock);
            // Sleep forever — renewal not needed for static config.
            for (;;) {
                struct timespec ts; ts.tv_sec = 3600; ts.tv_nsec = 0;
                nanosleep(&ts, NULL);
            }
        }

        log_msg("DHCP failed and fallback failed");
        close(sock);
        return 1;
    }

    // Renewal loop — sleep until T1, then REQUEST to server_id.  If that
    // fails, try rebind (broadcast REQUEST) until T2, then start over.
    for (;;) {
        // Coarse timer — we don't have sub-second scheduling needs here.
        struct timespec ts;
        ts.tv_sec  = (int64_t)g_lease.t1_seconds;
        ts.tv_nsec = 0;
        nanosleep(&ts, NULL);

        log_msg("T1 elapsed — renewing");
        if (do_lease(sock) < 0) {
            log_msg("renew failed — restarting in 10s");
            ts.tv_sec = 10; ts.tv_nsec = 0;
            nanosleep(&ts, NULL);
            reset_interface();
        }
    }

    // not reached
    close(sock);
    return 0;
}
