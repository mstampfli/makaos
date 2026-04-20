#ifndef _MAKAOS_SYS_WAIT_H
#define _MAKAOS_SYS_WAIT_H 1

#include <sys/types.h>

#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

// status decode — exit_code in low byte, signal in next byte
#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)

pid_t wait(int* status);
pid_t waitpid(pid_t pid, int* status, int options);

#endif
