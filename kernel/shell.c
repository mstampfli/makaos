#include "common.h"
#include "vfs.h"
#include "keyboard.h"
#include "ext2.h"
#include "elf.h"
#include "process.h"
#include "sched.h"
#include "kheap.h"

// ── VGA ──────────────────────────────────────────────────────────────────
#define VGA_COLS 80
#define VGA_ROWS 25

// g_vga is defined in idt.c.
extern volatile uint16_t* g_vga;

// g_vga_row/g_vga_col are defined in vfs.c (exported).
// Declared via vfs.h.

static uint8_t s_attr = 0x0F; // white on black

// ── String utilities ──────────────────────────────────────────────────────
static uint32_t kstrlen(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static int kstrcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void kstrcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// ── Terminal layer ────────────────────────────────────────────────────────
static void term_scroll(void) {
    for (uint32_t r = 0; r < VGA_ROWS - 1; r++)
        for (uint32_t c = 0; c < VGA_COLS; c++)
            g_vga[r * VGA_COLS + c] = g_vga[(r + 1) * VGA_COLS + c];
    for (uint32_t c = 0; c < VGA_COLS; c++)
        g_vga[(VGA_ROWS - 1) * VGA_COLS + c] = (uint16_t)' ' | ((uint16_t)s_attr << 8);
    g_vga_row = VGA_ROWS - 1;
}

static void term_putc(char c) {
    if (c == '\r') {
        g_vga_col = 0;
        return;
    }
    if (c == '\n') {
        g_vga_col = 0;
        g_vga_row++;
        if (g_vga_row >= VGA_ROWS) term_scroll();
        return;
    }
    if (c == '\b' || c == 127) {
        // Backspace: move back, erase, move back again.
        if (g_vga_col > 0) {
            g_vga_col--;
            g_vga[g_vga_row * VGA_COLS + g_vga_col] = (uint16_t)' ' | ((uint16_t)s_attr << 8);
        }
        return;
    }
    if (g_vga_col >= VGA_COLS) {
        g_vga_col = 0;
        g_vga_row++;
        if (g_vga_row >= VGA_ROWS) term_scroll();
    }
    g_vga[g_vga_row * VGA_COLS + g_vga_col] = (uint16_t)(uint8_t)c | ((uint16_t)s_attr << 8);
    g_vga_col++;
}

static void term_puts(const char* s) {
    for (; *s; s++) term_putc(*s);
}

static void term_puthex(uint64_t v, int digits) {
    const char* hex = "0123456789ABCDEF";
    char buf[17];
    buf[digits] = '\0';
    for (int i = digits - 1; i >= 0; i--) {
        buf[i] = hex[v & 0xF];
        v >>= 4;
    }
    term_puts(buf);
}

static void term_putu32(uint32_t v) {
    char buf[12];
    buf[11] = '\0';
    if (v == 0) { term_putc('0'); return; }
    int i = 11;
    while (v && i > 0) {
        buf[--i] = '0' + (char)(v % 10);
        v /= 10;
    }
    term_puts(&buf[i]);
}

static void term_clear(void) {
    uint8_t saved = s_attr;
    s_attr = 0x07;
    for (uint32_t i = 0; i < VGA_COLS * VGA_ROWS; i++)
        g_vga[i] = (uint16_t)' ' | ((uint16_t)s_attr << 8);
    g_vga_row = 0;
    g_vga_col = 0;
    s_attr = saved;
}

static void term_set_color(uint8_t attr) {
    s_attr = attr;
}

// ── Line input ────────────────────────────────────────────────────────────
static int read_line(char* buf, int max) {
    int len = 0;
    while (1) {
        char c = keyboard_wait();
        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            term_putc('\n');
            break;
        }
        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                term_putc('\b');
            }
            continue;
        }
        if ((unsigned char)c < 0x20 || len >= max - 1) continue;
        buf[len++] = c;
        term_putc(c);
    }
    return len;
}

