// sys/times.h - process times (POSIX).

#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H 1

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tms {
    clock_t tms_utime;   /* user CPU time */
    clock_t tms_stime;   /* system CPU time */
    clock_t tms_cutime;  /* user CPU time of waited-for children */
    clock_t tms_cstime;  /* system CPU time of waited-for children */
};

clock_t times(struct tms* buf);

#ifdef __cplusplus
}
#endif

#endif /* sys/times.h */
