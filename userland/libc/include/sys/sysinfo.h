// sys/sysinfo.h - system statistics (Linux-compatible layout).
//
// struct sysinfo matches the Linux ABI field order so portable code (e.g.
// OpenJDK's os_linux.cpp, which reads si.freeram * si.mem_unit) works
// unchanged. Values are backed by sysconf(); see sys_sysinfo.c.

#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sysinfo {
    long           uptime;              /* seconds since boot */
    unsigned long  loads[3];            /* 1/5/15 min load averages */
    unsigned long  totalram;            /* total usable main memory (mem_unit) */
    unsigned long  freeram;             /* available memory (mem_unit) */
    unsigned long  sharedram;
    unsigned long  bufferram;
    unsigned long  totalswap;
    unsigned long  freeswap;
    unsigned short procs;               /* number of current processes */
    unsigned short pad;
    unsigned long  totalhigh;
    unsigned long  freehigh;
    unsigned int   mem_unit;            /* size of a memory unit in bytes */
    char           _f[20 - 2 * sizeof(long) - sizeof(int)]; /* pad to 64 bytes */
};

int sysinfo(struct sysinfo* info);
int get_nprocs(void);
int get_nprocs_conf(void);

#ifdef __cplusplus
}
#endif

#endif /* sys/sysinfo.h */
