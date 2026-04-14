// svcmgr — MakaOS service manager
//
// Reads /etc/services/*.svc at boot, builds a dependency DAG, spawns each
// service in topological order, then stays alive to supervise (restart on
// failure if restart=always or restart=on-failure).
//
// .svc file format (line-oriented, key=value, # comments):
//
//   path=/bin/net          # required: absolute path to binary
//   argv=net -v            # optional: argv[0..n], space-split (default: path basename)
//   uid=0                  # optional: drop to uid before exec   (default: 0)
//   gid=0                  # optional: drop to gid before exec   (default: 0)
//   stdio=null             # optional: null | inherit             (default: null)
//   pledge=net stdio       # optional: space-separated pledge words
//   unveil=/etc r          # optional: multiple lines ok, path + perms char
//   after=                 # optional: space-separated service names to wait for
//   restart=on-failure     # optional: always | on-failure | never (default: never)

#include "../../libc/libc.h"

// ── Limits ────────────────────────────────────────────────────────────────

#define SVC_MAX          64        // max services
#define SVC_MAX_ARGV     16        // max argv entries per service
#define SVC_MAX_UNVEIL   16        // max unveil entries per service
#define SVC_MAX_AFTER    8         // max after= dependencies
#define SVC_PATH_MAX     256
#define SVC_LINE_MAX     512
#define SVC_FILE_MAX     4096

// ── Pledge word → bitmask table ───────────────────────────────────────────

typedef struct { const char* word; uint32_t bit; } pledge_word_t;

static const pledge_word_t s_pledge_words[] = {
    { "stdio",     PLEDGE_STDIO     },
    { "rpath",     PLEDGE_RPATH     },
    { "wpath",     PLEDGE_WPATH     },
    { "cpath",     PLEDGE_CPATH     },
    { "exec",      PLEDGE_EXEC      },
    { "proc",      PLEDGE_PROC      },
    { "net",       PLEDGE_INET      },
    { "unix",      PLEDGE_UNIX      },
    { "signal",    PLEDGE_SIGNAL    },
    { "thread",    PLEDGE_THREAD    },
    { "prot_exec", PLEDGE_PROT_EXEC },
    { "setuid",    PLEDGE_SETUID    },
    { "chown",     PLEDGE_CHOWN     },
    { "chmod",     PLEDGE_CHMOD     },
    { "tty",       PLEDGE_TTY       },
    { "ioctl",     PLEDGE_IOCTL     },
    { NULL,        0                },
};

// ── Restart policy ────────────────────────────────────────────────────────

#define RESTART_NEVER       0
#define RESTART_ON_FAILURE  1
#define RESTART_ALWAYS      2

// ── Service descriptor ────────────────────────────────────────────────────

typedef struct {
    char     name[64];               // derived from filename (stem of .svc)
    char     path[SVC_PATH_MAX];
    char     argv_store[SVC_MAX_ARGV][SVC_PATH_MAX]; // backing storage for argv strings
    char*    argv[SVC_MAX_ARGV + 1]; // NULL-terminated pointers into argv_store
    uint32_t argc;
    uint32_t uid;
    uint32_t gid;
    int      stdio_null;             // 1 = /dev/null, 0 = inherit
    uint32_t pledge_mask;
    int      has_pledge;
    struct { char path[SVC_PATH_MAX]; uint8_t perms; } unveil[SVC_MAX_UNVEIL];
    uint32_t nunveil;
    char     after[SVC_MAX_AFTER][64]; // dependency names
    uint32_t nafter;
    int      restart;                // RESTART_*

    // Runtime state
    int      pid;                    // current child pid, -1 if not running
    int      started;                // 1 once first spawned
    int      in_degree;              // remaining unsatisfied deps (DAG)
    int      done;                   // 1 = spawned this boot cycle
} svc_t;

static svc_t s_svcs[SVC_MAX];
static int   s_nsvc = 0;

// ── String helpers ────────────────────────────────────────────────────────

static int sm_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static int sm_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int sm_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static void sm_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void sm_log(const char* msg) {
    write(2, "[svcmgr] ", 9);
    write(2, msg, sm_strlen(msg));
    write(2, "\n", 1);
}

