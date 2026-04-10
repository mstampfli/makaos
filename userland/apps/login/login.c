// ── login — MakaOS console login ─────────────────────────────────────────
//
// Reads /etc/passwd and /etc/shadow to authenticate a user.
// On success: drops privileges to the user's uid/gid, chdirs to home,
// and exec-spawns /bin/shell as the user's session.
//
// /etc/passwd format (colon-separated, one user per line):
//   username:uid:gid:home:shell
//   root:0:0:/root:/bin/shell
//   maka:1000:1000:/home/maka:/bin/shell
//
// /etc/shadow format:
//   username:password
//   root:toor
//   maka:maka
//
// No crypto — plaintext passwords for now.  ksec/PAM replace this later.

#include "../../libc/libc.h"

#define PASSWD_PATH  "/etc/passwd"
#define SHADOW_PATH  "/etc/shadow"
#define MAX_USERS    32
#define MAX_LINE     256

typedef struct {
    char     name[64];
    uint32_t uid;
    uint32_t gid;
    char     home[128];
    char     shell[128];
} passwd_entry_t;

typedef struct {
    char name[64];
    char password[128];
} shadow_entry_t;

static passwd_entry_t s_passwd[MAX_USERS];
static shadow_entry_t s_shadow[MAX_USERS];
static int            s_npasswd = 0;
static int            s_nshadow = 0;

// ── String helpers ────────────────────────────────────────────────────────

static int s_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static void s_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int s_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int s_atoi(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

// ── File reading ──────────────────────────────────────────────────────────

static int read_file(const char* path, char* buf, int max) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, (size_t)(max - 1));
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

// ── Line parser — walks `buf` line by line, calls cb(line, data) ──────────

typedef void (*line_cb_t)(char* line, void* data);

static void foreach_line(char* buf, line_cb_t cb, void* data) {
    char* p = buf;
    while (*p) {
        char* start = p;
        while (*p && *p != '\n') p++;
        char save = *p;
        *p = '\0';
        // skip blank lines and comments
        if (start[0] != '\0' && start[0] != '#')
            cb(start, data);
        *p = save ? save : '\0';
        if (*p) p++;
    }
}

// ── /etc/passwd parser ────────────────────────────────────────────────────
// Format: name:uid:gid:home:shell

static void parse_passwd_line(char* line, void* data) {
    (void)data;
    if (s_npasswd >= MAX_USERS) return;

    passwd_entry_t* e = &s_passwd[s_npasswd];
    char* fields[5];
    int nf = 0;
    char* p = line;
    fields[nf++] = p;
    while (*p && nf < 5) {
        if (*p == ':') { *p = '\0'; fields[nf++] = p + 1; }
        p++;
    }
    if (nf < 5) return;

    s_strcpy(e->name,  fields[0], sizeof(e->name));
    e->uid = (uint32_t)s_atoi(fields[1]);
    e->gid = (uint32_t)s_atoi(fields[2]);
    s_strcpy(e->home,  fields[3], sizeof(e->home));
    s_strcpy(e->shell, fields[4], sizeof(e->shell));
    s_npasswd++;
}

// ── /etc/shadow parser ────────────────────────────────────────────────────
// Format: name:password

static void parse_shadow_line(char* line, void* data) {
    (void)data;
    if (s_nshadow >= MAX_USERS) return;

    shadow_entry_t* e = &s_shadow[s_nshadow];
    char* p = line;
    char* name = p;
    while (*p && *p != ':') p++;
    if (!*p) return;
    *p++ = '\0';

    s_strcpy(e->name,     name, sizeof(e->name));
    s_strcpy(e->password, p,    sizeof(e->password));
    s_nshadow++;
}

// ── Lookup helpers ────────────────────────────────────────────────────────

static passwd_entry_t* find_passwd(const char* name) {
    for (int i = 0; i < s_npasswd; i++)
        if (s_strcmp(s_passwd[i].name, name) == 0) return &s_passwd[i];
    return (passwd_entry_t*)0;
}

static shadow_entry_t* find_shadow(const char* name) {
    for (int i = 0; i < s_nshadow; i++)
        if (s_strcmp(s_shadow[i].name, name) == 0) return &s_shadow[i];
    return (shadow_entry_t*)0;
}

// ── Terminal: read password without echo ──────────────────────────────────

