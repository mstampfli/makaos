// ── mntent.h — mounted-filesystem table ──────────────────────────────
// Port-surface header (glib's gunixmounts.c needs SOME mount-table
// backend or it #errors).  MakaOS has a single static mount layout
// and no /etc/mtab, so setmntent returns NULL and every consumer sees
// an empty table — gio degrades to "no mounts", which is accurate.

#ifndef _MAKAOS_MNTENT_H
#define _MAKAOS_MNTENT_H 1

#include <stdio.h>

#define MOUNTED        "/etc/mtab"
#define MNTTYPE_IGNORE "ignore"
#define MNTOPT_RO      "ro"
#define MNTOPT_RW      "rw"

struct mntent {
    char* mnt_fsname;
    char* mnt_dir;
    char* mnt_type;
    char* mnt_opts;
    int   mnt_freq;
    int   mnt_passno;
};

FILE*          setmntent(const char* filename, const char* type);
struct mntent* getmntent(FILE* fp);
struct mntent* getmntent_r(FILE* fp, struct mntent* out,
                           char* buf, int buflen);
int            addmntent(FILE* fp, const struct mntent* mnt);
int            endmntent(FILE* fp);
char*          hasmntopt(const struct mntent* mnt, const char* opt);

#endif