// ── Path resolution helper ────────────────────────────────────────────────
// Resolve `arg` relative to g_cwd into an absolute path in `out`.
static void resolve_path(const char* arg, char* out, uint32_t out_size) {
    if (!arg || arg[0] == '\0') {
        kstrcpy(out, g_cwd, out_size);
        return;
    }

    if (arg[0] == '/') {
        // Already absolute.
        kstrcpy(out, arg, out_size);
        return;
    }

    // Relative — prepend g_cwd.
    uint32_t cwdlen = kstrlen(g_cwd);
    uint32_t pos = 0;

    // Copy g_cwd.
    for (uint32_t i = 0; i < cwdlen && pos + 1 < out_size; i++)
        out[pos++] = g_cwd[i];

    // Add separator if needed.
    if (pos > 0 && out[pos - 1] != '/' && pos + 1 < out_size)
        out[pos++] = '/';

    // Append arg.
    for (uint32_t i = 0; arg[i] && pos + 1 < out_size; i++)
        out[pos++] = arg[i];

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
    term_set_color(0x0B); // light cyan
    term_puts("Available commands:\n");
    term_set_color(0x0F);
    term_puts("  help         - Show this help message\n");
    term_puts("  clear        - Clear the screen\n");
    term_puts("  echo [...]   - Print arguments to screen\n");
    term_puts("  ls [path]    - List files in directory (default: cwd)\n");
    term_puts("  cat <file>   - Print contents of a file\n");
    term_puts("  run <file>   - Load and run an ELF binary\n");
    term_puts("  edit <file>  - Edit a file (Ctrl+S save, ESC quit)\n");
    term_puts("  cd <path>    - Change working directory\n");
    term_puts("  mkdir <path> - Create a directory\n");
    term_puts("  ps           - List running processes\n");
    term_puts("  about        - About MakaOS\n");
    term_puts("  reboot       - Reboot the system\n");
}

static void cmd_clear(void) {
    term_clear();
}

static void cmd_echo(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) term_putc(' ');
        term_puts(argv[i]);
    }
    term_putc('\n');
}

static void cmd_ls(int argc, char* argv[]) {
    static char ls_path[256];
    if (argc >= 2) {
        resolve_path(argv[1], ls_path, sizeof(ls_path));
    } else {
        kstrcpy(ls_path, g_cwd, sizeof(ls_path));
    }

    ext2_entry_t* entries = kmalloc(64 * sizeof(ext2_entry_t));
    if (!entries) { term_puts("ls: out of memory\n"); return; }

    int count = ext2_readdir(ls_path, entries, 64);
    if (count < 0) {
        term_set_color(0x0C);
        term_puts("ls: cannot read directory: ");
        term_puts(ls_path);
        term_putc('\n');
        term_set_color(0x0F);
        kfree(entries);
        return;
    }
    if (count == 0) {
        term_puts("(empty)\n");
        kfree(entries);
        return;
    }

    for (int i = 0; i < count; i++) {
        ext2_entry_t* e = &entries[i];
        // Print name left-padded to 20 chars.
        uint32_t nlen = kstrlen(e->name);
        if (e->is_dir) {
            term_set_color(0x0E); // yellow for dirs
            term_putc('[');
            term_puts(e->name);
            term_putc(']');
            term_set_color(0x0F);
            nlen += 2;
        } else {
            term_puts(e->name);
        }
        for (uint32_t j = nlen; j < 22; j++) term_putc(' ');
        if (e->is_dir) {
            term_set_color(0x0E);
            term_puts("<DIR>");
            term_set_color(0x0F);
        } else {
            term_putu32(e->size);
            term_puts(" bytes");
        }
        term_putc('\n');
    }

    kfree(entries);
}

