#ifndef _MAKAOS_SYS_SELECT_H
#define _MAKAOS_SYS_SELECT_H 1

#include <sys/types.h>
#include <time.h>
#include <signal.h>

#define FD_SETSIZE 1024

typedef struct {
    unsigned long __bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define __FDS_BITS(set)    ((set)->__bits)
#define __FD_ELT(d)        ((d) / (8 * sizeof(unsigned long)))
#define __FD_MASK(d)       (1UL << ((d) % (8 * sizeof(unsigned long))))

#define FD_ZERO(s)        do { \
    for (int __i = 0; __i < (int)(sizeof(*(s))/sizeof(unsigned long)); __i++) \
        __FDS_BITS(s)[__i] = 0; } while (0)
#define FD_SET(d, s)   (__FDS_BITS(s)[__FD_ELT(d)] |=  __FD_MASK(d))
#define FD_CLR(d, s)   (__FDS_BITS(s)[__FD_ELT(d)] &= ~__FD_MASK(d))
#define FD_ISSET(d, s) ((__FDS_BITS(s)[__FD_ELT(d)] &   __FD_MASK(d)) != 0)

struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};

int select(int nfds, fd_set* rd, fd_set* wr, fd_set* er, struct timeval* tv);
int pselect(int nfds, fd_set* rd, fd_set* wr, fd_set* er,
             const struct timespec* tv, const sigset_t* mask);

#endif
