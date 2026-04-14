// MakaOS userland shell
// Input: reads from stdin (fd 0) which is /dev/tty0 — the N_TTY TTY.
// In canonical mode the TTY handles echo, backspace, ^C, ^Z, line buffering.
// The editor switches to raw mode via tcsetattr to read individual keystrokes.

#include "libc.h"
#include "stdio.h"

// ── Output helpers ────────────────────────────────────────────────────────

static void puts_fd(const char* s) {
    size_t len = strlen(s);
    write(1, s, len);
}

static void putc_fd(char c) {
    write(1, &c, 1);
}

static void putu32(uint32_t v) {
    char buf[12];
    buf[11] = '\0';
    if (v == 0) { putc_fd('0'); return; }
    int i = 11;
    while (v && i > 0) { buf[--i] = '0' + (char)(v % 10); v /= 10; }
    puts_fd(&buf[i]);
}

// ── read_line: canonical line from stdin ─────────────────────────────────
// The TTY is in ICANON mode: it echoes chars, handles backspace, and returns
// a full line (including the trailing '\n') on Enter.  Strip the '\n' here.

static int read_line(char* buf, int max) {
    ssize_t n = read(0, buf, max - 1);
    if (n <= 0) return 0;
    if (buf[n - 1] == '\n') n--;
    buf[n] = '\0';
    return (int)n;
}

// ── Path resolution ───────────────────────────────────────────────────────

static void resolve_path(const char* cwd, const char* arg, char* out, uint32_t outsz) {
    char raw[512];
    if (!arg || arg[0] == '\0') {
        strncpy(raw, cwd, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
    } else if (arg[0] == '/') {
        strncpy(raw, arg, sizeof(raw) - 1);
        raw[sizeof(raw) - 1] = '\0';
    } else {
        snprintf(raw, sizeof(raw), "%s%s%s",
                 cwd,
                 (cwd[strlen(cwd) - 1] == '/') ? "" : "/",
                 arg);
    }

    const char* segs[64];
    int nseg = 0;
    char tmp[512];
    strncpy(tmp, raw, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char* p = tmp;
    if (*p == '/') p++;
    while (*p) {
        char* start = p;
        while (*p && *p != '/') p++;
        if (*p == '/') *p++ = '\0';

        if (start[0] == '\0' || (start[0] == '.' && start[1] == '\0')) {
            // skip
        } else if (start[0] == '.' && start[1] == '.' && start[2] == '\0') {
            if (nseg > 0) nseg--;
        } else if (nseg < 64) {
            segs[nseg++] = start;
        }
    }

    uint32_t pos = 0;
    out[pos++] = '/';
    for (int i = 0; i < nseg && pos + 1 < outsz; i++) {
        if (i > 0 && pos + 1 < outsz) out[pos++] = '/';
        for (uint32_t j = 0; segs[i][j] && pos + 1 < outsz; j++)
            out[pos++] = segs[i][j];
    }
    out[pos] = '\0';
}

// ── Command parsing ───────────────────────────────────────────────────────

static int parse_args(char* line, char* argv[], int max_argc) {
    int argc = 0;
    char* p = line;
    while (*p && argc < max_argc) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

// ── Commands ──────────────────────────────────────────────────────────────

static void cmd_help(void) {
    puts_fd("Available commands:\n");
    puts_fd("  help           Show this help\n");
    puts_fd("  clear          Clear the screen\n");
    puts_fd("  echo [...]     Print arguments\n");
    puts_fd("  ls [path]      List directory\n");
    puts_fd("  cat <file>     Print file contents\n");
    puts_fd("  edit <file>    Edit a file (Ctrl+S save, ESC quit)\n");
    puts_fd("  cd [path]      Change directory\n");
    puts_fd("  pwd            Print working directory\n");
    puts_fd("  mkdir <path>   Create directory\n");
    puts_fd("  rm <path>      Remove file\n");
    puts_fd("  mv <src> <dst> Rename/move file\n");
    puts_fd("  about          About MakaOS\n");
    puts_fd("  reboot         Reboot the system\n");
    puts_fd("  <name>         Run /bin/<name> or a path directly\n");
}

static void cmd_clear(void) {
    putc_fd('\f');
}

static void cmd_echo(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) putc_fd(' ');
        puts_fd(argv[i]);
    }
    putc_fd('\n');
}

static void cmd_ls(const char* cwd, int argc, char* argv[]) {
    char path[256];
    if (argc >= 2)
        resolve_path(cwd, argv[1], path, sizeof(path));
    else
        strncpy(path, cwd, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    k_dirent_t* entries = malloc(64 * sizeof(k_dirent_t));
    if (!entries) { puts_fd("ls: out of memory\n"); return; }

    int count = _sys_readdir(path, strlen(path), entries, 64);
    if (count < 0) {
        puts_fd("ls: cannot read: ");
        puts_fd(path);
        putc_fd('\n');
        free(entries);
        return;
    }
    if (count == 0) {
        puts_fd("(empty)\n");
        free(entries);
        return;
    }

    for (int i = 0; i < count; i++) {
        k_dirent_t* e = &entries[i];
        uint32_t nlen = (uint32_t)strlen(e->name);
        if (e->is_dir) {
            putc_fd('[');
            puts_fd(e->name);
            putc_fd(']');
            nlen += 2;
        } else {
            puts_fd(e->name);
        }
        for (uint32_t j = nlen; j < 22; j++) putc_fd(' ');
        if (e->is_dir) {
            puts_fd("<DIR>");
        } else {
            putu32(e->size);
            puts_fd(" bytes");
        }
        putc_fd('\n');
    }
    free(entries);
}

static void cmd_cat(const char* cwd, int argc, char* argv[]) {
    if (argc < 2) { puts_fd("Usage: cat <file>\n"); return; }
    char path[256];
    resolve_path(cwd, argv[1], path, sizeof(path));

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        puts_fd("cat: not found: "); puts_fd(path); putc_fd('\n');
        return;
    }

    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);
    close(fd);
    putc_fd('\n');
}

