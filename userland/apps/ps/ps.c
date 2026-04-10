// ps — list running processes via /proc
//
// Reads /proc to enumerate PIDs, then reads /proc/<pid>/status for each.
// Output format:
//   PID   PPID  STATE     NAME

#include "libc.h"
#include "stdio.h"

// ── String helpers ────────────────────────────────────────────────────────

static int str_len(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int str_is_pid(const char* s) {
    if (!s[0]) return 0;
    for (int i = 0; s[i]; i++) if (!is_digit(s[i])) return 0;
    return 1;
}

// ── Parse a field from /proc/<pid>/status ────────────────────────────────
// Scans buf for a line starting with `key:`, returns pointer to value.
// Returns NULL if not found.

static const char* parse_field(const char* buf, int buflen, const char* key) {
    int klen = str_len(key);
    for (int i = 0; i < buflen; ) {
        // Check if this line starts with key + ':'
        int match = 1;
        for (int k = 0; k < klen && match; k++)
            if (buf[i + k] != key[k]) match = 0;
        if (match && buf[i + klen] == ':') {
            // Skip past ':' and whitespace.
            int j = i + klen + 1;
            while (j < buflen && (buf[j] == ' ' || buf[j] == '\t')) j++;
            return buf + j;
        }
        // Advance to next line.
        while (i < buflen && buf[i] != '\n') i++;
        i++; // skip '\n'
    }
    return NULL;
}

// Copy a field value (up to newline) into dst[max].
static void copy_field(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i] && src[i] != '\n') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// ── Right-pad a string to width, writing into out ────────────────────────
static void write_padded(const char* s, int width) {
    int len = str_len(s);
    write(1, s, len);
    for (int i = len; i < width; i++) write(1, " ", 1);
}

// ── Main ─────────────────────────────────────────────────────────────────

int main(void) {
    // Print header.
    write(1, "PID   PPID  STATE     NAME\n", 27);
    write(1, "----  ----  --------  ----\n", 27);

    // Read /proc directory — one entry per PID.
    dirent_t entries[256];
    int count = readdir("/proc", 5, entries, 256);
    if (count < 0) {
        write(1, "ps: cannot read /proc\n", 22);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        // Skip non-PID entries (e.g. "self").
        if (!str_is_pid(entries[i].name)) continue;

        // Build /proc/<pid>/status path.
        char status_path[64];
        str_copy(status_path, "/proc/", 64);
        int plen = str_len(status_path);
        str_copy(status_path + plen, entries[i].name, 64 - plen);
        plen = str_len(status_path);
        str_copy(status_path + plen, "/status", 64 - plen);

        int fd = open(status_path, 0);
        if (fd < 0) continue;

        char buf[512];
        int n = (int)read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';

        // Extract fields.
        char pid_s[16]   = { 0 };
        char ppid_s[16]  = { 0 };
        char state_s[16] = { 0 };
        char name_s[16]  = { 0 };

        const char* v;
        if ((v = parse_field(buf, n, "Pid")))   copy_field(pid_s,   v, 16);
        if ((v = parse_field(buf, n, "PPid")))  copy_field(ppid_s,  v, 16);
        if ((v = parse_field(buf, n, "State"))) copy_field(state_s, v, 16);
        if ((v = parse_field(buf, n, "Name")))  copy_field(name_s,  v, 16);

        write_padded(pid_s,   6);
        write_padded(ppid_s,  6);
        write_padded(state_s, 10);
        write(1, name_s, str_len(name_s));
        write(1, "\n", 1);
    }

    return 0;
}