static int read_password(char* buf, int max) {
    // Disable echo via TIOCGETA/TIOCSETA.
    // We don't have tcgetattr wrappers yet — read char by char via raw read.
    // For now: just read normally (echo visible). TODO: suppress echo via ioctl.
    int n = 0;
    while (n < max - 1) {
        char c;
        int r = (int)read(0, &c, 1);
        if (r <= 0) break;
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 127) {
            if (n > 0) { n--; write(1, "\b \b", 3); }
            continue;
        }
        buf[n++] = c;
        // No echo for password
    }
    buf[n] = '\0';
    write(1, "\n", 1);
    return n;
}

// ── Main ──────────────────────────────────────────────────────────────────

int main(void) {
    static char passwd_buf[4096];
    static char shadow_buf[4096];

    // Load user database.
    if (read_file(PASSWD_PATH, passwd_buf, sizeof(passwd_buf)) < 0) {
        write(1, "login: cannot open /etc/passwd\n", 31);
        return 1;
    }
    foreach_line(passwd_buf, parse_passwd_line, (void*)0);

    // /etc/shadow: only root can read it (mode 0640 root:shadow or 0600).
    // We're currently root, so this works.
    if (read_file(SHADOW_PATH, shadow_buf, sizeof(shadow_buf)) < 0) {
        write(1, "login: cannot open /etc/shadow\n", 31);
        return 1;
    }
    foreach_line(shadow_buf, parse_shadow_line, (void*)0);

    // Login loop — retry on bad credentials.
    char username[64];
    char password[128];

    for (;;) {
        // Prompt for username.
        write(1, "\nKore login: ", 13);
        int un = (int)read(0, username, sizeof(username) - 1);
        if (un <= 0) continue;
        // Strip trailing newline.
        while (un > 0 && (username[un-1] == '\n' || username[un-1] == '\r')) un--;
        username[un] = '\0';
        if (un == 0) continue;

        // Prompt for password.
        write(1, "Password: ", 10);
        read_password(password, sizeof(password));

        // Lookup user.
        passwd_entry_t* pw = find_passwd(username);
        if (!pw) {
            write(1, "Login incorrect.\n", 17);
            continue;
        }

        // Check password.
        shadow_entry_t* sh = find_shadow(username);
        if (sh) {
            if (s_strcmp(sh->password, password) != 0) {
                write(1, "Login incorrect.\n", 17);
                continue;
            }
        }
        // If no shadow entry: allow login without password (not recommended,
        // but lets root in even if shadow is misconfigured).

        // ── Credentials drop ────────────────────────────────────────────
        // Order matters: setgid BEFORE setuid (once uid != 0 we may lose
        // the ability to setgid to arbitrary groups).
        setgid(pw->gid);
        setgroups(0, (uint32_t*)0);   // clear supplemental groups
        setuid(pw->uid);

        // ── Change to home directory ─────────────────────────────────────
        int homelen = s_strlen(pw->home);
        if (chdir(pw->home, (size_t)homelen) < 0) {
            // Fall back to / if home doesn't exist yet.
            chdir("/", 1);
        }

        // ── Print welcome ────────────────────────────────────────────────
        write(1, "Welcome, ", 9);
        write(1, username, s_strlen(username));
        write(1, "!\n", 2);

        // ── Exec shell as this user ──────────────────────────────────────
        // spawn inherits our (now-dropped) credentials via cred_copy in
        // elf_exec_from_ext2.  The child shell runs as uid=pw->uid.
        const char* sh_argv[] = { pw->shell, (char*)0 };
        int inherit_stdio[3] = { -1, -1, -1 };
        int pid = spawn(pw->shell, sh_argv, (void*)0, inherit_stdio);
        if (pid < 0) {
            write(1, "login: failed to exec shell\n", 28);
            return 1;
        }

        // Wait for shell to exit, then re-show login prompt.
        int status = 0;
        waitpid(pid, &status, 0);

        // Re-read databases for next login (they may have changed).
        s_npasswd = 0; s_nshadow = 0;
        if (read_file(PASSWD_PATH, passwd_buf, sizeof(passwd_buf)) >= 0)
            foreach_line(passwd_buf, parse_passwd_line, (void*)0);
        if (read_file(SHADOW_PATH, shadow_buf, sizeof(shadow_buf)) >= 0)
            foreach_line(shadow_buf, parse_shadow_line, (void*)0);
    }

    return 0;
}
