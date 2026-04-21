#ifndef _MAKAOS_SYS_FILE_H
#define _MAKAOS_SYS_FILE_H 1

// BSD advisory file locking — used by fontconfig's cache-write path
// and libdrm's magic-file logic.  Single-user MakaOS has no contention,
// so the real implementation is a no-op returning success.

#define LOCK_SH   1   // shared lock
#define LOCK_EX   2   // exclusive lock
#define LOCK_UN   8   // unlock
#define LOCK_NB   4   // non-blocking

int flock(int fd, int operation);

#endif
