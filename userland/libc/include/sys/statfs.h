#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H
// glibc's <sys/statfs.h> exposes struct statfs + statfs()/fstatfs(); on MakaOS
// those live in <sys/vfs.h>. Provided so code that includes <sys/statfs.h>
// (e.g. OpenJDK's ZGC zPhysicalMemoryBacking_linux.cpp) compiles.
#include <sys/vfs.h>
#endif
