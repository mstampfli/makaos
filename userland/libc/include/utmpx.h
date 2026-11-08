// utmpx.h - user accounting database (POSIX, Linux-compatible subset).
//
// MakaOS keeps no utmp database, so the iteration functions report an empty
// database (see utmpx.c). Consumers that scan it (e.g. OpenJDK looking for the
// "system boot" record to print OS uptime) then find nothing and skip that bit.

#ifndef _UTMPX_H
#define _UTMPX_H 1

#include <sys/types.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UT_LINESIZE  32
#define UT_NAMESIZE  32
#define UT_HOSTSIZE  256

// ut_type values.
#define EMPTY         0
#define RUN_LVL       1
#define BOOT_TIME     2
#define NEW_TIME      3
#define OLD_TIME      4
#define INIT_PROCESS  5
#define LOGIN_PROCESS 6
#define USER_PROCESS  7
#define DEAD_PROCESS  8

struct utmpx {
    short          ut_type;
    pid_t          ut_pid;
    char           ut_line[UT_LINESIZE];
    char           ut_id[4];
    char           ut_user[UT_NAMESIZE];
    char           ut_host[UT_HOSTSIZE];
    struct { short e_termination; short e_exit; } ut_exit;
    long           ut_session;
    struct timeval ut_tv;
    int            ut_addr_v6[4];
    char           __glibc_reserved[20];
};

void          setutxent(void);
void          endutxent(void);
struct utmpx* getutxent(void);

#ifdef __cplusplus
}
#endif

#endif /* utmpx.h */
