// byteswap.h - glibc-compatible byte-swap macros.
//
// Provides bswap_16/32/64 as used by portable code (e.g. OpenJDK's
// bytes_linux_zero.hpp). Backed by the compiler builtins so they compile to a
// single bswap/movbe on x86-64.

#ifndef _BYTESWAP_H
#define _BYTESWAP_H 1

#include <stdint.h>

#define bswap_16(x) __builtin_bswap16((uint16_t)(x))
#define bswap_32(x) __builtin_bswap32((uint32_t)(x))
#define bswap_64(x) __builtin_bswap64((uint64_t)(x))

#endif /* byteswap.h */
