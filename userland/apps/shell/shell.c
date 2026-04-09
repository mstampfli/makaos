// MakaOS userland shell
// Reads from fd 0 (cooked TTY), writes to fd 1 (kernel terminal).
// All filesystem ops go through syscalls. No kernel internals.

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

// ANSI color helpers — kernel terminal interprets these.
static void color(const char* ansi) { puts_fd(ansi); }
#define COL_RESET  "\033[0m"
#define COL_GREEN  "\033[32m"
#define COL_CYAN   "\033[36m"
#define COL_YELLOW "\033[33m"
#define COL_RED    "\033[31m"
#define COL_LBLUE  "\033[94m"

// ── Path resolution ───────────────────────────────────────────────────────
// Resolve arg relative to cwd, normalize . and .. components.

static void resolve_path(const char* cwd, const char* arg, char* out, uint32_t outsz) {
    // Build raw absolute path.
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

    // Normalize: process . and .. components.
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
        } else {
            if (nseg < 64) segs[nseg++] = start;
        }
    }

    // Reconstruct.
    out[0] = '/';
    uint32_t pos = 1;
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
    color(COL_CYAN);
    puts_fd("Available commands:\n");
    color(COL_RESET);
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
    puts_fd("  <path>         Run an ELF binary (bare name searches /bin/)\n");
}

static void cmd_clear(void) {
    puts_fd("\033[2J\033[H");
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

    dirent_t* entries = malloc(64 * sizeof(dirent_t));
    if (!entries) { puts_fd("ls: out of memory\n"); return; }

    int count = readdir(path, strlen(path), entries, 64);
    if (count < 0) {
        color(COL_RED);
        puts_fd("ls: cannot read: ");
        puts_fd(path);
        putc_fd('\n');
        color(COL_RESET);
        free(entries);
        return;
    }
    if (count == 0) {
        puts_fd("(empty)\n");
        free(entries);
        return;
    }

    for (int i = 0; i < count; i++) {
        dirent_t* e = &entries[i];
        uint32_t nlen = (uint32_t)strlen(e->name);
        if (e->is_dir) {
            color(COL_YELLOW);
            putc_fd('[');
            puts_fd(e->name);
            putc_fd(']');
            color(COL_RESET);
            nlen += 2;
        } else {
            puts_fd(e->name);
        }
        for (uint32_t j = nlen; j < 22; j++) putc_fd(' ');
        if (e->is_dir) {
            color(COL_YELLOW);
            puts_fd("<DIR>");
            color(COL_RESET);
        } else {
            putu32(e->size);
            puts_fd(" bytes");
        }
        putc_fd('\n');
    }
    free(entries);
}

static void cmd_cat(const char* cwd, int argc, char* argv[]) {
    if (argc < 2) {
        color(COL_RED); puts_fd("Usage: cat <file>\n"); color(COL_RESET);
        return;
    }
    char path[256];
    resolve_path(cwd, argv[1], path, sizeof(path));

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        color(COL_RED);
        puts_fd("cat: not found: "); puts_fd(path); putc_fd('\n');
        color(COL_RESET);
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

    int r = chdir(resolved, strlen(resolved));
    if (r < 0) {
        color(COL_RED);
        puts_fd("cd: not found: "); puts_fd(resolved); putc_fd('\n');
        color(COL_RESET);
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
    if (argc < 2) {
        color(COL_RED); puts_fd("Usage: mkdir <path>\n"); color(COL_RESET);
        return;
    }
    char path[256];
    resolve_path(cwd, argv[1], path, sizeof(path));
    int r = mkdir(path, strlen(path));
    if (r < 0) {
        color(COL_RED);
        puts_fd("mkdir: failed: "); puts_fd(path); putc_fd('\n');
        color(COL_RESET);
    }
}

static void cmd_rm(const char* cwd, int argc, char* argv[]) {
    if (argc < 2) {
        color(COL_RED); puts_fd("Usage: rm <path>\n"); color(COL_RESET);
        return;
    }
    char path[256];
    resolve_path(cwd, argv[1], path, sizeof(path));
    int r = unlink(path, strlen(path));
    if (r < 0) {
        color(COL_RED);
        puts_fd("rm: failed: "); puts_fd(path); putc_fd('\n');
        color(COL_RESET);
    }
}

static void cmd_mv(const char* cwd, int argc, char* argv[]) {
    if (argc < 3) {
        color(COL_RED); puts_fd("Usage: mv <src> <dst>\n"); color(COL_RESET);
        return;
    }
    char src[256], dst[256];
    resolve_path(cwd, argv[1], src, sizeof(src));
    resolve_path(cwd, argv[2], dst, sizeof(dst));
    int r = sys_rename(src, strlen(src), dst, strlen(dst));
    if (r < 0) {
        color(COL_RED);
        puts_fd("mv: failed: "); puts_fd(src); puts_fd(" -> "); puts_fd(dst); putc_fd('\n');
        color(COL_RESET);
    }
}

static void cmd_about(void) {
    color(COL_CYAN);
    puts_fd("MakaOS\n");
    color(COL_RESET);
    puts_fd("  Version  : 0.2.0\n");
    puts_fd("  Arch     : x86-64\n");
    puts_fd("  Boot     : UEFI + GOP framebuffer\n");
    puts_fd("  Features : PMM (buddy), VMM (demand paging), kheap,\n");
    puts_fd("             MLFQ scheduler, ext2, ring-3 ELF loader,\n");
    puts_fd("             IOAPIC/MSI interrupts, HDA audio, virtio-net\n");
}

static void cmd_reboot(void) {
    puts_fd("Rebooting...\n");
    syscall0(SYS_REBOOT);
    for (;;);
}

// ── Editor ────────────────────────────────────────────────────────────────
// Ctrl+S (0x13) = save, ESC (0x1B) = quit without saving.
// Uses a heap-allocated buffer that doubles on overflow.

static void cmd_edit(const char* cwd, int argc, char* argv[]) {
    if (argc < 2) {
        color(COL_RED); puts_fd("Usage: edit <file>\n"); color(COL_RESET);
        return;
    }
    char path[256];
    resolve_path(cwd, argv[1], path, sizeof(path));

    uint32_t buf_size = 4096;
    char* buf = malloc(buf_size);
    if (!buf) { color(COL_RED); puts_fd("edit: out of memory\n"); color(COL_RESET); return; }
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

    // Print header + existing content.
    color(COL_LBLUE);
    puts_fd("-- EDITOR: "); puts_fd(path); puts_fd(" | Ctrl+S=save  ESC=quit --\n");
    color(COL_RESET);
    if (len > 0) write(1, buf, len);

    // Edit loop (cooked mode — kernel delivers one line at a time on Enter,
    // but we read single chars for Ctrl+S / ESC detection).
    // Switch to raw mode for the editor so we can intercept Ctrl+S / ESC.
    termios_t saved, raw;
    tcgetattr(0, &saved);
    raw = saved;
    raw.c_lflag &= ~(uint32_t)(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);

    for (;;) {
        char c;
        ssize_t n = read(0, &c, 1);
        if (n <= 0) continue;

        if (c == '\x1b') {
            color(COL_YELLOW);
            puts_fd("\n[Quit without saving]\n");
            color(COL_RESET);
            break;
        }

        if (c == '\x13') {
            // Save: open for write (create/trunc), write buffer, close.
            int wfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0);
            int ok = 0;
            if (wfd >= 0) {
                ssize_t written = write(wfd, buf, len);
                ok = (written == (ssize_t)len);
                close(wfd);
            }
            color(ok ? COL_GREEN : COL_RED);
            puts_fd(ok ? "\n[Saved]\n" : "\n[Save FAILED]\n");
            color(COL_RESET);
            continue;
        }

        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                // Erase char on terminal: backspace, space, backspace.
                write(1, "\b \b", 3);
            }
            continue;
        }

        char to_append = 0;
        if (c == '\n' || c == '\r') to_append = '\n';
        else if ((unsigned char)c >= 0x20) to_append = c;
        if (!to_append) continue;

        if (len + 1 >= buf_size) {
            uint32_t new_size = buf_size * 2;
            char* grown = realloc(buf, new_size);
            if (!grown) {
                color(COL_RED);
                puts_fd("\n[Out of memory — save now!]\n");
                color(COL_RESET);
                continue;
            }
            buf = grown;
            buf_size = new_size;
        }
        buf[len++] = to_append;
        write(1, &to_append, 1);
    }

    // Restore terminal.
    tcsetattr(0, TCSANOW, &saved);
    free(buf);
}

