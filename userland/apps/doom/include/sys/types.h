#pragma once
// sys/types.h shim
#include "../../../../libc/libc.h"
typedef int64_t  off_t;
typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t nlink_t;
#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef uint32_t mode_t;
#endif
#ifndef _UID_T_DEFINED
#define _UID_T_DEFINED
typedef uint32_t uid_t;
#endif
#ifndef _GID_T_DEFINED
#define _GID_T_DEFINED
typedef uint32_t gid_t;
#endif
#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int64_t  pid_t;
#endif