static void cmd_cd(char* cwd, int argc, char* argv[]) {
    const char* target = (argc >= 2) ? argv[1] : "/";
    char resolved[256];
    resolve_path(cwd, target, resolved, sizeof(resolved));

    int r = chdir(resolved);
    if (r < 0) {
        puts_fd("cd: not found: "); puts_fd(resolved); putc_fd('\n');
        return;
    }
    strncpy(cwd, resolved, 256);
    cwd[255] = '\0';
}

static void cmd_pwd(const char* cwd) {
    puts_fd(cwd);
    putc_fd('\n');
}

static void cmd_mkdir(const char* cwd, int argc, char* argv[]) {
    if (argc < 2) { puts_fd("Usage: mkdir <path>\n"); return; }
    char path[256];
    resolve_path(cwd, argv[1], path, sizeof(path));
    int r = mkdir(path, 0755);
    if (r < 0) { puts_fd("mkdir: failed: "); puts_fd(path); putc_fd('\n'); }
}

static void cmd_rm(const char* cwd, int argc, char* argv[]) {
    if (argc < 2) { puts_fd("Usage: rm <path>\n"); return; }
    char path[256];
    resolve_path(cwd, argv[1], path, sizeof(path));
    int r = unlink(path);
    if (r < 0) { puts_fd("rm: failed: "); puts_fd(path); putc_fd('\n'); }
}

static void cmd_mv(const char* cwd, int argc, char* argv[]) {
    if (argc < 3) { puts_fd("Usage: mv <src> <dst>\n"); return; }
    char src[256], dst[256];
    resolve_path(cwd, argv[1], src, sizeof(src));
    resolve_path(cwd, argv[2], dst, sizeof(dst));
    int r = rename(src, dst);
    if (r < 0) { puts_fd("mv: failed: "); puts_fd(src); puts_fd(" -> "); puts_fd(dst); putc_fd('\n'); }
}

