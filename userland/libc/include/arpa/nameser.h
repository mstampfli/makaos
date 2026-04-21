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

#endif
