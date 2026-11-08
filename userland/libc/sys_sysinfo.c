// sys_sysinfo.c - <sys/sysinfo.h> (Linux-compatible).
//
// Backed by sysconf(): memory sizes come from _SC_PHYS_PAGES/_SC_AVPHYS_PAGES
// (which MakaOS reports in page units) and the CPU count from
// _SC_NPROCESSORS_*. totalram/freeram are expressed in mem_unit == page-size
// units, matching the Linux ABI, so callers computing bytes as
// `freeram * mem_unit` get the right value.

#include <sys/sysinfo.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int get_nprocs_conf(void) {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    return n > 0 ? (int)n : 1;
}

int get_nprocs(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

int sysinfo(struct sysinfo* info) {
    if (!info) { errno = EFAULT; return -1; }
    memset(info, 0, sizeof(*info));

    long pagesz = sysconf(_SC_PAGESIZE);
    long total  = sysconf(_SC_PHYS_PAGES);
    long avail  = sysconf(_SC_AVPHYS_PAGES);
    if (pagesz <= 0) pagesz = 4096;
    if (total  <  0) total  = 0;
    if (avail  <  0) avail  = total;

    info->mem_unit = (unsigned int)pagesz;
    info->totalram = (unsigned long)total;   /* in mem_unit units */
    info->freeram  = (unsigned long)avail;
    info->procs    = (unsigned short)get_nprocs();
    return 0;
}
