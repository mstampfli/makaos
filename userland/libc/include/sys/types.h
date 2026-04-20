#ifndef _MAKAOS_SYS_TYPES_H
#define _MAKAOS_SYS_TYPES_H 1

#include <stdint.h>
#include <stddef.h>

typedef int32_t  pid_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint32_t mode_t;
#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED 1
typedef long long off_t;
#endif
typedef long long blkcnt_t;
typedef long long blksize_t;
typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t nlink_t;
typedef uint32_t id_t;
typedef long long time_t;
typedef long long clock_t;
typedef long long suseconds_t;
typedef uint32_t socklen_t;
typedef uint64_t fsblkcnt_t;
typedef uint64_t fsfilcnt_t;
typedef int      sig_atomic_t;
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED 1
typedef long ssize_t;
#endif

#endif
