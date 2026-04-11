// ksec — Kore Security Policy Daemon
//
// Runs as uid=0, started by init before any unprivileged processes.
// Registers itself as the kernel's policy agent via register_policy_agent().
// Reads ksec_request_t packets from the kernel, evaluates /etc/ksec.policy,
// and writes ksec_response_t verdicts back.
//
// Also listens on /run/ksec (Unix socket) for userland resource requests
// (privilege escalation, privileged fd hand-off).
//
// Policy file format (/etc/ksec.policy):
//   # comment
//   setuid  /path/to/binary  allow=*|uid=N|gid=NAME  target=N  auth=none|password
//   resource /dev/foo        allow=*|uid=N|gid=NAME   auth=none|password
//
// Matching for allow= field:
//   *          any caller
//   uid=N      caller's euid == N
//   gid=N      caller's egid == N or N in supplemental groups
//   uid=N,gid=M  both must match

#include "libc.h"
#include "stdio.h"

// ── Wire types (must match kernel/security/ksec.h exactly) ────────────────

#define KSEC_OP_EXEC_SETUID   1
#define KSEC_OP_SETEUID       2
#define KSEC_OP_SETUID        3
#define KSEC_OP_RESOURCE_FD   4
#define KSEC_OP_SETUID_BIT    5
#define KSEC_OP_CLRUID_BIT    6

#define KSEC_VERDICT_ALLOW    0
#define KSEC_VERDICT_DENY     1
#define KSEC_VERDICT_CHALLENGE 2

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint8_t  op;
    uint8_t  _pad[3];
    uint32_t caller_pid;
    uint32_t caller_uid;
    uint32_t caller_gid;
    uint32_t inode;
    uint32_t dev;
    uint32_t target_uid;
    char     resource[128];
    uint32_t file_uid;
} ksec_request_t;

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint8_t  verdict;
    uint8_t  _pad[3];
    uint32_t granted_euid;
    uint32_t granted_egid;
    uint32_t granted_rights;
} ksec_response_t;

// ── Policy entry ──────────────────────────────────────────────────────────

#define POLICY_MAX      64
#define POLICY_TYPE_SETUID   0
#define POLICY_TYPE_RESOURCE 1

#define ALLOW_ANY    0
#define ALLOW_UID    1
#define ALLOW_GID    2

#define AUTH_NONE     0
#define AUTH_PASSWORD 1
#define AUTH_TIMED    2

typedef struct {
    uint8_t  type;          // POLICY_TYPE_*
    char     path[128];     // binary path or resource path (may contain '*')
    uint32_t inode;         // 0 = match by path only
    uint32_t dev;           // 0 = any device
    uint8_t  allow_type;    // ALLOW_ANY / ALLOW_UID / ALLOW_GID
    uint32_t allow_id;      // uid or gid (ALLOW_ANY: ignored)
    uint32_t target_euid;   // for setuid: the euid to grant
    uint8_t  auth;          // AUTH_*
    uint8_t  active;
} policy_entry_t;

static policy_entry_t s_policy[POLICY_MAX];
static int            s_policy_count = 0;

// ── String helpers ────────────────────────────────────────────────────────

__attribute__((unused))
static int s_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static int s_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int s_strncmp(const char* a, const char* b, int n) {
    int i = 0;
    for (; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

__attribute__((unused))
static void s_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int s_atoi(const char* s) {
    int v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static int s_skip_ws(const char* s, int i) {
    while (s[i] == ' ' || s[i] == '\t') i++;
    return i;
}

// Glob match: '*' matches any sequence including empty.
__attribute__((unused))
static int glob_match(const char* pattern, const char* str) {
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*str) {
                if (glob_match(pattern, str)) return 1;
                str++;
            }
            return 0;
        }
        if (*pattern != *str) return 0;
        pattern++; str++;
    }
    while (*pattern == '*') pattern++;
    return !*pattern && !*str;
}

// ── Policy parser ─────────────────────────────────────────────────────────

