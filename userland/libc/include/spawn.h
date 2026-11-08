// spawn.h - POSIX process spawning (posix_spawn family).
//
// Implemented over fork + file-action replay + execve (see spawn.c), which is
// the portable fallback and is exactly what a subprocess launcher (e.g.
// OpenJDK's ProcessImpl) needs.

#ifndef _SPAWN_H
#define _SPAWN_H 1

#include <sys/types.h>
#include <signal.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   __used;
    int   __allocated;
    void* __actions;      // opaque array of file actions
} posix_spawn_file_actions_t;

typedef struct {
    short              __flags;
    pid_t              __pgrp;
    sigset_t           __sd;      // signals to reset to SIG_DFL
    sigset_t           __ss;      // signal mask to set
    struct sched_param __sp;
    int                __policy;
} posix_spawnattr_t;

#define POSIX_SPAWN_RESETIDS      0x01
#define POSIX_SPAWN_SETPGROUP     0x02
#define POSIX_SPAWN_SETSIGDEF     0x04
#define POSIX_SPAWN_SETSIGMASK    0x08
#define POSIX_SPAWN_SETSCHEDPARAM 0x10
#define POSIX_SPAWN_SETSCHEDULER  0x20
#define POSIX_SPAWN_USEVFORK      0x40
#define POSIX_SPAWN_SETSID        0x80

int posix_spawn(pid_t* pid, const char* path,
                const posix_spawn_file_actions_t* file_actions,
                const posix_spawnattr_t* attrp,
                char* const argv[], char* const envp[]);
int posix_spawnp(pid_t* pid, const char* file,
                 const posix_spawn_file_actions_t* file_actions,
                 const posix_spawnattr_t* attrp,
                 char* const argv[], char* const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t* fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* fa, int fildes,
                                     const char* path, int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* fa, int fildes);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* fa, int fildes,
                                     int newfildes);

int posix_spawnattr_init(posix_spawnattr_t* attr);
int posix_spawnattr_destroy(posix_spawnattr_t* attr);
int posix_spawnattr_setflags(posix_spawnattr_t* attr, short flags);
int posix_spawnattr_getflags(const posix_spawnattr_t* attr, short* flags);
int posix_spawnattr_setpgroup(posix_spawnattr_t* attr, pid_t pgroup);
int posix_spawnattr_getpgroup(const posix_spawnattr_t* attr, pid_t* pgroup);
int posix_spawnattr_setsigmask(posix_spawnattr_t* attr, const sigset_t* sigmask);
int posix_spawnattr_setsigdefault(posix_spawnattr_t* attr, const sigset_t* sigdefault);
int posix_spawnattr_setschedparam(posix_spawnattr_t* attr, const struct sched_param* sp);
int posix_spawnattr_setschedpolicy(posix_spawnattr_t* attr, int policy);

#ifdef __cplusplus
}
#endif

#endif /* spawn.h */