static void cmd_about(void) {
    puts_fd("MakaOS\n");
    puts_fd("  Version  : 0.2.0\n");
    puts_fd("  Arch     : x86-64\n");
    puts_fd("  Boot     : UEFI + GOP framebuffer\n");
    puts_fd("  Features : PMM (buddy), VMM (demand paging), kheap,\n");
    puts_fd("             MLFQ scheduler, ext2, ring-3 ELF loader,\n");
    puts_fd("             N_TTY line discipline, IOAPIC/MSI, HDA, virtio-net\n");
}

static void cmd_reboot(void) {
    puts_fd("Rebooting...\n");
    syscall0(SYS_REBOOT);
    for (;;);
}

// ── Editor ────────────────────────────────────────────────────────────────
// Switches stdin to raw mode (no echo, no line buffering) so individual
// keystrokes are readable.  Ctrl+S (0x13) saves; ESC (0x1B) quits.
// Restores canonical mode on exit.

static void cmd_edit(const char* cwd, int argc, char* argv[]) {
    if (argc < 2) { puts_fd("Usage: edit <file>\n"); return; }
    char path[256];
    resolve_path(cwd, argv[1], path, sizeof(path));

    uint32_t buf_size = 4096;
    char* buf = malloc(buf_size);
    if (!buf) { puts_fd("edit: out of memory\n"); return; }
    uint32_t len = 0;

    // Pre-load existing file.
    int rfd = open(path, O_RDONLY, 0);
    if (rfd >= 0) {
        char tmp[512];
        ssize_t n;
        while ((n = read(rfd, tmp, sizeof(tmp))) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (len + 1 >= buf_size) {
                    uint32_t new_size = buf_size * 2;
                    char* grown = realloc(buf, new_size);
                    if (!grown) goto preload_done;
                    buf = grown;
                    buf_size = new_size;
                }
                buf[len++] = tmp[i];
            }
        }
    preload_done:
        close(rfd);
    }

    puts_fd("-- EDITOR: "); puts_fd(path); puts_fd(" | Ctrl+S=save  ESC=quit --\n");
    if (len > 0) write(1, buf, len);

    // Switch to raw mode: no ICANON, no ECHO, VMIN=1, VTIME=0.
    termios_t saved, raw;
    tcgetattr(0, &saved);
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSETSF, &raw);

    for (;;) {
        char c;
        if (read(0, &c, 1) <= 0) continue;

        // ESC — quit without saving.
        if ((unsigned char)c == 0x1B) {
            puts_fd("\n[Quit without saving]\n");
            break;
        }

        // Ctrl+S (0x13) — save.
        if ((unsigned char)c == 0x13) {
            int wfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
            int ok = 0;
            if (wfd >= 0) {
                ssize_t written = write(wfd, buf, len);
                ok = (written == (ssize_t)len);
                close(wfd);
            }
            puts_fd(ok ? "\n[Saved]\n" : "\n[Save FAILED]\n");
            continue;
        }

        // Backspace (0x08 or 0x7F).
        if (c == '\b' || (unsigned char)c == 0x7F) {
            if (len > 0) { len--; write(1, "\b \b", 3); }
            continue;
        }

        // Regular printable or newline.
        if ((unsigned char)c >= 0x20 || c == '\n' || c == '\r') {
            char to_append = (c == '\r') ? '\n' : c;
            if (len + 1 >= buf_size) {
                uint32_t new_size = buf_size * 2;
                char* grown = realloc(buf, new_size);
                if (!grown) { puts_fd("\n[OOM -- save now!]\n"); continue; }
                buf = grown;
                buf_size = new_size;
            }
            buf[len++] = to_append;
            write(1, &to_append, 1);
        }
    }

    // Restore canonical mode.
    tcsetattr(0, TCSETSF, &saved);
    free(buf);
}

// ── Run a binary ──────────────────────────────────────────────────────────

