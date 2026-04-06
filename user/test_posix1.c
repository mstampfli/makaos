#include "libc.h"

// ── Tiny test framework ───────────────────────────────────────────────────

static int s_run = 0, s_pass = 0;

static void check(const char* name, int ok) {
    s_run++;
    if (ok) { s_pass++; printf("  [PASS] %s\n", name); }
    else              printf("  [FAIL] %s\n", name);
}

// ── Tests ─────────────────────────────────────────────────────────────────

static void test_malloc_zero(void) {
    printf("-- malloc(0)\n");
    void* p = malloc(0);
    check("malloc(0) != NULL",  p != NULL);
    free(p); // must not crash
    check("free(malloc(0)) ok", 1);
}

static void test_memmove(void) {
    printf("-- memmove\n");

    // Forward overlap: src < dst, ranges overlap
    char a[16];
    memcpy(a, "0123456789ABCDEF", 16);
    memmove(a + 4, a, 8);          // copy "01234567" → positions 4-11
    check("memmove fwd [4]='0'", a[4] == '0');
    check("memmove fwd [5]='1'", a[5] == '1');
    check("memmove fwd [11]='7'", a[11] == '7');

    // Backward overlap: dst < src, ranges overlap
    char b[16];
    memcpy(b, "0123456789ABCDEF", 16);
    memmove(b, b + 2, 8);          // copy "23456789" → positions 0-7
    check("memmove bwd [0]='2'", b[0] == '2');
    check("memmove bwd [7]='9'", b[7] == '9');

    // Non-overlapping (sanity)
    char c[16] = {0};
    memmove(c, "hello", 5);
    check("memmove non-overlap", c[0]=='h' && c[4]=='o' && c[5]=='\0');
}

static void test_strdup(void) {
    printf("-- strdup / strndup\n");

    char* s = strdup("hello");
    check("strdup != NULL",          s != NULL);
    check("strdup content",          s && strcmp(s, "hello") == 0);
    check("strdup independent copy", s && (s[0] = 'H', s[0] == 'H'));
    free(s);

    char* sn = strndup("hello world", 5);
    check("strndup != NULL",  sn != NULL);
    check("strndup length=5", sn && strlen(sn) == 5);
    check("strndup content",  sn && strcmp(sn, "hello") == 0);
    free(sn);

    // strndup with max >= strlen — should copy all
    char* sn2 = strndup("hi", 100);
    check("strndup max>len", sn2 && strcmp(sn2, "hi") == 0);
    free(sn2);
}

static void test_printf_octal(void) {
    printf("-- printf %%o\n");

    char buf[32];
    snprintf(buf, sizeof(buf), "%o", 0u);
    check("printf %o 0",   strcmp(buf, "0")   == 0);

    snprintf(buf, sizeof(buf), "%o", 8u);
    check("printf %o 8",   strcmp(buf, "10")  == 0);

    snprintf(buf, sizeof(buf), "%o", 255u);
    check("printf %o 255", strcmp(buf, "377") == 0);

    snprintf(buf, sizeof(buf), "%o", 511u);
    check("printf %o 511", strcmp(buf, "777") == 0);

    snprintf(buf, sizeof(buf), "%04o", 8u);
    check("printf %04o 8", strcmp(buf, "0010") == 0);
}

static void test_write_readonly_fd(void) {
    printf("-- write() to read-only fd\n");
    // fd 0 is keyboard (read-only — no write handler).
    ssize_t r = write(0, "x", 1);
    check("write(stdin) == -1", r == (ssize_t)-1);
}

static void test_brk_exact(void) {
    printf("-- brk() exact value\n");
    uint64_t cur = brk(0);
    check("brk(0) returns current brk", cur != (uint64_t)-1);

    // Ask for cur+1: should get exactly cur+1, not rounded to page.
    uint64_t got = brk(cur + 1);
    check("brk(cur+1) == cur+1 (no round-up)", got == cur + 1);

    // Ask for a page-unaligned value well into next page.
    uint64_t want = cur + 2049;
    got = brk(want);
    check("brk(cur+2049) == cur+2049", got == want);

    // Restore (shrink back).
    brk(cur);
}

static void test_close_low_fd(void) {
    printf("-- close(fd < 3)\n");
    // Run inside a forked child so we don't lose our own stdin/stdout.
    int child = fork();
    if (child == 0) {
        // In child: close fd 0 (stdin/keyboard).
        int r0 = close(0);
        // close fd 2 (stderr/vga).
        int r2 = close(2);
        // Exit: 0 if both returned 0, else 1.
        exit((r0 == 0 && r2 == 0) ? 0 : 1);
    }
    int status = 0;
    wait(child, &status);
    check("close(0) returns 0", WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_exit_wait_codes(void) {
    printf("-- exit() / wait() exit codes\n");

    // Helper: fork a child that exits with `code`, parent checks it.
    struct { int code; const char* label; } cases[] = {
        { 0,   "exit(0)"   },
        { 1,   "exit(1)"   },
        { 42,  "exit(42)"  },
        { 127, "exit(127)" },
        { 255, "exit(255)" },
    };

    for (int i = 0; i < 5; i++) {
        int expected = cases[i].code;
        int child = fork();
        if (child == 0) exit(expected);

        int status = 0;
        int wpid = wait(child, &status);

        char label[64];
        // Build label: "wait pid ok - exit(N)"
        snprintf(label, sizeof(label), "wait pid ok - %s", cases[i].label);
        check(label, wpid == child);

        snprintf(label, sizeof(label), "WIFEXITED    - %s", cases[i].label);
        check(label, WIFEXITED(status));

        snprintf(label, sizeof(label), "WEXITSTATUS  - %s", cases[i].label);
        check(label, WEXITSTATUS(status) == expected);
    }
}

static void test_wait_status_ptr_null(void) {
    printf("-- wait() with NULL status\n");
    int child = fork();
    if (child == 0) exit(7);
    int wpid = wait(child, NULL);  // should not crash
    check("wait(pid, NULL) returns pid", wpid == child);
}

// ── Entry ─────────────────────────────────────────────────────────────────

void _start(void) {
    printf("========= POSIX-1 test suite =========\n");

    test_malloc_zero();
    test_memmove();
    test_strdup();
    test_printf_octal();
    test_write_readonly_fd();
    test_brk_exact();
    test_close_low_fd();
    test_exit_wait_codes();
    test_wait_status_ptr_null();

    printf("======================================\n");
    printf("Result: %d / %d passed\n", s_pass, s_run);
    if (s_pass == s_run)
        printf("ALL TESTS PASSED\n");
    else
        printf("%d FAILED\n", s_run - s_pass);

    exit(s_pass == s_run ? 0 : 1);
}
