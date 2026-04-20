#ifndef _MAKAOS_SYS_TIMERFD_H
#define _MAKAOS_SYS_TIMERFD_H 1

#include <time.h>

// Linux-compatible timerfd flags.
#define TFD_CLOEXEC       0x0002
#define TFD_NONBLOCK      0x0004
#define TFD_TIMER_ABSTIME 0x0001

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags,
                    const struct itimerspec* new_value,
                    struct itimerspec* old_value);
int timerfd_gettime(int fd, struct itimerspec* curr_value);

#endif
