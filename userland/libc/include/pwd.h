#ifndef _MAKAOS_PWD_H
#define _MAKAOS_PWD_H 1

#include <sys/types.h>

struct passwd {
    char*  pw_name;
    char*  pw_passwd;
    uid_t  pw_uid;
    gid_t  pw_gid;
    char*  pw_gecos;
    char*  pw_dir;
    char*  pw_shell;
};

struct passwd* getpwnam(const char* name);
struct passwd* getpwuid(uid_t uid);
int getpwnam_r(const char* name, struct passwd* pw,
                char* buf, size_t buflen, struct passwd** result);
int getpwuid_r(uid_t uid, struct passwd* pw,
                char* buf, size_t buflen, struct passwd** result);
void setpwent(void);
void endpwent(void);
struct passwd* getpwent(void);

#endif