static void sm_log2(const char* a, const char* b) {
    write(2, "[svcmgr] ", 9);
    write(2, a, sm_strlen(a));
    write(2, b, sm_strlen(b));
    write(2, "\n", 1);
}

// Skip leading whitespace, return pointer past it.
static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// Copy next whitespace-delimited token from *pp into dst (max bytes).
// Advances *pp past the token. Returns length, 0 if none.
static int next_token(const char** pp, char* dst, int max) {
    const char* p = skip_ws(*pp);
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && i < max - 1)
        dst[i++] = *p++;
    dst[i] = '\0';
    *pp = p;
    return i;
}

// Extract the stem from a filename: "net.svc" → "net".
static void stem(const char* fname, char* out, int max) {
    int i = 0;
    while (fname[i] && fname[i] != '.' && i < max - 1) { out[i] = fname[i]; i++; }
    out[i] = '\0';
}

// ── .svc parser ───────────────────────────────────────────────────────────

static uint8_t parse_unveil_perms(const char* s) {
    uint8_t p = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == 'r') p |= UNVEIL_READ;
        if (s[i] == 'w') p |= UNVEIL_WRITE;
        if (s[i] == 'x') p |= UNVEIL_EXEC;
        if (s[i] == 'c') p |= UNVEIL_CREATE;
    }
    return p;
}

static uint32_t parse_pledge(const char* val) {
    uint32_t mask = 0;
    const char* p = val;
    char tok[32];
    while (next_token(&p, tok, sizeof(tok)) > 0) {
        for (int i = 0; s_pledge_words[i].word; i++) {
            if (sm_strcmp(tok, s_pledge_words[i].word) == 0) {
                mask |= s_pledge_words[i].bit;
                break;
            }
        }
    }
    return mask;
}

