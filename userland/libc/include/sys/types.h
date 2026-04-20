#ifndef _MAKAOS_SYS_TYPES_H
#define _MAKAOS_SYS_TYPES_H 1

#include <stdint.h>
#include <stddef.h>

typedef int32_t  pid_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint32_t mode_t;
typedef int64_t  off_t;
typedef int64_t  blkcnt_t;
typedef int64_t  blksize_t;
typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t nlink_t;
typedef uint32_t id_t;
typedef int64_t  time_t;
typedef int64_t  clock_t;
typedef int64_t  suseconds_t;
typedef uint32_t socklen_t;
typedef uint64_t fsblkcnt_t;
typedef uint64_t fsfilcnt_t;
typedef int      sig_atomic_t;

#endif