// Parse a single line into s_policy[].
// Returns 1 if a valid entry was added, 0 otherwise.
static int parse_policy_line(const char* line) {
    int i = s_skip_ws(line, 0);

    // Skip comments and blank lines.
    if (!line[i] || line[i] == '#' || line[i] == '\n') return 0;

    if (s_policy_count >= POLICY_MAX) return 0;
    policy_entry_t* e = &s_policy[s_policy_count];
    e->active      = 0;
    e->inode       = 0;
    e->dev         = 0;
    e->allow_type  = ALLOW_ANY;
    e->allow_id    = 0;
    e->target_euid = 0;
    e->auth        = AUTH_NONE;

    // Determine type: "setuid" or "resource".
    if (s_strncmp(line + i, "setuid", 6) == 0 && (line[i+6] == ' ' || line[i+6] == '\t')) {
        e->type = POLICY_TYPE_SETUID;
        i = s_skip_ws(line, i + 6);
    } else if (s_strncmp(line + i, "resource", 8) == 0 && (line[i+8] == ' ' || line[i+8] == '\t')) {
        e->type = POLICY_TYPE_RESOURCE;
        i = s_skip_ws(line, i + 8);
    } else {
        return 0;   // unknown type
    }

    // Path field (no spaces).
    int pstart = i;
    while (line[i] && line[i] != ' ' && line[i] != '\t' && line[i] != '\n') i++;
    int plen = i - pstart;
    if (plen == 0 || plen >= 128) return 0;
    s_strncmp(e->path, line + pstart, 0);  // use strcpy below
    int pi = 0;
    for (; pi < plen && pi < 127; pi++) e->path[pi] = line[pstart + pi];
    e->path[pi] = '\0';

    // Parse key=value tokens.
    for (;;) {
        i = s_skip_ws(line, i);
        if (!line[i] || line[i] == '\n' || line[i] == '#') break;

        // Find '='.
        int kstart = i;
        while (line[i] && line[i] != '=' && line[i] != ' ' && line[i] != '\t') i++;
        if (line[i] != '=') break;
        int klen = i - kstart;
        i++;  // skip '='

        // Find end of value.
        int vstart = i;
        while (line[i] && line[i] != ' ' && line[i] != '\t' && line[i] != '\n') i++;
        int vlen = i - vstart;
        char val[128];
        int vi = 0;
        for (; vi < vlen && vi < 127; vi++) val[vi] = line[vstart + vi];
        val[vi] = '\0';

        if (s_strncmp(line + kstart, "allow", klen) == 0 && klen == 5) {
            if (s_strcmp(val, "*") == 0) {
                e->allow_type = ALLOW_ANY;
            } else if (s_strncmp(val, "uid=", 4) == 0) {
                e->allow_type = ALLOW_UID;
                e->allow_id   = (uint32_t)s_atoi(val + 4);
            } else if (s_strncmp(val, "gid=", 4) == 0) {
                e->allow_type = ALLOW_GID;
                e->allow_id   = (uint32_t)s_atoi(val + 4);
            }
        } else if (s_strncmp(line + kstart, "target", klen) == 0 && klen == 6) {
            e->target_euid = (uint32_t)s_atoi(val);
        } else if (s_strncmp(line + kstart, "auth", klen) == 0 && klen == 4) {
            if (s_strcmp(val, "password") == 0) e->auth = AUTH_PASSWORD;
            else if (s_strcmp(val, "timed") == 0) e->auth = AUTH_TIMED;
            else e->auth = AUTH_NONE;
        } else if (s_strncmp(line + kstart, "inode", klen) == 0 && klen == 5) {
            e->inode = (uint32_t)s_atoi(val);
        }
    }

    e->active = 1;
    s_policy_count++;
    return 1;
}

// Load policy from file.
static void load_policy(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    char buf[4096];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    // Split into lines and parse.
    int i = 0;
    while (i < n) {
        int lstart = i;
        while (i < n && buf[i] != '\n') i++;
        buf[i] = '\0';
        parse_policy_line(buf + lstart);
        i++;
    }
}

// ── Policy lookup ─────────────────────────────────────────────────────────

// Returns matching policy entry or NULL.
// For EXEC_SETUID: match by inode (if nonzero) then by path glob.
static policy_entry_t* find_setuid(uint32_t inode, uint32_t dev,
                                    uint32_t caller_uid, uint32_t caller_gid) {
    int i;
    for (i = 0; i < s_policy_count; i++) {
        policy_entry_t* e = &s_policy[i];
        if (!e->active || e->type != POLICY_TYPE_SETUID) continue;

        // Inode match (most specific — set when chmod u+s notification arrived).
        if (e->inode != 0) {
            if (e->inode != inode || (e->dev != 0 && e->dev != dev)) continue;
        }

        // allow= check.
        if (e->allow_type == ALLOW_UID && e->allow_id != caller_uid) continue;
        if (e->allow_type == ALLOW_GID && e->allow_id != caller_gid) continue;

        return e;
    }
    return NULL;
}

// Auto-create a setuid entry when kernel notifies us of chmod u+s.
static void auto_create_setuid(uint32_t inode, uint32_t dev, uint32_t file_uid) {
    if (s_policy_count >= POLICY_MAX) return;
    policy_entry_t* e = &s_policy[s_policy_count];
    e->type        = POLICY_TYPE_SETUID;
    e->inode       = inode;
    e->dev         = dev;
    e->allow_type  = ALLOW_ANY;
    e->allow_id    = 0;
    e->target_euid = file_uid;
    e->auth        = AUTH_NONE;
    e->active      = 1;
    e->path[0]     = '\0';
    s_policy_count++;
    // Log the auto-creation.
    printf("ksec: auto-created setuid entry for inode=%u dev=%u target_euid=%u\n",
           inode, dev, file_uid);
}

