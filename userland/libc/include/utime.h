// ── utime.h — file access/modification time update ──────────────────
// Port-surface header (glib's gstdio.c includes it unconditionally).
// MakaOS ext2 does not yet expose a timestamp-update syscall; utime()
// fails with ENOSYS, which every consumer we ship treats as a soft
// error ("could not preserve mtime").

#ifndef _MAKAOS_UTIME_H
#define _MAKAOS_UTIME_H 1

#include <sys/types.h>

struct utimbuf {
    time_t actime;   // access time
    time_t modtime;  // modification time
};

int utime(const char* path, const struct utimbuf* times);

#endif
