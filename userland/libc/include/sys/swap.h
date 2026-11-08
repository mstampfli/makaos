#ifndef _SYS_SWAP_H
#define _SYS_SWAP_H

// Minimal <sys/swap.h> for MakaOS.  MakaOS has no swap; this exists so code that
// includes it (e.g. OpenJDK's jdk.management OperatingSystemImpl.c, which reads
// swap totals from sysinfo() and only needs the header to be present) compiles.
// The swapon/swapoff prototypes and SWAP_FLAG_* constants match the Linux ABI;
// the calls are unimplemented (no swap device) at the libc layer.

#define SWAP_FLAG_PREFER      0x8000
#define SWAP_FLAG_PRIO_MASK   0x7fff
#define SWAP_FLAG_PRIO_SHIFT  0
#define SWAP_FLAG_DISCARD     0x10000

#ifdef __cplusplus
extern "C" {
#endif

int swapon(const char *path, int swapflags);
int swapoff(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SWAP_H */
