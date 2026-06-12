#ifndef _FTS_H
#define _FTS_H
// Minimal BSD fts(3) — file-tree traversal.  Implemented over
// opendir/readdir/d_type (no per-entry stat by default).  Covers the
// subset ports actually use: fts_open / fts_read / fts_close, with
// FTSENT.fts_path / fts_name / fts_info / fts_level.  Pre-order only
// (each directory is yielded as FTS_D before its children); FTS_DP /
// fts_children / fts_set are not provided.

#include <sys/types.h>
#include <sys/stat.h>

// fts_open options
#define FTS_COMFOLLOW 0x0001
#define FTS_LOGICAL   0x0002   // follow symlinks (we always do)
#define FTS_NOCHDIR   0x0004   // we never chdir anyway
#define FTS_NOSTAT    0x0008
#define FTS_PHYSICAL  0x0010
#define FTS_SEEDOT    0x0020
#define FTS_XDEV      0x0040

// FTSENT.fts_info values
#define FTS_D     1   // directory, pre-order
#define FTS_DC    2   // directory causing a cycle
#define FTS_DEFAULT 3
#define FTS_DNR   4   // directory unreadable
#define FTS_DOT   5
#define FTS_DP    6   // directory, post-order
#define FTS_ERR   7
#define FTS_F     8   // regular file
#define FTS_NS    10
#define FTS_NSOK  11
#define FTS_SL    12
#define FTS_SLNONE 13

typedef struct _ftsent {
    struct _ftsent* fts_link;
    long            fts_number;
    void*           fts_pointer;
    char*           fts_path;       // full path from the traversal root
    int             fts_errno;
    int             fts_symfd;
    unsigned short  fts_pathlen;
    unsigned short  fts_namelen;
    ino_t           fts_ino;
    dev_t           fts_dev;
    nlink_t         fts_nlink;
    short           fts_level;
    unsigned short  fts_info;
    unsigned short  fts_flags;
    unsigned short  fts_instr;
    struct stat*    fts_statp;
    char            fts_name[1];    // flexible; allocated with the struct
} FTSENT;

typedef struct _fts FTS;

FTS*    fts_open(char* const* path_argv, int options,
                 int (*compar)(const FTSENT**, const FTSENT**));
FTSENT* fts_read(FTS* ftsp);
int     fts_close(FTS* ftsp);

#endif // _FTS_H