static void cmd_cat(int argc, char* argv[]) {
    if (argc < 2) {
        term_set_color(0x0C);
        term_puts("Usage: cat <filename>\n");
        term_set_color(0x0F);
        return;
    }

    static char cat_path[256];
    resolve_path(argv[1], cat_path, sizeof(cat_path));

    vfs_file_t* f = ext2_open(cat_path);
    if (!f) {
        term_set_color(0x0C);
        term_puts("cat: file not found: ");
        term_puts(cat_path);
        term_putc('\n');
        term_set_color(0x0F);
        return;
    }

    uint8_t buf[512];
    int64_t n;
    char prev = 0;
    while ((n = vfs_read(f, buf, sizeof(buf))) > 0) {
        for (int64_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') { prev = c; continue; }
            if (c == '\n' && prev == '\r') { term_putc('\n'); prev = c; continue; }
            term_putc(c);
            prev = c;
        }
    }
    vfs_close(f);
    term_putc('\n');
}

static void cmd_run(int argc, char* argv[]) {
    if (argc < 2) {
        term_set_color(0x0C);
        term_puts("Usage: run <filename>\n");
        term_set_color(0x0F);
        return;
    }

    static char run_path[256];
    resolve_path(argv[1], run_path, sizeof(run_path));

    uint32_t pid = pid_alloc();
    task_t* child = elf_load_from_ext2(run_path, pid);
    if (!child) {
        term_set_color(0x0C);
        term_puts("run: failed to load: ");
        term_puts(run_path);
        term_putc('\n');
        term_set_color(0x0F);
        return;
    }

    uint32_t child_pid = child->pid;
    sched_add(child);

    term_set_color(0x0A); // light green
    term_puts("[Running PID ");
    term_putu32(child_pid);
    term_puts("]\n");
    term_set_color(0x0F);

    sched_wait_pid(child_pid);

    term_set_color(0x0A);
    term_puts("[Done PID ");
    term_putu32(child_pid);
    term_puts("]\n");
    term_set_color(0x0F);
}

static void cmd_cd(int argc, char* argv[]) {
    if (argc < 2) {
        // cd with no args goes to root.
        g_cwd[0] = '/';
        g_cwd[1] = '\0';
        return;
    }

    static char cd_path[256];
    resolve_path(argv[1], cd_path, sizeof(cd_path));

    // Verify it exists and is a directory.
    ext2_entry_t tmp[1];
    int r = ext2_readdir(cd_path, tmp, 1);
    if (r < 0) {
        // Try opening as a path — readdir returns -1 on error.
        // One special case: it might be "/" (root).
        // Also allow if it's a path that succeeds as readdir = 0 (empty dir).
        term_set_color(0x0C);
        term_puts("cd: not a directory or not found: ");
        term_puts(cd_path);
        term_putc('\n');
        term_set_color(0x0F);
        return;
    }

    kstrcpy(g_cwd, cd_path, sizeof(g_cwd));
}

static void cmd_mkdir(int argc, char* argv[]) {
    if (argc < 2) {
        term_set_color(0x0C);
        term_puts("Usage: mkdir <path>\n");
        term_set_color(0x0F);
        return;
    }

    static char mkdir_path[256];
    resolve_path(argv[1], mkdir_path, sizeof(mkdir_path));

    int ok = ext2_mkdir(mkdir_path);
    if (!ok) {
        term_set_color(0x0C);
        term_puts("mkdir: failed: ");
        term_puts(mkdir_path);
        term_putc('\n');
        term_set_color(0x0F);
    }
}

static void cmd_about(void) {
    term_set_color(0x0B);
    term_puts("MakaOS\n");
    term_set_color(0x0F);
    term_puts("  Version  : 0.1.2\n");
    term_puts("  Arch     : x86-64\n");
    term_puts("  Boot     : BIOS + custom bootloader (NASM)\n");
    term_puts("  Features : PMM (buddy), VMM (demand paging), kheap,\n");
    term_puts("             cooperative scheduler, ext2, ring-3 ELF loader\n");
}

typedef struct {
    int count;
} ps_ctx_t;

static const char* state_name(task_state_t st) {
    switch (st) {
        case TASK_READY:    return "READY  ";
        case TASK_RUNNING:  return "RUNNING";
        case TASK_DEAD:     return "DEAD   ";
        case TASK_SLEEPING: return "SLEEP  ";
        default:            return "?      ";
    }
}

