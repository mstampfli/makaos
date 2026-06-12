// ── fts.c — minimal file-tree traversal (BSD fts(3) subset) ──────────
//
// Pre-order DFS over a set of root paths using opendir/readdir/d_type.
// Each fts_read() returns the next FTSENT; the returned pointer stays
// valid until the following fts_read()/fts_close().  Implemented for the
// ports that need it (tofi's .desktop scan): fts_open/fts_read/fts_close,
// fts_path / fts_name / fts_info / fts_level.  No stat() per entry — we
// classify dir vs file from dirent.d_type.

#include <fts.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#define FTS_MAX_DEPTH 64   // guards symlink cycles under FTS_LOGICAL

typedef struct {
    DIR*  dir;
    char* path;     // directory path (no trailing slash)
    size_t pathlen;
} fts_frame_t;

struct _fts {
    char**       roots;     // copied root path strings
    int          nroots;
    int          root_idx;

    fts_frame_t* stack;     // open-directory DFS stack
    int          depth;     // entries used
    int          cap;       // capacity

    FTSENT*      cur;        // last returned entry (freed on next read/close)
    int          options;
};

static int push_dir(FTS* f, const char* path) {
    DIR* d = opendir(path);
    if (!d) return 0;                       // skip unreadable dirs
    if (f->depth >= FTS_MAX_DEPTH) { closedir(d); return 0; }
    if (f->depth == f->cap) {
        int nc = f->cap ? f->cap * 2 : 8;
        fts_frame_t* ns = realloc(f->stack, (size_t)nc * sizeof(*ns));
        if (!ns) { closedir(d); return 0; }
        f->stack = ns; f->cap = nc;
    }
    fts_frame_t* fr = &f->stack[f->depth++];
    fr->dir = d;
    fr->pathlen = strlen(path);
    fr->path = malloc(fr->pathlen + 1);
    if (!fr->path) { closedir(d); f->depth--; return 0; }
    memcpy(fr->path, path, fr->pathlen + 1);
    return 1;
}

static void pop_dir(FTS* f) {
    fts_frame_t* fr = &f->stack[--f->depth];
    closedir(fr->dir);
    free(fr->path);
}

static void free_cur(FTS* f) {
    if (f->cur) { free(f->cur); f->cur = NULL; }
}

FTS* fts_open(char* const* path_argv, int options,
              int (*compar)(const FTSENT**, const FTSENT**)) {
    (void)compar;
    if (!path_argv || !path_argv[0]) return NULL;
    FTS* f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->options = options;

    int n = 0;
    while (path_argv[n]) n++;
    f->roots = calloc((size_t)n, sizeof(char*));
    if (!f->roots) { free(f); return NULL; }
    for (int i = 0; i < n; i++) {
        size_t l = strlen(path_argv[i]);
        f->roots[i] = malloc(l + 1);
        if (!f->roots[i]) { for (int j = 0; j < i; j++) free(f->roots[j]);
                            free(f->roots); free(f); return NULL; }
        memcpy(f->roots[i], path_argv[i], l + 1);
    }
    f->nroots = n;
    return f;
}

// Build an FTSENT for `name` inside the directory at `frame`.
static FTSENT* make_entry(FTS* f, fts_frame_t* fr, const char* name,
                          int info, int level) {
    size_t nl = strlen(name);
    size_t pl = fr->pathlen + 1 + nl;           // path "/" name
    // FTSENT + room for fts_name (flexible) + the full path string.
    FTSENT* e = malloc(sizeof(FTSENT) + nl + 1 + pl + 1);
    if (!e) return NULL;
    memset(e, 0, sizeof(FTSENT));
    char* namebuf = e->fts_name;                // fts_name[1] flexible
    memcpy(namebuf, name, nl + 1);
    char* pathbuf = namebuf + nl + 1;
    memcpy(pathbuf, fr->path, fr->pathlen);
    pathbuf[fr->pathlen] = '/';
    memcpy(pathbuf + fr->pathlen + 1, name, nl + 1);
    e->fts_path    = pathbuf;
    e->fts_pathlen = (unsigned short)pl;
    e->fts_namelen = (unsigned short)nl;
    e->fts_info    = (unsigned short)info;
    e->fts_level   = (short)level;
    return e;
}

FTSENT* fts_read(FTS* f) {
    if (!f) return NULL;
    free_cur(f);

    for (;;) {
        if (f->depth == 0) {
            // Open the next root directory.
            if (f->root_idx >= f->nroots) return NULL;   // traversal complete
            const char* rp = f->roots[f->root_idx++];
            // Try to descend; if it isn't a readable directory, skip it.
            if (!push_dir(f, rp)) continue;
            continue;   // first real entry comes from the readdir below
        }

        fts_frame_t* fr = &f->stack[f->depth - 1];
        struct dirent* de = readdir(fr->dir);
        if (!de) { pop_dir(f); continue; }

        const char* nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;   // skip . and ..

        int level = f->depth;   // root entries are level 1
        if (de->d_type == DT_DIR) {
            FTSENT* e = make_entry(f, fr, nm, FTS_D, level);
            if (!e) continue;
            // Recurse into it (its fts_path is the dir we descend).
            push_dir(f, e->fts_path);
            f->cur = e;
            return e;
        } else {
            FTSENT* e = make_entry(f, fr, nm, FTS_F, level);
            if (!e) continue;
            f->cur = e;
            return e;
        }
    }
}

int fts_close(FTS* f) {
    if (!f) return -1;
    free_cur(f);
    while (f->depth > 0) pop_dir(f);
    free(f->stack);
    for (int i = 0; i < f->nroots; i++) free(f->roots[i]);
    free(f->roots);
    free(f);
    return 0;
}
