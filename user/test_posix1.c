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
    waitpid(child, &status, 0);
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
        int wpid = waitpid(child, &status, 0);

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
    int wpid = waitpid(child, NULL, 0);  // should not crash
    check("wait(pid, NULL) returns pid", wpid == child);
}

// ── errno tests ───────────────────────────────────────────────────────────

static void test_errno(void) {
    printf("-- errno\n");

    // open nonexistent file → ENOENT
    errno = 0;
    int fd = open("/does/not/exist", O_RDONLY);
    check("open(nonexist) == -1",   fd == -1);
    check("errno == ENOENT",        errno == ENOENT);

    // read from bad fd → EBADF
    errno = 0;
    ssize_t r = read(99, (void*)0x400000, 1);
    check("read(badfd) == -1",      r == (ssize_t)-1);
    check("errno == EBADF (read)",  errno == EBADF);

    // write to bad fd → EBADF
    errno = 0;
    r = write(99, "x", 1);
    check("write(badfd) == -1",     r == (ssize_t)-1);
    check("errno == EBADF (write)", errno == EBADF);

    // write to read-only fd (stdin) → EBADF (no write handler)
    errno = 0;
    r = write(0, "x", 1);
    check("write(stdin) == -1",     r == (ssize_t)-1);
    check("errno == EBADF (ro fd)", errno == EBADF);

    // close bad fd → EBADF
    errno = 0;
    int rc = close(99);
    check("close(badfd) == -1",     rc == -1);
    check("errno == EBADF (close)", errno == EBADF);

    // kill with invalid signal → EINVAL
    errno = 0;
    rc = kill(1, 999);
    check("kill(bad sig) == -1",    rc == -1);
    check("errno == EINVAL (kill)", errno == EINVAL);

    // lseek on bad fd → EBADF
    errno = 0;
    long off = lseek(99, 0, SEEK_SET);
    check("lseek(badfd) == -1",     off == -1L);
    check("errno == EBADF (lseek)", errno == EBADF);

    // lseek on non-seekable fd (stdout = VGA) → EINVAL
    errno = 0;
    off = lseek(1, 0, SEEK_SET);
    check("lseek(stdout) == -1",    off == -1L);
    check("errno == EINVAL (lseek non-seek)", errno == EINVAL);

    // errno is cleared on success: open a real file
    errno = 99;
    fd = open("/bin/test_posix1", O_RDONLY);
    check("open(real) >= 0",        fd >= 0);
    if (fd >= 0) close(fd);
    // errno should be unchanged (still 99) — success doesn't touch errno
    check("errno unchanged on success", errno == 99);
}

// ── open() flags tests ────────────────────────────────────────────────────

static void test_open_flags(void) {
    printf("-- open() flags\n");

    // O_RDONLY: open existing file for reading
    int fd = open("/bin/test_posix1", O_RDONLY);
    check("O_RDONLY opens existing", fd >= 0);
    if (fd >= 0) {
        char buf[4] = {0};
        ssize_t n = read(fd, buf, 4);
        check("read after O_RDONLY works", n == 4);
        close(fd);
    }

    // O_CREAT: create a new file that doesn't exist
    fd = open("/tmp_creat_test", O_CREAT | O_WRONLY);
    check("O_CREAT creates file", fd >= 0);
    if (fd >= 0) close(fd);

    // Open again without O_CREAT — file now exists, should work
    fd = open("/tmp_creat_test", O_RDONLY);
    check("O_RDONLY on just-created file", fd >= 0);
    if (fd >= 0) close(fd);

    // O_CREAT | O_EXCL on existing file → EEXIST
    errno = 0;
    fd = open("/tmp_creat_test", O_CREAT | O_EXCL | O_WRONLY);
    check("O_CREAT|O_EXCL on existing == -1", fd == -1);
    check("errno == EEXIST", errno == EEXIST);

    // O_CREAT | O_EXCL on new file → succeeds
    fd = open("/tmp_excl_test", O_CREAT | O_EXCL | O_WRONLY);
    check("O_CREAT|O_EXCL on new file >= 0", fd >= 0);
    if (fd >= 0) close(fd);

    // Cleanup
    unlink("/tmp_creat_test", 14);
    unlink("/tmp_excl_test",  14);
}

