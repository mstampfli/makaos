// netinet/in_systm.h - network byte-order integer types (BSD).
#ifndef _NETINET_IN_SYSTM_H
#define _NETINET_IN_SYSTM_H 1
#include <stdint.h>
typedef uint16_t n_short;   // short as received from the net
typedef uint32_t n_long;    // long as received from the net
typedef uint32_t n_time;    // ms since 00:00 GMT
#endif
