// restrict — run a command with pledge and/or unveil restrictions
//
// Usage:
//   restrict [options] -- command [args...]
//
// Options:
//   -p <words>          pledge mask (space or comma separated pledge words)
//   -u <path>:<perms>   unveil entry (perms: r w x c, combinable e.g. rw)
//
// Examples:
//   restrict -p "net stdio" -- /bin/http_get http://example.com/
//   restrict -u /etc:r -u /var/run:rw -p "net stdio rpath" -- /bin/net
//
// Multiple -u flags are allowed. -p may appear once.
// Restrictions are applied atomically via spawn(); the child cannot escape them.
// restrict itself waits for the child and forwards its exit status.

#include "../../libc/libc.h"

// ── Pledge word table ─────────────────────────────────────────────────────

typedef struct { const char* word; uint32_t bit; } pw_t;
static const pw_t s_words[] = {
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

static int r_strlen(const char* s) { int n=0; while(s[n]) n++; return n; }
static int r_strcmp(const char* a, const char* b) {
    while (*a && *b && *a==*b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void r_strcpy(char* d, const char* s, int max) {
    int i=0; while(i<max-1&&s[i]){d[i]=s[i];i++;} d[i]='\0';
}

static void usage(void) {
    const char* msg =
        "usage: restrict [-p pledge_words] [-u path:perms] -- cmd [args]\n"
        "  -p  space/comma separated pledge words (net stdio rpath ...)\n"
        "  -u  unveil entry as path:perms (r=read w=write x=exec c=create)\n";
    write(2, msg, r_strlen(msg));
}

static uint32_t parse_pledge(const char* s) {
    uint32_t mask = 0;
    char tok[32]; int ti = 0;
    for (int i = 0; ; i++) {
        char c = s[i];
        if (c == ' ' || c == ',' || c == '\0') {
            if (ti > 0) {
                tok[ti] = '\0';
                for (int j = 0; s_words[j].word; j++)
                    if (r_strcmp(tok, s_words[j].word) == 0) { mask |= s_words[j].bit; break; }
                ti = 0;
            }
            if (c == '\0') break;
        } else if (ti < 31) {
            tok[ti++] = c;
        }
    }
    return mask;
}

static uint8_t parse_perms(const char* s) {
    uint8_t p = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == 'r') p |= UNVEIL_READ;
        if (s[i] == 'w') p |= UNVEIL_WRITE;
        if (s[i] == 'x') p |= UNVEIL_EXEC;
        if (s[i] == 'c') p |= UNVEIL_CREATE;
    }
    return p;
}

int main(int argc, char** argv) {
    spawn_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    // Unveil entries stored separately; at most argc/2 -u entries possible.
    spawn_unveil_entry_t unveils[argc / 2 + 1];
    int has_pledge = 0;

    int i = 1;
    for (; i < argc; i++) {
        if (r_strcmp(argv[i], "--") == 0) { i++; break; }
        if (r_strcmp(argv[i], "-p") == 0) {
            if (++i >= argc) { usage(); return 1; }
            attr.pledge_mask = parse_pledge(argv[i]);
            has_pledge = 1;
        } else if (r_strcmp(argv[i], "-u") == 0) {
            if (++i >= argc) { usage(); return 1; }
            // Split "path:perms" at the last colon.
            const char* arg = argv[i];
            int alen = r_strlen(arg);
            int colon = -1;
            for (int j = alen - 1; j >= 0; j--) { if (arg[j] == ':') { colon = j; break; } }
            if (colon < 0) { write(2, "restrict: -u requires path:perms\n", 33); return 1; }
            r_strcpy(unveils[attr.nunveil].path, arg, colon + 1 > 256 ? 256 : colon + 1);
            unveils[attr.nunveil].path[colon < 255 ? colon : 255] = '\0';
            unveils[attr.nunveil].perms = parse_perms(arg + colon + 1);
            attr.nunveil++;
        } else {
            // No leading '--', treat as start of command.
            break;
        }
    }

    if (i >= argc) { usage(); return 1; }

    // Build flags.
    if (has_pledge) attr.flags |= SPAWN_ATTR_PLEDGE;
    if (attr.nunveil > 0) { attr.flags |= SPAWN_ATTR_UNVEIL; attr.unveil = unveils; }

    // stdio: inherit all three (restrict is transparent).
    int stdio[3] = { -1, -1, -1 };

    // Build argv for child: argv[i..].
    const char* const* child_argv = (const char* const*)&argv[i];

    int pid = spawn(argv[i], child_argv, (const char* const*)0,
                    stdio, (attr.flags ? &attr : NULL));
    if (pid < 0) {
        write(2, "restrict: spawn failed\n", 23);
        return 1;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
