#ifndef _MAKAOS_SYS_UTSNAME_H
#define _MAKAOS_SYS_UTSNAME_H 1

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int uname(struct utsname* buf);

#endif
