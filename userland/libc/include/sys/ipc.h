// sys/ipc.h - System V IPC common definitions (Linux-compatible subset).

#ifndef _SYS_IPC_H
#define _SYS_IPC_H 1

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int key_t;

struct ipc_perm {
    key_t          __key;
    uid_t          uid;
    gid_t          gid;
    uid_t          cuid;
    gid_t          cgid;
    unsigned short mode;
    unsigned short __seq;
};

#define IPC_PRIVATE ((key_t)0)

// ipc get() flags.
#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000

// ipc ctl() commands.
#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2

#ifdef __cplusplus
}
#endif

#endif /* sys/ipc.h */