static int parse_svc(svc_t* s, const char* buf, int len) {
    // Defaults
    s->uid        = 0;
    s->gid        = 0;
    s->stdio_null = 1;
    s->has_pledge = 0;
    s->pledge_mask= PLEDGE_ALL;
    s->nunveil    = 0;
    s->nafter     = 0;
    s->restart    = RESTART_NEVER;
    s->argc       = 0;
    s->pid        = -1;
    s->started    = 0;
    s->in_degree  = 0;
    s->done       = 0;

    const char* p = buf;
    const char* end = buf + len;

    while (p < end) {
        // Skip blank lines and comments.
        p = skip_ws(p);
        if (*p == '#' || *p == '\n' || *p == '\0') {
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            continue;
        }

        // Read key.
        char key[64]; int ki = 0;
        while (p < end && *p != '=' && *p != '\n' && ki < 63)
            key[ki++] = *p++;
        key[ki] = '\0';
        if (*p != '=') { while (p < end && *p != '\n') p++; if (p < end) p++; continue; }
        p++; // skip '='

        // Read value (rest of line, trimmed).
        const char* vstart = p;
        while (p < end && *p != '\n') p++;
        int vlen = (int)(p - vstart);
        // Trim trailing whitespace.
        while (vlen > 0 && (vstart[vlen-1] == ' ' || vstart[vlen-1] == '\t')) vlen--;
        char val[SVC_LINE_MAX];
        if (vlen >= SVC_LINE_MAX) vlen = SVC_LINE_MAX - 1;
        for (int i = 0; i < vlen; i++) val[i] = vstart[i];
        val[vlen] = '\0';
        if (p < end) p++; // skip '\n'

        if (sm_strcmp(key, "path") == 0) {
            sm_strcpy(s->path, val, SVC_PATH_MAX);
        } else if (sm_strcmp(key, "argv") == 0) {
            const char* vp = val;
            s->argc = 0;
            while (s->argc < SVC_MAX_ARGV) {
                char tok[SVC_PATH_MAX];
                if (next_token(&vp, tok, sizeof(tok)) == 0) break;
                sm_strcpy(s->argv_store[s->argc], tok, SVC_PATH_MAX);
                s->argv[s->argc] = s->argv_store[s->argc];
                s->argc++;
            }
            s->argv[s->argc] = NULL;
        } else if (sm_strcmp(key, "uid") == 0) {
            uint32_t v = 0;
            for (int i = 0; val[i] >= '0' && val[i] <= '9'; i++) v = v*10 + (val[i]-'0');
            s->uid = v;
        } else if (sm_strcmp(key, "gid") == 0) {
            uint32_t v = 0;
            for (int i = 0; val[i] >= '0' && val[i] <= '9'; i++) v = v*10 + (val[i]-'0');
            s->gid = v;
        } else if (sm_strcmp(key, "stdio") == 0) {
            s->stdio_null = (sm_strcmp(val, "null") == 0);
        } else if (sm_strcmp(key, "pledge") == 0) {
            s->pledge_mask = parse_pledge(val);
            s->has_pledge  = 1;
        } else if (sm_strcmp(key, "unveil") == 0) {
            if (s->nunveil < SVC_MAX_UNVEIL) {
                const char* vp = val;
                char upath[SVC_PATH_MAX], uperms[8];
                if (next_token(&vp, upath,   sizeof(upath))   > 0 &&
                    next_token(&vp, uperms,  sizeof(uperms))  > 0) {
                    sm_strcpy(s->unveil[s->nunveil].path, upath, SVC_PATH_MAX);
                    s->unveil[s->nunveil].perms = parse_unveil_perms(uperms);
                    s->nunveil++;
                }
            }
        } else if (sm_strcmp(key, "after") == 0) {
            const char* vp = val;
            while (s->nafter < SVC_MAX_AFTER) {
                char tok[64];
                if (next_token(&vp, tok, sizeof(tok)) == 0) break;
                sm_strcpy(s->after[s->nafter++], tok, 64);
            }
        } else if (sm_strcmp(key, "restart") == 0) {
            if (sm_strcmp(val, "always") == 0)      s->restart = RESTART_ALWAYS;
            else if (sm_strcmp(val, "on-failure") == 0) s->restart = RESTART_ON_FAILURE;
            else                                    s->restart = RESTART_NEVER;
        }
    }

    // If no argv= given, synthesise from path basename.
    if (s->argc == 0 && s->path[0]) {
        const char* base = s->path;
        for (int i = 0; s->path[i]; i++) if (s->path[i] == '/') base = s->path + i + 1;
        sm_strcpy(s->argv_store[0], base, SVC_PATH_MAX);
        s->argv[0] = s->argv_store[0];
        s->argv[1] = NULL;
        s->argc    = 1;
    }

    return s->path[0] ? 0 : -1;
}

// ── Service loader ────────────────────────────────────────────────────────

