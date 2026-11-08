// sys/prctl.h - process/thread control (Linux-compatible subset).
//
// MakaOS implements only the options portable code actually relies on; the
// rest report failure so callers fall back gracefully (see sys_prctl.c).

#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H 1

#ifdef __cplusplus
extern "C" {
#endif

#define PR_SET_NAME       15
#define PR_GET_NAME       16
#define PR_SET_TIMERSLACK 29
#define PR_GET_TIMERSLACK 30

int prctl(int option, ...);

#ifdef __cplusplus
}
#endif

#endif /* sys/prctl.h */