static void cmd_run(const char* cwd, int argc, char* argv[]) {
    char path[256];
    const char* cmd = argv[0];

    if (cmd[0] != '/') {
        snprintf(path, sizeof(path), "/bin/%s", cmd);
        struct stat st;
        if (stat(path, &st) < 0) {
            resolve_path(cwd, cmd, path, sizeof(path));
            if (stat(path, &st) < 0) {
                puts_fd("shell: command not found: "); puts_fd(cmd); putc_fd('\n');
                return;
            }
        }
    } else {
        resolve_path(cwd, cmd, path, sizeof(path));
    }

    (void)argc;

    // spawn: create a fresh task — inherit our own stdin/stdout/stderr.
    // Pass -i to force interactive mode (bash checks isatty but our tty
    // detection may not be perfect, so be explicit).
    static const int inherit_stdio[3] = { -1, -1, -1 };
    const char* bash_argv[] = { path, "-i", NULL };
    // Only pass -i if the binary looks like bash; otherwise pass NULL argv.
    int is_bash = 0;
    { const char* p = path; while (*p) p++; while (p > path && *p != '/') p--; if (*p == '/') p++;
      is_bash = (p[0]=='b' && p[1]=='a' && p[2]=='s' && p[3]=='h' && !p[4]); }
    int pid = spawn(path, is_bash ? bash_argv : NULL, NULL, inherit_stdio, NULL);
    if (pid < 0) { puts_fd("shell: spawn failed: "); puts_fd(path); putc_fd('\n'); return; }

    int status = 0;
    waitpid(pid, &status, 0);
    // Debug: print exit status so we can see why a program exited immediately
    if (WIFEXITED(status)) {
        puts_fd("[shell: exited with code ");
        char nbuf[12]; int n = WEXITSTATUS(status); int pos = 0;
        if (n == 0) { nbuf[pos++] = '0'; }
        else { int tmp = n; int digits = 0; while (tmp) { tmp /= 10; digits++; } pos = digits; tmp = n; while (tmp) { nbuf[--pos] = '0' + (tmp % 10); tmp /= 10; } pos = digits; }
        for (int i = 0; i < pos; i++) putc_fd(nbuf[i]);
        puts_fd("]\n");
    }
}

// ── Main loop ─────────────────────────────────────────────────────────────

int main(void) {
    // stdin/stdout/stderr are already /dev/tty0 (set up by the ELF loader).
    // tty0 starts in canonical+echo mode: the TTY handles line editing.

    // Set ourselves as the controlling terminal's foreground process group.
    uint32_t my_pgid = (uint32_t)getpgrp();
    ioctl(0, TIOCSPGRP, &my_pgid);

    char cwd[256] = "/";
    getcwd(cwd, sizeof(cwd));

    cmd_clear();
    puts_fd("MakaOS Shell v0.3 -- type 'help' for commands\n\n");

    char line[256];
    char* argv[16];

    for (;;) {
        puts_fd("MakaOS:");
        puts_fd(cwd);
        puts_fd("> ");

        int n = read_line(line, sizeof(line));
        if (n <= 0) continue;

        int argc = parse_args(line, argv, 16);
        if (argc == 0) continue;

        if      (strcmp(argv[0], "help")  == 0) cmd_help();
        else if (strcmp(argv[0], "clear") == 0) cmd_clear();
        else if (strcmp(argv[0], "echo")  == 0) cmd_echo(argc, argv);
        else if (strcmp(argv[0], "ls")    == 0) cmd_ls(cwd, argc, argv);
        else if (strcmp(argv[0], "cat")   == 0) cmd_cat(cwd, argc, argv);
        else if (strcmp(argv[0], "cd")    == 0) cmd_cd(cwd, argc, argv);
        else if (strcmp(argv[0], "pwd")   == 0) cmd_pwd(cwd);
        else if (strcmp(argv[0], "mkdir") == 0) cmd_mkdir(cwd, argc, argv);
        else if (strcmp(argv[0], "rm")    == 0) cmd_rm(cwd, argc, argv);
        else if (strcmp(argv[0], "mv")    == 0) cmd_mv(cwd, argc, argv);
        else if (strcmp(argv[0], "edit")  == 0) cmd_edit(cwd, argc, argv);
        else if (strcmp(argv[0], "about") == 0) cmd_about();
        else if (strcmp(argv[0], "reboot")== 0) cmd_reboot();
        else cmd_run(cwd, argc, argv);
    }
}