static void ps_print_task(task_t* t, const char* role) {
    term_putu32(t->pid);
    term_putc(' ');
    // Pad pid to 5 chars.
    uint32_t pid_digits = 1;
    uint32_t v = t->pid;
    while (v >= 10) { pid_digits++; v /= 10; }
    for (uint32_t i = pid_digits; i < 5; i++) term_putc(' ');

    term_puts(state_name(t->state));
    term_putc(' ');
    term_puts(role);
    term_putc('\n');
}

static void ps_cb(task_t* t, void* data) {
    (void)data;
    ps_print_task(t, (t->flags & TASK_FLAG_KTHREAD) ? "kthread" : "user");
}

static void cmd_ps(void) {
    term_set_color(0x0B);
    term_puts("PID   STATE   TYPE\n");
    term_set_color(0x0F);
    // Print ourselves (the shell).
    extern task_t* g_current;
    if (g_current) ps_print_task(g_current, "shell (running)");
    // Print all queued tasks.
    sched_for_each(ps_cb, NULL);
}

static void cmd_reboot(void) {
    term_puts("Rebooting...\n");
    // Pulse keyboard controller reset line.
    outb(0x64, 0xFE);
    // If that didn't work, triple-fault.
    for (;;) __asm__ volatile("cli; hlt");
}

// ── Editor ────────────────────────────────────────────────────────────────
// Ctrl+S (0x13) = save,  ESC (0x1B) = quit without saving.
//
// Buffer grows dynamically: starts at 4 KiB, doubles each time it fills up.
// No fixed cap — grows until OOM.

// Grow the edit buffer by doubling.  Returns new buf ptr or NULL on OOM.
static uint8_t* edit_grow(uint8_t* old_buf, uint32_t* size, uint32_t len) {
    uint32_t new_size = *size * 2;
    uint8_t* new_buf = kmalloc(new_size);
    if (!new_buf) return NULL;

    for (uint32_t i = 0; i < len; i++) new_buf[i] = old_buf[i];
    kfree(old_buf);
    *size = new_size;
    return new_buf;
}

// Shrink the edit buffer by halving if len dropped below 1/4 capacity.
// Minimum size is 4096.  Silently keeps old buf on OOM (safe to ignore).
static uint8_t* edit_shrink(uint8_t* old_buf, uint32_t* size, uint32_t len) {
    if (*size <= 4096 || len >= *size / 4) return old_buf;

    uint32_t new_size = *size / 2;
    if (new_size < 4096) new_size = 4096;

    uint8_t* new_buf = kmalloc(new_size);
    if (!new_buf) return old_buf; // OOM: keep old, no data lost

    for (uint32_t i = 0; i < len; i++) new_buf[i] = old_buf[i];
    kfree(old_buf);
    *size = new_size;
    return new_buf;
}

