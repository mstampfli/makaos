// netinet/ip_icmp.h - ICMPv4 message format (Linux-compatible subset).
#ifndef _NETINET_IP_ICMP_H
#define _NETINET_IP_ICMP_H 1
#include <stdint.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>

struct icmp {
    uint8_t  icmp_type;
    uint8_t  icmp_code;
    uint16_t icmp_cksum;
    union {
        uint8_t  ih_pptr;
        struct in_addr ih_gwaddr;
        struct ih_idseq { uint16_t icd_id; uint16_t icd_seq; } ih_idseq;
        uint32_t ih_void;
        struct ih_pmtu { uint16_t ipm_void; uint16_t ipm_nextmtu; } ih_pmtu;
    } icmp_hun;
#define icmp_pptr   icmp_hun.ih_pptr
#define icmp_gwaddr icmp_hun.ih_gwaddr
#define icmp_id     icmp_hun.ih_idseq.icd_id
#define icmp_seq    icmp_hun.ih_idseq.icd_seq
#define icmp_void   icmp_hun.ih_void
#define icmp_nextmtu icmp_hun.ih_pmtu.ipm_nextmtu
    union {
        struct { n_time its_otime, its_rtime, its_ttime; } id_ts;
        uint32_t id_mask;
        uint8_t  id_data[1];
    } icmp_dun;
#define icmp_otime icmp_dun.id_ts.its_otime
#define icmp_rtime icmp_dun.id_ts.its_rtime
#define icmp_ttime icmp_dun.id_ts.its_ttime
#define icmp_mask  icmp_dun.id_mask
#define icmp_data  icmp_dun.id_data
};

#define ICMP_ECHOREPLY 0
#define ICMP_ECHO      8
#define ICMP_MINLEN    8
#endif
