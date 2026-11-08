// spawn.c - <spawn.h> (posix_spawn family) over fork + execve.
//
// The child replays the recorded file actions (open/close/dup2) and applies
// the requested spawn attributes, then execs. This is the portable fallback
// implementation and matches what subprocess launchers (e.g. OpenJDK's
// ProcessImpl) rely on.

#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

extern char** environ;

enum { ACT_OPEN, ACT_CLOSE, ACT_DUP2 };

struct __spawn_action {
    int         tag;
    int         fd;
    int         newfd;   // dup2
    const char* path;    // open
    int         oflag;   // open
    mode_t      mode;    // open
};

int posix_spawn_file_actions_init(posix_spawn_file_actions_t* fa) {
    fa->__used = 0; fa->__allocated = 0; fa->__actions = NULL;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* fa) {
    free(fa->__actions);
    fa->__actions = NULL; fa->__used = fa->__allocated = 0;
    return 0;
}

static struct __spawn_action* spawn_add(posix_spawn_file_actions_t* fa) {
    if (fa->__used == fa->__allocated) {
        int n = fa->__allocated ? fa->__allocated * 2 : 8;
        void* p = realloc(fa->__actions, (size_t)n * sizeof(struct __spawn_action));
        if (!p) return NULL;
        fa->__actions = p; fa->__allocated = n;
    }
    return &((struct __spawn_action*)fa->__actions)[fa->__used++];
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* fa, int fd,
                                     const char* path, int oflag, mode_t mode) {
    struct __spawn_action* a = spawn_add(fa);
    if (!a) return ENOMEM;
    a->tag = ACT_OPEN; a->fd = fd; a->path = path; a->oflag = oflag; a->mode = mode;
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* fa, int fd) {
    struct __spawn_action* a = spawn_add(fa);
    if (!a) return ENOMEM;
    a->tag = ACT_CLOSE; a->fd = fd;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* fa, int fd, int newfd) {
    struct __spawn_action* a = spawn_add(fa);
    if (!a) return ENOMEM;
    a->tag = ACT_DUP2; a->fd = fd; a->newfd = newfd;
    return 0;
}

int posix_spawnattr_init(posix_spawnattr_t* a) { memset(a, 0, sizeof(*a)); return 0; }
int posix_spawnattr_destroy(posix_spawnattr_t* a) { (void)a; return 0; }
int posix_spawnattr_setflags(posix_spawnattr_t* a, short f) { a->__flags = f; return 0; }
int posix_spawnattr_getflags(const posix_spawnattr_t* a, short* f) { *f = a->__flags; return 0; }
int posix_spawnattr_setpgroup(posix_spawnattr_t* a, pid_t p) { a->__pgrp = p; return 0; }
int posix_spawnattr_getpgroup(const posix_spawnattr_t* a, pid_t* p) { *p = a->__pgrp; return 0; }
int posix_spawnattr_setsigmask(posix_spawnattr_t* a, const sigset_t* m) { a->__ss = *m; return 0; }
int posix_spawnattr_setsigdefault(posix_spawnattr_t* a, const sigset_t* d) { a->__sd = *d; return 0; }
int posix_spawnattr_setschedparam(posix_spawnattr_t* a, const struct sched_param* sp) { a->__sp = *sp; return 0; }
int posix_spawnattr_setschedpolicy(posix_spawnattr_t* a, int pol) { a->__policy = pol; return 0; }

static int spawn_common(pid_t* pidp, const char* path, int usepath,
                        const posix_spawn_file_actions_t* fa,
                        const posix_spawnattr_t* attr,
                        char* const argv[], char* const envp[]) {
    pid_t pid = fork();
    if (pid < 0)
        return errno;

    if (pid == 0) {
        // Child: apply spawn attributes first.
        if (attr) {
            short fl = attr->__flags;
            if (fl & POSIX_SPAWN_SETSID)    setsid();
            if (fl & POSIX_SPAWN_SETPGROUP) setpgid(0, attr->__pgrp);
            if (fl & POSIX_SPAWN_RESETIDS)  { setgid(getgid()); setuid(getuid()); }
            if (fl & POSIX_SPAWN_SETSIGMASK)
                sigprocmask(SIG_SETMASK, &attr->__ss, NULL);
            if (fl & POSIX_SPAWN_SETSIGDEF) {
                struct sigaction sa;
                memset(&sa, 0, sizeof(sa));
                sa.sa_handler = SIG_DFL;
                for (int s = 1; s < NSIG; s++)
                    if (sigismember(&attr->__sd, s))
                        sigaction(s, &sa, NULL);
            }
            // SETSCHEDPARAM/SETSCHEDULER: MakaOS has no scheduling policies; skip.
        }

        // Replay the recorded file actions in order.
        if (fa) {
            struct __spawn_action* acts = (struct __spawn_action*)fa->__actions;
            for (int i = 0; i < fa->__used; i++) {
                struct __spawn_action* a = &acts[i];
                if (a->tag == ACT_OPEN) {
                    int f = open(a->path, a->oflag, a->mode);
                    if (f < 0) _exit(127);
                    if (f != a->fd) { if (dup2(f, a->fd) < 0) _exit(127); close(f); }
                } else if (a->tag == ACT_CLOSE) {
                    close(a->fd);
                } else { // ACT_DUP2
                    if (dup2(a->fd, a->newfd) < 0) _exit(127);
                }
            }
        }

        if (usepath) {
            if (envp) environ = (char**)envp;
            execvp(path, argv);
        } else {
            execve(path, argv, envp ? envp : environ);
        }
        _exit(127);   // exec failed
    }

    if (pidp) *pidp = pid;
    return 0;
}

int posix_spawn(pid_t* pid, const char* path,
                const posix_spawn_file_actions_t* fa,
                const posix_spawnattr_t* attr,
                char* const argv[], char* const envp[]) {
    return spawn_common(pid, path, 0, fa, attr, argv, envp);
}

int posix_spawnp(pid_t* pid, const char* file,
                 const posix_spawn_file_actions_t* fa,
                 const posix_spawnattr_t* attr,
                 char* const argv[], char* const envp[]) {
    return spawn_common(pid, file, 1, fa, attr, argv, envp);
}
