// MakaOS <sys/sysmacros.h> — dev_t decomposition macros.  Linux splits
// dev_t into a 12-bit major + 20-bit minor interleaved in a u32.  We
// use the same layout so ported code works against our stat().

#pragma once
#include <sys/types.h>      // dev_t lives here (uint64_t)

// Linux-compatible 12-bit-major / 20-bit-minor split, though the
// actual numeric encoding doesn't matter on MakaOS — nothing reads
// raw dev_t values yet.  Callers (libdrm, libudev-zero) use these
// macros to format strings and compare against stat() output.
#define major(dev)  (((unsigned)((dev) >> 8))  & 0xFFFU)
#define minor(dev)  (((unsigned)(dev))         & 0xFFU)
#define makedev(ma, mi)  ((dev_t)(((unsigned long)(ma) & 0xFFFU) << 8 | ((unsigned long)(mi) & 0xFFU)))
