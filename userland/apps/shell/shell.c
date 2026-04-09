// MakaOS userland shell
// Input: reads raw PS/2 scancodes from /dev/kbdraw, does its own line editing.
// Output: writes to fd 1 (kernel fb terminal).
// No ANSI colors — kernel terminal does not interpret escape sequences.

#include "libc.h"
#include "stdio.h"

// ── Keyboard (raw scancodes, same as kernel shell) ────────────────────────

static int g_kbd_fd = -1;

// PS/2 set-1 scancode → ASCII (press events only, sc < 0x80).
static char sc_to_ascii(uint8_t sc) {
    if (sc & 0x80) return 0;  // release event — ignore
    static const char row1[] = "qwertyuiop";
    static const char row2[] = "asdfghjkl";
    static const char row3[] = "zxcvbnm";
    switch (sc) {
        case 0x1C: return '\n';
        case 0x39: return ' ';
        case 0x0E: return '\b';
        case 0x01: return 27;   // ESC
        // digits / symbols on top row
        case 0x02: return '1'; case 0x03: return '2'; case 0x04: return '3';
        case 0x05: return '4'; case 0x06: return '5'; case 0x07: return '6';
        case 0x08: return '7'; case 0x09: return '8'; case 0x0A: return '9';
        case 0x0B: return '0'; case 0x0C: return '-'; case 0x0D: return '=';
        case 0x1A: return '['; case 0x1B: return ']';
        case 0x27: return ';'; case 0x28: return '\'';
        case 0x29: return '`'; case 0x2B: return '\\';
        case 0x33: return ','; case 0x34: return '.'; case 0x35: return '/';
        default: break;
    }
    if (sc >= 0x10 && sc <= 0x19) return row1[sc - 0x10];
    if (sc >= 0x1E && sc <= 0x26) return row2[sc - 0x1E];
    if (sc >= 0x2C && sc <= 0x32) return row3[sc - 0x2C];
    return 0;
}

// Blocking: wait for next key press, return ASCII char (0 = unmapped/skip).
static char kbd_getchar(void) {
    for (;;) {
        uint8_t sc;
        ssize_t n = read(g_kbd_fd, &sc, 1);
        if (n <= 0) continue;
        if (sc == 0xE0) continue;  // extended prefix — skip
        char c = sc_to_ascii(sc);
        if (c) return c;
    }
}

// Read a full line from keyboard into buf (max-1 chars + NUL).
// Echoes chars and handles backspace. Returns length (without NUL).
static int read_line(char* buf, int max) {
    int len = 0;
    for (;;) {
        char c = kbd_getchar();
        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            write(1, "\n", 1);
            return len;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                write(1, "\b \b", 3);  // erase on terminal
            }
            continue;
        }
        if ((unsigned char)c < 0x20 || len >= max - 1) continue;
        buf[len++] = c;
        write(1, &c, 1);  // echo
    }
}

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
    // Kernel fb_clear is not directly callable; write form-feed as hint.
    // fb_term_putc handles '\f' as clear on our kernel terminal.
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

    dirent_t* entries = malloc(64 * sizeof(dirent_t));
    if (!entries) { puts_fd("ls: out of memory\n"); return; }

    int count = readdir(path, strlen(path), entries, 64);
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
        dirent_t* e = &entries[i];
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

    int r = chdir(resolved, strlen(resolved));
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
    int r = mkdir(path, strlen(path));
    if (r < 0) { puts_fd("mkdir: failed: "); puts_fd(path); putc_fd('\n'); }
}

static void cmd_rm(const char* cwd, int argc, char* argv[]) {
    if (argc < 2) { puts_fd("Usage: rm <path>\n"); return; }
    char path[256];
    resolve_path(cwd, argv[1], path, sizeof(path));
    int r = unlink(path, strlen(path));
    if (r < 0) { puts_fd("rm: failed: "); puts_fd(path); putc_fd('\n'); }
}

static void cmd_mv(const char* cwd, int argc, char* argv[]) {
    if (argc < 3) { puts_fd("Usage: mv <src> <dst>\n"); return; }
    char src[256], dst[256];
    resolve_path(cwd, argv[1], src, sizeof(src));
    resolve_path(cwd, argv[2], dst, sizeof(dst));
    int r = sys_rename(src, strlen(src), dst, strlen(dst));
    if (r < 0) { puts_fd("mv: failed: "); puts_fd(src); puts_fd(" -> "); puts_fd(dst); putc_fd('\n'); }
}

static void cmd_about(void) {
    puts_fd("MakaOS\n");
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

    // Read raw scancodes directly; track Ctrl key state to detect Ctrl+S.
    // Left Ctrl = scancode 0x1D (press), 0x9D (release).
    int ctrl_held = 0;
    for (;;) {
        uint8_t sc;
        ssize_t n = read(g_kbd_fd, &sc, 1);
        if (n <= 0) continue;
        if (sc == 0xE0) continue;

        // Track Ctrl key.
        if (sc == 0x1D) { ctrl_held = 1; continue; }
        if (sc == 0x9D) { ctrl_held = 0; continue; }

        if (sc & 0x80) continue;  // other release events — ignore

        // ESC (scancode 0x01) — quit.
        if (sc == 0x01) {
            puts_fd("\n[Quit without saving]\n");
            break;
        }

        // Ctrl+S (s = scancode 0x1F) — save.
        if (ctrl_held && sc == 0x1F) {
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

        char c = sc_to_ascii(sc);
        if (!c) continue;

        if (c == '\b') {
            if (len > 0) { len--; write(1, "\b \b", 3); }
            continue;
        }

        char to_append = 0;
        if (c == '\n') to_append = '\n';
        else if ((unsigned char)c >= 0x20) to_append = c;
        if (!to_append) continue;

        if (len + 1 >= buf_size) {
            uint32_t new_size = buf_size * 2;
            char* grown = realloc(buf, new_size);
            if (!grown) { puts_fd("\n[OOM — save now!]\n"); continue; }
            buf = grown;
            buf_size = new_size;
        }
        buf[len++] = to_append;
        write(1, &to_append, 1);
    }
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
        stat_t st;
        if (stat(path, strlen(path), &st) < 0) {
            resolve_path(cwd, cmd, path, sizeof(path));
            if (stat(path, strlen(path), &st) < 0) {
                puts_fd("shell: command not found: "); puts_fd(cmd); putc_fd('\n');
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

    int pid = fork();
    if (pid < 0) { puts_fd("shell: fork failed\n"); return; }
    if (pid == 0) {
        execve(path, child_argv, child_envp);
        puts_fd("shell: exec failed: "); puts_fd(path); putc_fd('\n');
        exit(1);
    }

    // Parent: wait for child.
    int status = 0;
    waitpid(pid, &status, 0);
}

// ── Main loop ─────────────────────────────────────────────────────────────

int main(void) {
    g_kbd_fd = open("/dev/kbdraw", O_RDONLY, 0);
    if (g_kbd_fd < 0) {
        puts_fd("shell: failed to open /dev/kbdraw\n");
        exit(1);
    }

    char cwd[256] = "/";
    getcwd(cwd, sizeof(cwd));

    cmd_clear();
    puts_fd("MakaOS Shell v0.2 -- type 'help' for commands\n\n");

    char line[256];
    char* argv[16];

    for (;;) {
        puts_fd("MakaOS:");
        puts_fd(cwd);
        puts_fd("> ");

        int n = read_line(line, sizeof(line));
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