// ── Run a binary ──────────────────────────────────────────────────────────

static void cmd_run(const char* cwd, int argc, char* argv[]) {
    // argv[0] is the command — resolve to absolute path.
    char path[256];
    const char* cmd = argv[0];

    // If bare name (no '/'), try /bin/<cmd> first.
    if (cmd[0] != '/') {
        snprintf(path, sizeof(path), "/bin/%s", cmd);
        // Check it exists.
        stat_t st;
        if (stat(path, strlen(path), &st) < 0) {
            // Try relative to cwd.
            resolve_path(cwd, cmd, path, sizeof(path));
            if (stat(path, strlen(path), &st) < 0) {
                color(COL_RED);
                puts_fd("shell: command not found: "); puts_fd(cmd); putc_fd('\n');
                color(COL_RESET);
                return;
            }
        }
    } else {
        resolve_path(cwd, cmd, path, sizeof(path));
    }

    // Build child argv: argv[0] = path, rest from command line.
    const char* child_argv[64];
    child_argv[0] = path;
    int ca = 1;
    for (int i = 1; i < argc && ca < 63; i++) child_argv[ca++] = argv[i];
    child_argv[ca] = NULL;

    const char* child_envp[] = {
        "PATH=/bin",
        "HOME=/",
        "TERM=linux",
        NULL
    };

    // Fork + exec so the shell survives if exec fails or child exits.
    int pid = fork();
    if (pid < 0) {
        color(COL_RED); puts_fd("shell: fork failed\n"); color(COL_RESET);
        return;
    }
    if (pid == 0) {
        // Child: exec the binary.
        execve(path, child_argv, child_envp);
        // If we get here exec failed.
        color(COL_RED);
        puts_fd("shell: exec failed: "); puts_fd(path); putc_fd('\n');
        color(COL_RESET);
        exit(1);
    }

    // Parent: wait for child.
    int status = 0;
    waitpid(pid, &status, 0);
}

// ── Main loop ─────────────────────────────────────────────────────────────

int main(void) {
    char cwd[256] = "/";

    // Sync our local cwd with the kernel's (in case exec inherited a non-root cwd).
    getcwd(cwd, sizeof(cwd));

    cmd_clear();
    color(COL_CYAN);
    puts_fd("MakaOS Shell v0.2");
    color(COL_RESET);
    puts_fd(" -- type 'help' for commands\n\n");

    char line[256];
    char* argv[16];

    for (;;) {
        color(COL_GREEN);
        puts_fd("MakaOS:");
        puts_fd(cwd);
        puts_fd("> ");
        color(COL_RESET);

        ssize_t n = read(0, line, sizeof(line) - 1);
        if (n <= 0) continue;

        // Strip trailing newline.
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
        line[n] = '\0';
        if (n == 0) continue;

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
        else {
            // Try to run as a binary.
            cmd_run(cwd, argc, argv);
        }
    }
}
