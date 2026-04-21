#ifndef _MAKAOS_SIGNAL_H
#define _MAKAOS_SIGNAL_H 1

#include <sys/types.h>

// Signal numbers — match kernel.
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    SIGABRT
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31

#define NSIG     32

// sig_handler_t special values
#define SIG_DFL  ((void (*)(int)) 0)
#define SIG_IGN  ((void (*)(int)) 1)
#define SIG_ERR  ((void (*)(int)) -1)

// sigprocmask how
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// sigaction flags
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

typedef unsigned long sigset_t;

// Forward-declared so struct sigaction can reference it.
typedef struct siginfo siginfo_t;

// POSIX sigaction: sa_handler and sa_sigaction alias — callers pick
// based on SA_SIGINFO in sa_flags.  Anonymous union is the canonical
// layout shared with glibc/musl.
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t*, void*);
    };
    sigset_t   sa_mask;
    int        sa_flags;
    void     (*sa_restorer)(void);
};

struct siginfo {
    int    si_signo;
    int    si_errno;
    int    si_code;
    pid_t  si_pid;
    uid_t  si_uid;
    int    si_status;
    void*  si_addr;
    int    si_value;
};

int kill(pid_t pid, int sig);
int raise(int sig);
void (*signal(int sig, void (*handler)(int)))(int);
int sigaction(int sig, const struct sigaction* act, struct sigaction* old);
int sigprocmask(int how, const sigset_t* set, sigset_t* old);
int sigpending(sigset_t* set);
int sigsuspend(const sigset_t* mask);

int sigemptyset(sigset_t* set);
int sigfillset(sigset_t* set);
int sigaddset(sigset_t* set, int sig);
int sigdelset(sigset_t* set, int sig);
int sigismember(const sigset_t* set, int sig);

#endif