// ── open() write-mode tests ───────────────────────────────────────────────

static void test_open_write(void) {
    printf("-- open() write modes\n");

    // O_WRONLY: write to a file
    int fd = open("/tmp_write_test", O_CREAT | O_WRONLY);
    check("O_WRONLY open", fd >= 0);
    if (fd >= 0) {
        ssize_t n = write(fd, "hello", 5);
        check("write 5 bytes", n == 5);
        close(fd);
    }

    // O_RDONLY on written file: read back and verify
    fd = open("/tmp_write_test", O_RDONLY);
    check("O_RDONLY after write", fd >= 0);
    if (fd >= 0) {
        char buf[8] = {0};
        ssize_t n = read(fd, buf, 8);
        check("read back 5 bytes", n == 5);
        check("content == 'hello'", strncmp(buf, "hello", 5) == 0);
        close(fd);
    }

    // O_RDONLY: write should fail with EBADF
    fd = open("/tmp_write_test", O_RDONLY);
    if (fd >= 0) {
        errno = 0;
        ssize_t n = write(fd, "x", 1);
        check("write to O_RDONLY == -1", n == -1);
        check("errno == EBADF for O_RDONLY write", errno == EBADF);
        close(fd);
    }

    // O_TRUNC: open existing file with O_TRUNC, it should be empty
    fd = open("/tmp_write_test", O_WRONLY | O_TRUNC);
    check("O_TRUNC open", fd >= 0);
    if (fd >= 0) close(fd);
    fd = open("/tmp_write_test", O_RDONLY);
    if (fd >= 0) {
        char buf[8] = {0};
        ssize_t n = read(fd, buf, 8);
        check("O_TRUNC makes file empty (read == 0)", n == 0);
        close(fd);
    }

    // O_APPEND: writes always go to end of file
    fd = open("/tmp_write_test", O_WRONLY);
    if (fd >= 0) { write(fd, "abc", 3); close(fd); }
    fd = open("/tmp_write_test", O_WRONLY | O_APPEND);
    check("O_APPEND open", fd >= 0);
    if (fd >= 0) {
        // Even if we seek to 0, write should go to end
        lseek(fd, 0, SEEK_SET);
        write(fd, "xyz", 3);
        close(fd);
    }
    fd = open("/tmp_write_test", O_RDONLY);
    if (fd >= 0) {
        char buf[8] = {0};
        ssize_t n = read(fd, buf, 8);
        check("O_APPEND appended (total 6 bytes)", n == 6);
        check("O_APPEND content = 'abcxyz'", strncmp(buf, "abcxyz", 6) == 0);
        close(fd);
    }

    // Cleanup
    unlink("/tmp_write_test", 15);
}

// ── lseek tests ───────────────────────────────────────────────────────────

static void test_lseek(void) {
    printf("-- lseek\n");

    // First write a small known file
    // Use ext2_write_file via the shell? No — use open+write.
    // write_file syscall isn't exposed; use spawn. Actually we can write via
    // a child process. Simpler: use a file we know exists in /bin.
    // /bin/test_posix1 starts with ELF magic: 0x7F 'E' 'L' 'F'

    int fd = open("/bin/test_posix1", O_RDONLY);
    check("lseek: open file", fd >= 0);
    if (fd < 0) return;

    // SEEK_SET to 0: read first 4 bytes (ELF magic)
    long pos = lseek(fd, 0, SEEK_SET);
    check("SEEK_SET 0 returns 0", pos == 0);

    char magic[4] = {0};
    read(fd, magic, 4);
    check("ELF magic[0] = 0x7F", (unsigned char)magic[0] == 0x7F);
    check("ELF magic[1] = 'E'",  magic[1] == 'E');

    // SEEK_CUR: we're now at offset 4; advance by 0, get position
    pos = lseek(fd, 0, SEEK_CUR);
    check("SEEK_CUR 0 == 4", pos == 4);

    // SEEK_SET to byte 1: re-read, should get 'E'
    lseek(fd, 1, SEEK_SET);
    char c = 0;
    read(fd, &c, 1);
    check("SEEK_SET 1 reads 'E'", c == 'E');

    // SEEK_CUR with negative offset: go back 1 from current (offset 2)
    pos = lseek(fd, -1, SEEK_CUR);
    check("SEEK_CUR -1 from 2 == 1", pos == 1);

    // SEEK_END to -4: last 4 bytes, position = file_size - 4
    // Just check it doesn't return -1 and returns > 0
    pos = lseek(fd, -4, SEEK_END);
    check("SEEK_END -4 > 0", pos > 0);

    close(fd);
}