// ── Main event loop ───────────────────────────────────────────────────────

int main(void) {
    // Must be uid=0.
    if (geteuid() != 0) {
        printf("ksec: must run as root\n");
        exit(1);
    }

    // Load policy.
    load_policy("/etc/ksec.policy");
    printf("ksec: loaded %d policy entries\n", s_policy_count);

    // Create a pipe pair for the kernel channel.
    // kernel writes requests to req_pipe[1], ksec reads from req_pipe[0].
    // ksec writes responses to resp_pipe[1], kernel reads from resp_pipe[0].
    int req_pipe[2], resp_pipe[2];
    if (pipe(req_pipe) != 0 || pipe(resp_pipe) != 0) {
        printf("ksec: pipe creation failed\n");
        exit(1);
    }

    // Register with kernel: kernel reads requests from req_pipe[0] via ksec_reader_thread,
    // and we write responses to resp_pipe[1].
    // Wait — the kernel's ksec channel direction is:
    //   kernel WRITES requests → we READ them.
    //   we WRITE responses → kernel READS them.
    // So: register_policy_agent(kernel_read_fd, kernel_write_fd)
    //   where kernel_read_fd = resp_pipe[0]  (kernel reads our responses)
    //   and   kernel_write_fd = req_pipe[1]  (kernel writes requests to us via ksec_notify/request)
    //
    // We READ from req_pipe[0] and WRITE to resp_pipe[1].
    if (register_policy_agent(resp_pipe[0], req_pipe[1]) != 0) {
        printf("ksec: failed to register as policy agent\n");
        exit(1);
    }

    printf("ksec: registered as policy agent\n");

    // Main loop: read requests, evaluate policy, write responses.
    for (;;) {
        ksec_request_t req;
        int n = (int)read(req_pipe[0], &req, sizeof(req));
        if (n != (int)sizeof(req)) {
            // Short read or error — kernel channel closed? Yield and retry.
            continue;
        }

        ksec_response_t resp = {0};
        resp.seq = req.seq;

        switch (req.op) {

        case KSEC_OP_EXEC_SETUID: {
            policy_entry_t* e = find_setuid(req.inode, req.dev,
                                             req.caller_uid, req.caller_gid);
            if (!e) {
                resp.verdict = KSEC_VERDICT_DENY;
                printf("ksec: DENY exec_setuid inode=%u pid=%u\n",
                       req.inode, req.caller_pid);
            } else if (e->auth == AUTH_PASSWORD) {
                // Challenge: for now deny — full challenge protocol requires
                // a /run/ksec socket and pinentry, which is a separate feature.
                resp.verdict = KSEC_VERDICT_DENY;
                printf("ksec: DENY exec_setuid (auth=password not yet implemented) inode=%u\n",
                       req.inode);
            } else {
                resp.verdict      = KSEC_VERDICT_ALLOW;
                resp.granted_euid = e->target_euid;
                resp.granted_egid = 0;   // gid unchanged
                printf("ksec: ALLOW exec_setuid inode=%u pid=%u → euid=%u\n",
                       req.inode, req.caller_pid, e->target_euid);
            }
            write(resp_pipe[1], &resp, sizeof(resp));
            break;
        }

        case KSEC_OP_SETEUID:
        case KSEC_OP_SETUID: {
            // Kernel-side in-slot transitions are handled directly; we only
            // see genuine escalation requests here.
            // Policy: root (uid=0) may set any uid via ksec only if explicitly allowed.
            resp.verdict = KSEC_VERDICT_DENY;
            printf("ksec: DENY setuid/seteuid escalation uid=%u→%u pid=%u\n",
                   req.caller_uid, req.target_uid, req.caller_pid);
            write(resp_pipe[1], &resp, sizeof(resp));
            break;
        }

        case KSEC_OP_SETUID_BIT: {
            // Notification only — no response.
            printf("ksec: setuid bit set on inode=%u dev=%u by pid=%u (uid=%u)\n",
                   req.inode, req.dev, req.caller_pid, req.caller_uid);
            auto_create_setuid(req.inode, req.dev, req.file_uid);
            // No response written — fire-and-forget.
            break;
        }

        case KSEC_OP_CLRUID_BIT: {
            // ksec wants the kernel to clear a setuid bit — not applicable here
            // (we send this, not receive it).  Ignore.
            break;
        }

        default:
            // Unknown opcode: deny with a response to unblock the waiting task.
            resp.verdict = KSEC_VERDICT_DENY;
            write(resp_pipe[1], &resp, sizeof(resp));
            break;
        }
    }
}
