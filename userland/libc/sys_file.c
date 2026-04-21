// ── sys_file.c — <sys/file.h> flock (advisory file lock) ────────────
//
// BSD advisory locking.  MakaOS is single-user and has no contention
// on fontconfig cache / libdrm magic files, so flock is a no-op that
// reports success.  When multi-user / multi-process hardening matters
// we can back this with SYS_FLOCK; callers will continue to work.

#include <sys/file.h>

// TODO(scalability-debt-ledger-#5): SYS_FLOCK → inode-keyed lock table.
// Required before sqlite/lmdb/gdbm work correctly under concurrent writers.
int flock(int fd, int operation) {
    (void)fd; (void)operation;
    return 0;
}
