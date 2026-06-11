#ifndef _MAKAOS_ARPA_NAMESER_H
#define _MAKAOS_ARPA_NAMESER_H 1

// DNS resolver constants.  Sourced from BIND's RFC1035 mapping —
// gio uses these for parsing /etc/resolv.conf / making DNS queries.

// Classes (RFC 1035 §3.2.4)
#define C_IN    1   // Internet
#define C_CS    2   // CSNET (obsolete)
#define C_CH    3   // Chaos
#define C_HS    4   // Hesiod
#define C_ANY 255

// Record types
#define T_A        1
#define T_NS       2
#define T_CNAME    5
#define T_SOA      6
#define T_PTR     12
#define T_MX      15
#define T_TXT     16
#define T_AAAA    28
#define T_SRV     33
#define T_NAPTR   35
#define T_ANY    255

// Limits (RFC 1035 §2.3.4)
#define NS_PACKETSZ    512
#define NS_MAXDNAME   1025
#define NS_MAXLABEL     63
#define NS_HFIXEDSZ     12
#define NS_QFIXEDSZ      4
#define NS_RRFIXEDSZ    10
#define NS_INT32SZ       4
#define NS_INT16SZ       2

// ── BIND4 compat layer (arpa/nameser_compat.h equivalent) ───────────
// gio's gthreadedresolver.c parses res_query answers with the classic
// HEADER struct + GETSHORT/GETLONG cursor macros + dn_expand.  Our
// res_query always fails (no kernel DNS), so these parse paths never
// see live data — but they must compile.

typedef struct {
    unsigned id      :16;
    unsigned rd      :1;
    unsigned tc      :1;
    unsigned aa      :1;
    unsigned opcode  :4;
    unsigned qr      :1;
    unsigned rcode   :4;
    unsigned cd      :1;
    unsigned ad      :1;
    unsigned unused  :1;
    unsigned ra      :1;
    unsigned qdcount :16;
    unsigned ancount :16;
    unsigned nscount :16;
    unsigned arcount :16;
} HEADER;

#define GETSHORT(s, cp) do { \
    const unsigned char* t_cp = (const unsigned char*)(cp); \
    (s) = ((unsigned short)t_cp[0] << 8) | (unsigned short)t_cp[1]; \
    (cp) += NS_INT16SZ; \
} while (0)

#define GETLONG(l, cp) do { \
    const unsigned char* t_cp = (const unsigned char*)(cp); \
    (l) = ((unsigned long)t_cp[0] << 24) | ((unsigned long)t_cp[1] << 16) \
        | ((unsigned long)t_cp[2] << 8)  |  (unsigned long)t_cp[3]; \
    (cp) += NS_INT32SZ; \
} while (0)

#endif
