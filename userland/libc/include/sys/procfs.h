#ifndef _SYS_PROCFS_H
#define _SYS_PROCFS_H

// Minimal <sys/procfs.h> for MakaOS.  MakaOS has no /proc core-file structures.
// This exists so code that merely includes the header (e.g. OpenJDK's
// jdk.management OperatingSystemImpl.c, which includes it but references none of
// its types) compiles.  No prpsinfo/prstatus/elf_gregset_t are declared because
// nothing on MakaOS uses them.

#include <sys/types.h>

#endif /* _SYS_PROCFS_H */