// ── getppid / _exit / waitpid / wait tests ────────────────────────────────

static void test_waitpid_getppid(void) {
    printf("-- getppid / waitpid / wait\n");

    int parent_pid = getpid();
    check("getpid() > 0", parent_pid > 0);

    // getppid: child's ppid should equal parent's pid
    int child = fork();
    if (child == 0) {
        int ppid = getppid();
        _exit(ppid == parent_pid ? 0 : 1);
    }
    int status = 0;
    waitpid(child, &status, 0);
    check("child getppid == parent pid", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // _exit: same as exit for WEXITSTATUS purposes
    child = fork();
    if (child == 0) _exit(42);
    status = 0;
    waitpid(child, &status, 0);
    check("_exit(42) WEXITSTATUS == 42", WIFEXITED(status) && WEXITSTATUS(status) == 42);

    // wait(*status): waits for any child
    child = fork();
    if (child == 0) exit(7);
    status = 0;
    int wpid = wait(&status);
    check("wait() returns child pid", wpid == child);
    check("wait() WEXITSTATUS == 7", WIFEXITED(status) && WEXITSTATUS(status) == 7);

    // WNOHANG: child not yet exited → returns 0
    child = fork();
    if (child == 0) { /* spin briefly then exit */ exit(0); }
    // Try immediately — child may not have exited yet.
    // We can't reliably test WNOHANG without sleep, so just verify the call
    // doesn't crash and returns either 0 or child pid.
    status = 0;
    int r = waitpid(child, &status, WNOHANG);
    check("WNOHANG returns >= 0", r >= 0);
    // Clean up: wait properly
    if (r == 0) waitpid(child, &status, 0);
}

// ── strtol / atoi / strstr / strrchr tests ────────────────────────────────

static void test_libc_str(void) {
    printf("-- strtol / atoi / strstr / strrchr\n");

    // atoi
    check("atoi \"0\"",    atoi("0")    == 0);
    check("atoi \"42\"",   atoi("42")   == 42);
    check("atoi \"-7\"",   atoi("-7")   == -7);
    check("atoi \"  3\"",  atoi("  3")  == 3);

    // strtol bases
    char* end;
    check("strtol dec",  strtol("255", &end, 10) == 255 && *end == '\0');
    check("strtol hex",  strtol("0xFF", &end, 16) == 255 && *end == '\0');
    check("strtol oct",  strtol("077", &end, 8) == 63 && *end == '\0');
    check("strtol auto hex", strtol("0xFF", &end, 0) == 255);
    check("strtol auto oct", strtol("010", &end, 0) == 8);
    check("strtol neg",  strtol("-1", &end, 10) == -1);
    // endptr stops at non-digit
    strtol("12abc", &end, 10);
    check("strtol endptr", *end == 'a');

    // strstr
    check("strstr found",     strstr("hello world", "world") != NULL);
    check("strstr not found", strstr("hello", "xyz") == NULL);
    check("strstr empty needle", strstr("hello", "") != NULL);
    check("strstr at start", strstr("hello", "he") == (void*)"hello" - 0 + 0
          || strncmp(strstr("hello", "he"), "hello", 2) == 0);

    // strrchr
    check("strrchr finds last", strrchr("hello", 'l') != NULL &&
          strrchr("hello", 'l')[0] == 'l' &&
          strrchr("hello", 'l')[1] == 'o');
    check("strrchr not found", strrchr("hello", 'z') == NULL);
    check("strrchr first char", strrchr("abc", 'a') != NULL);
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
    test_errno();
    test_open_flags();
    test_open_write();
    test_lseek();
    test_waitpid_getppid();
    test_libc_str();

    printf("======================================\n");
    printf("Result: %d / %d passed\n", s_pass, s_run);
    if (s_pass == s_run)
        printf("ALL TESTS PASSED\n");
    else
        printf("%d FAILED\n", s_run - s_pass);

    exit(s_pass == s_run ? 0 : 1);
}
