#ifndef _MAKAOS_TIME_H
#define _MAKAOS_TIME_H 1

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#define CLOCKS_PER_SEC 1000000L

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char* tm_zone;
};

time_t     time(time_t* tloc);
clock_t    clock(void);
int        nanosleep(const struct timespec* req, struct timespec* rem);
unsigned   sleep(unsigned seconds);

// clockid_t + clock_gettime — POSIX monotonic time.  Backs on to
// SYS_CLOCK_NS for all clock ids (MakaOS has a single system clock).
typedef int clockid_t;
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_BOOTTIME           7

int clock_gettime(clockid_t id, struct timespec* ts);
int clock_getres(clockid_t id, struct timespec* res);

struct tm* gmtime(const time_t* t);
struct tm* localtime(const time_t* t);
time_t     mktime(struct tm* tm);
size_t     strftime(char* s, size_t max, const char* fmt, const struct tm* tm);
char*      ctime(const time_t* t);
char*      asctime(const struct tm* tm);
double     difftime(time_t a, time_t b);

#ifdef __cplusplus
}
#endif

#endif
