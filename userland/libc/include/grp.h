// ── grp.h — group database ───────────────────────────────────────────
// Port-surface header.  MakaOS has /etc/passwd but no /etc/group; the
// lookups report "no such group" and consumers (gio file info) leave
// group attributes unset.

#ifndef _MAKAOS_GRP_H
#define _MAKAOS_GRP_H 1

#include <sys/types.h>
#include <stddef.h>

struct group {
    char*  gr_name;
    char*  gr_passwd;
    gid_t  gr_gid;
    char** gr_mem;
};

struct group* getgrnam(const char* name);
struct group* getgrgid(gid_t gid);
int getgrnam_r(const char* name, struct group* grp,
               char* buf, size_t buflen, struct group** result);
int getgrgid_r(gid_t gid, struct group* grp,
               char* buf, size_t buflen, struct group** result);

#endif
