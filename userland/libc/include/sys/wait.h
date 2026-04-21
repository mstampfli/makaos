#ifndef _MAKAOS_SYS_WAIT_H
#define _MAKAOS_SYS_WAIT_H 1

#include <sys/types.h>
#include <signal.h>   // siginfo_t

#define WNOHANG    1
#define WUNTRACED  2
#define WSTOPPED   WUNTRACED
#define WEXITED    4
#define WCONTINUED 8
#define WNOWAIT    0x01000000

// status decode — exit_code in low byte, signal in next byte
#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WIFCONTINUED(s) ((s) == 0xffff)

// waitid() id types
typedef enum {
    P_ALL  = 0,
    P_PID  = 1,
    P_PGID = 2,
} idtype_t;

// si_code values for SIGCHLD — waitid reports these in siginfo_t
#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_TRAPPED   4
#define CLD_STOPPED   5
#define CLD_CONTINUED 6

pid_t wait(int* status);
pid_t waitpid(pid_t pid, int* status, int options);
int   waitid(idtype_t idtype, id_t id, siginfo_t* info, int options);

#endif
