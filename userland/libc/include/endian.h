// endian.h -- byte-order macros and conversion helpers.
// MakaOS targets little-endian x86_64 only.  Layout mirrors glibc so software
// that probes <endian.h> (Mesa's u_endian.h, many others) works unchanged.
#ifndef _ENDIAN_H
#define _ENDIAN_H

#include <stdint.h>

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412
#define __BYTE_ORDER    __LITTLE_ENDIAN

// BSD-style non-underscore aliases (expected by a lot of portable code).
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#define PDP_ENDIAN    __PDP_ENDIAN
#define BYTE_ORDER    __BYTE_ORDER

#define __bswap_16(x) __builtin_bswap16(x)
#define __bswap_32(x) __builtin_bswap32(x)
#define __bswap_64(x) __builtin_bswap64(x)

// Little-endian host: host order == little order; big is the swap.
#define htobe16(x) __builtin_bswap16(x)
#define htole16(x) ((uint16_t)(x))
#define be16toh(x) __builtin_bswap16(x)
#define le16toh(x) ((uint16_t)(x))

#define htobe32(x) __builtin_bswap32(x)
#define htole32(x) ((uint32_t)(x))
#define be32toh(x) __builtin_bswap32(x)
#define le32toh(x) ((uint32_t)(x))

#define htobe64(x) __builtin_bswap64(x)
#define htole64(x) ((uint64_t)(x))
#define be64toh(x) __builtin_bswap64(x)
#define le64toh(x) ((uint64_t)(x))

#endif // _ENDIAN_H
