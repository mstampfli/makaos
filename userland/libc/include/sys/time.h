#ifndef _MAKAOS_SYS_TIME_H
#define _MAKAOS_SYS_TIME_H 1

#include <sys/select.h>
#include <time.h>

int gettimeofday(struct timeval* tv, void* tz);

#endif
