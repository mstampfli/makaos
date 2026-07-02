#include "unveil.h"
#include "kprintf.h"   // kprintf_atomic (locked whole-line output for selftest result lines)
#include "kheap.h"
#include "errno.h"
#include "kstr.h"    // str_len + str_lcpy (shared; were local s_strlen / s_strncpy)

// ── String helpers (no libc in kernel; str_len/str_lcpy from kstr.h) ──────

static int s_strncmp(const char* a, const char* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

// ── unveil_add ────────────────────────────────────────────────────────────

int unveil_add(unveil_table_t* t, const char* path, uint8_t perms) {
    if (!t || !path)    return -EINVAL;
    if (t->locked)      return -EPERM;
    if (path[0] != '/') return -EINVAL;

    uint32_t plen = str_len(path);
    if (plen == 0 || plen >= UNVEIL_PATH_MAX) return -ENAMETOOLONG;

    // Merge perms if path already exists — O(n) scan, acceptable since
    // unveil tables are small (tens of entries at most, set at startup).
    for (uint32_t i = 0; i < t->count; i++) {
        if (s_strncmp(t->entries[i].path, path, UNVEIL_PATH_MAX) == 0) {
            t->entries[i].perms |= perms;
            return 0;
        }
    }

    // Grow backing array if needed. Double capacity each time to amortise.
    if (t->count >= t->cap) {
        uint32_t new_cap = t->cap ? t->cap * 2 : 4;
        unveil_entry_t* nb = (unveil_entry_t*)kmalloc(new_cap * sizeof(unveil_entry_t));
        if (!nb) return -ENOMEM;
        for (uint32_t i = 0; i < t->count; i++) nb[i] = t->entries[i];
        kfree(t->entries);
        t->entries = nb;
        t->cap     = new_cap;
    }

    t->entries[t->count].perms = perms;
    str_lcpy(t->entries[t->count].path, path, UNVEIL_PATH_MAX);
    t->count++;
    return 0;
}

// ── unveil_check ──────────────────────────────────────────────────────────

int unveil_check(const unveil_table_t* t, const char* path, uint8_t need) {
    if (!t || !path) return 0;
    if (t->count == 0) return 1;   // empty table = full visibility

    uint32_t plen = str_len(path);

    for (uint32_t i = 0; i < t->count; i++) {
        uint32_t elen = str_len(t->entries[i].path);
        if (plen < elen) continue;
        if (s_strncmp(path, t->entries[i].path, elen) != 0) continue;
        // Boundary check: prevents "/home/bob" matching "/home/bobby".
        if (plen > elen && path[elen] != '/') continue;
        return (t->entries[i].perms & need) == need;
    }

    return 0;
}

#ifdef MAKAOS_BOOT_SELFTESTS
// Deterministic check of the unveil gate logic that every path syscall now
// relies on (F38 wired unveil_check into unlink/rename/mkdir/truncate/exec/
// stat/access via the shared unveil_ok helper).  Covers: empty table = full
// visibility, exact + inside-prefix match, the boundary guard (/home/bob must
// not match /home/bobby), per-permission-bit denial, and out-of-view denial.
void unveil_gate_selftest(void) {
    extern void kprintf(const char*, ...);
    int fails = 0;
    unveil_table_t t;
    unveil_init(&t);

    // Empty table -> everything visible regardless of the requested bits.
    if (!unveil_check(&t, "/etc/passwd", UNVEIL_READ)) {
        kprintf_atomic("[unveil] FAIL empty-table should allow\n"); fails++;
    }

    // Grant /home/bob with read+write+create (no exec).
    unveil_add(&t, "/home/bob", UNVEIL_READ | UNVEIL_WRITE | UNVEIL_CREATE);

    struct { const char* path; uint8_t need; int want; } c[] = {
        { "/home/bob",      UNVEIL_READ,   1 },  // exact match
        { "/home/bob/file", UNVEIL_READ,   1 },  // inside the view
        { "/home/bob/a/b",  UNVEIL_WRITE,  1 },  // deeper, granted bit
        { "/home/bob/x",    UNVEIL_CREATE, 1 },  // create granted
        { "/home/bob/x",    UNVEIL_EXEC,   0 },  // exec NOT granted -> deny
        { "/home/bobby",    UNVEIL_READ,   0 },  // boundary: not under /home/bob
        { "/etc/passwd",    UNVEIL_READ,   0 },  // outside the view
        { "/home",          UNVEIL_READ,   0 },  // parent dir is not itself covered
    };
    for (unsigned i = 0; i < sizeof(c)/sizeof(c[0]); i++) {
        int got = unveil_check(&t, c[i].path, c[i].need);
        if (got != c[i].want) {
            kprintf_atomic("[unveil] FAIL path=%s need=%u got=%d want=%d\n",
                    c[i].path, (unsigned)c[i].need, got, c[i].want);
            fails++;
        }
    }
    unveil_free(&t);
    kprintf_atomic(fails ? "[unveil] SELF-TEST FAILED\n"
                  : "[unveil] SELF-TEST PASSED (prefix + boundary + perm bits)\n");
}
#endif