static void cmd_edit(int argc, char* argv[]) {
    if (argc < 2) {
        term_set_color(0x0C);
        term_puts("Usage: edit <filename>\n");
        term_set_color(0x0F);
        return;
    }

    static char edit_path[256];
    resolve_path(argv[1], edit_path, sizeof(edit_path));

    uint32_t buf_size = 4096;
    uint8_t* buf      = kmalloc(buf_size);
    if (!buf) {
        term_set_color(0x0C);
        term_puts("edit: out of memory\n");
        term_set_color(0x0F);
        return;
    }
    uint32_t len = 0;

    // Pre-load existing file, growing buffer as needed.
    vfs_file_t* existing = ext2_open(edit_path);
    if (existing) {
        uint8_t tmp[512];
        int64_t n;
        while ((n = vfs_read(existing, tmp, sizeof(tmp))) > 0) {
            for (int64_t i = 0; i < n; i++) {
                if (len + 1 >= buf_size) {
                    uint8_t* grown = edit_grow(buf, &buf_size, len);
                    if (!grown) goto preload_done; // OOM: load what we have
                    buf = grown;
                }
                buf[len++] = tmp[i];
            }
        }
preload_done:
        vfs_close(existing);
    }

    // Header.
    term_set_color(0x1F);
    term_puts("-- EDITOR: ");
    term_puts(edit_path);
    term_puts(" | Ctrl+S=save  ESC=quit --");
    term_set_color(0x0F);
    term_putc('\n');

    if (len > 0) {
        term_set_color(0x07);
        for (uint32_t i = 0; i < len; i++) term_putc((char)buf[i]);
        term_set_color(0x0F);
    }

    // Edit loop — grow buffer whenever it fills up.
    for (;;) {
        char c = keyboard_wait();

        if (c == '\x1b') {
            term_set_color(0x0E);
            term_puts("\n[Quit without saving]\n");
            term_set_color(0x0F);
            break;
        }

        if (c == '\x13') {
            int ok = ext2_write_file(edit_path, buf, len);
            term_set_color(ok ? 0x0A : 0x0C);
            term_puts(ok ? "\n[Saved]\n" : "\n[Save FAILED]\n");
            term_set_color(0x0F);
            continue;
        }

        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                term_putc('\b');
                buf = edit_shrink(buf, &buf_size, len);
            }
            continue;
        }

        char to_append = 0;
        if (c == '\n' || c == '\r') to_append = '\n';
        else if ((unsigned char)c >= 0x20) to_append = c;

        if (!to_append) continue;

        // Grow buffer if full.
        if (len + 1 >= buf_size) {
            uint8_t* grown = edit_grow(buf, &buf_size, len);
            if (!grown) {
                term_set_color(0x0C);
                term_puts("\n[Out of memory — save now!]\n");
                term_set_color(0x0F);
                continue; // don't append, but don't crash
            }
            buf = grown;
        }

        buf[len++] = (uint8_t)to_append;
        term_putc(to_append);
    }

    kfree(buf);
}

// ── Shell main loop ───────────────────────────────────────────────────────
void shell_fn(void) {
    term_clear();
    term_set_color(0x0B);
    term_puts("MakaOS Shell v0.2");
    term_set_color(0x0F);
    term_puts(" -- type 'help' for commands\n\n");

    char line[256];
    char* argv[16];

    for (;;) {
        // Show cwd in prompt.
        term_set_color(0x0A); // green
        term_puts("MakaOS:");
        term_puts(g_cwd);
        term_puts("> ");
        term_set_color(0x0F);

        int len = read_line(line, sizeof(line));
        if (len == 0) continue;

        int argc = parse_args(line, argv, 16);
        if (argc == 0) continue;

        if      (kstrcmp(argv[0], "help")   == 0) cmd_help();
        else if (kstrcmp(argv[0], "clear")  == 0) cmd_clear();
        else if (kstrcmp(argv[0], "echo")   == 0) cmd_echo(argc, argv);
        else if (kstrcmp(argv[0], "ls")     == 0) cmd_ls(argc, argv);
        else if (kstrcmp(argv[0], "cat")    == 0) cmd_cat(argc, argv);
        else if (kstrcmp(argv[0], "run")    == 0) cmd_run(argc, argv);
        else if (kstrcmp(argv[0], "edit")   == 0) cmd_edit(argc, argv);
        else if (kstrcmp(argv[0], "cd")     == 0) cmd_cd(argc, argv);
        else if (kstrcmp(argv[0], "mkdir")  == 0) cmd_mkdir(argc, argv);
        else if (kstrcmp(argv[0], "about")  == 0) cmd_about();
        else if (kstrcmp(argv[0], "ps")     == 0) cmd_ps();
        else if (kstrcmp(argv[0], "reboot") == 0) cmd_reboot();
        else {
            term_set_color(0x0C);
            term_puts("Unknown command: ");
            term_puts(argv[0]);
            term_putc('\n');
            term_set_color(0x0F);
            term_puts("Type 'help' for available commands.\n");
        }
    }
}

// Suppress unused warning for term_puthex (used via term_puthex in future).
static void shell_suppress_unused(void) __attribute__((unused));
static void shell_suppress_unused(void) { (void)term_puthex; }