static void load_services(void) {
    char buf[SVC_FILE_MAX];

    // Read directory entries from /etc/services.
    // We use getdents via readdir syscall — libc exposes opendir/readdir.
    DIR* dir = opendir("/etc/services");
    if (!dir) { sm_log("cannot open /etc/services"); return; }

    struct dirent* de;
    while ((de = readdir(dir)) != NULL && s_nsvc < SVC_MAX) {
        const char* nm = de->d_name;
        int nlen = sm_strlen(nm);
        // Only process files ending in ".svc".
        if (nlen < 5 || sm_strncmp(nm + nlen - 4, ".svc", 4) != 0) continue;

        char path[SVC_PATH_MAX];
        sm_strcpy(path, "/etc/services/", SVC_PATH_MAX);
        int pl = sm_strlen(path);
        sm_strcpy(path + pl, nm, SVC_PATH_MAX - pl);

        int fd = open(path, O_RDONLY, 0);
        if (fd < 0) { sm_log2("cannot open: ", path); continue; }
        int n = (int)read(fd, buf, SVC_FILE_MAX - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';

        svc_t* s = &s_svcs[s_nsvc];
        if (parse_svc(s, buf, n) < 0) { sm_log2("parse failed: ", path); continue; }

        // Name = filename stem.
        stem(nm, s->name, sizeof(s->name));
        sm_log2("loaded: ", s->name);
        s_nsvc++;
    }
    closedir(dir);
}

// ── DAG builder + Kahn topological sort ───────────────────────────────────

static void build_dag(void) {
    for (int i = 0; i < s_nsvc; i++) {
        s_svcs[i].in_degree = 0;
        s_svcs[i].done      = 0;
    }
    for (int i = 0; i < s_nsvc; i++) {
        for (uint32_t d = 0; d < s_svcs[i].nafter; d++) {
            // Check this dep exists in our set.
            for (int j = 0; j < s_nsvc; j++) {
                if (sm_strcmp(s_svcs[j].name, s_svcs[i].after[d]) == 0) {
                    s_svcs[i].in_degree++;
                    break;
                }
            }
        }
    }
}

// ── Spawn a single service ────────────────────────────────────────────────

static void spawn_svc(svc_t* s) {
    // Build stdio spec.
    int stdio[3];
    if (s->stdio_null) {
        stdio[0] = -2; stdio[1] = -2; stdio[2] = -2; // /dev/null
    } else {
        stdio[0] = -1; stdio[1] = -1; stdio[2] = -1; // inherit
    }

    // Build spawn_attr.
    spawn_attr_t attr;
    memset(&attr, 0, sizeof(attr));

    // Always apply credentials (even uid=0/gid=0 is explicit).
    attr.flags |= SPAWN_ATTR_CRED;
    attr.uid    = s->uid;
    attr.gid    = s->gid;

    if (s->has_pledge) {
        attr.flags       |= SPAWN_ATTR_PLEDGE;
        attr.pledge_mask  = s->pledge_mask;
    }

    if (s->nunveil > 0) {
        attr.flags   |= SPAWN_ATTR_UNVEIL;
        attr.nunveil  = s->nunveil;
        attr.unveil   = (const spawn_unveil_entry_t*)s->unveil; // same layout
    }

    int pid = spawn(s->path, (const char* const*)s->argv,
                    (const char* const*)0, stdio, &attr);
    if (pid < 0) {
        sm_log2("spawn failed: ", s->name);
        return;
    }

    s->pid     = pid;
    s->started = 1;
    sm_log2("started: ", s->name);
}

// ── Initial boot spawn (topological order) ────────────────────────────────

static void spawn_all(void) {
    build_dag();

    // Kahn's algorithm.
    int queue[SVC_MAX];
    int qhead = 0, qtail = 0;
    int spawned = 0;

    for (int i = 0; i < s_nsvc; i++)
        if (s_svcs[i].in_degree == 0)
            queue[qtail++] = i;

    while (qhead != qtail) {
        int idx = queue[qhead++];
        svc_t* s = &s_svcs[idx];
        spawn_svc(s);
        s->done = 1;
        spawned++;

        // Reduce in-degree of services that depend on this one.
        for (int i = 0; i < s_nsvc; i++) {
            if (s_svcs[i].done) continue;
            for (uint32_t d = 0; d < s_svcs[i].nafter; d++) {
                if (sm_strcmp(s_svcs[i].after[d], s->name) == 0) {
                    s_svcs[i].in_degree--;
                    if (s_svcs[i].in_degree == 0)
                        queue[qtail++] = i;
                    break;
                }
            }
        }
    }

    if (spawned < s_nsvc)
        sm_log("WARNING: dependency cycle detected — some services not started");
}

// ── Supervisor loop ───────────────────────────────────────────────────────

static void supervise(void) {
    for (;;) {
        int status = 0;
        int pid = waitpid(-1, &status, 0);
        if (pid <= 0) continue;

        // Find which service this was.
        svc_t* s = NULL;
        for (int i = 0; i < s_nsvc; i++) {
            if (s_svcs[i].pid == pid) { s = &s_svcs[i]; break; }
        }
        if (!s) continue;

        s->pid = -1;
        int exited  = WIFEXITED(status);
        int code    = WEXITSTATUS(status);
        int success = exited && code == 0;

        if (s->restart == RESTART_ALWAYS ||
            (s->restart == RESTART_ON_FAILURE && !success)) {
            sm_log2("restarting: ", s->name);
            spawn_svc(s);
        } else {
            sm_log2("exited: ", s->name);
        }
    }
}

// ── Entry point ───────────────────────────────────────────────────────────

int main(void) {
    sm_log("starting");
    load_services();
    if (s_nsvc == 0) { sm_log("no services found"); }
    spawn_all();
    supervise();
    return 0;
}
