// Ring-3 virtual memory allocation test.
// Tests: sys_brk (demand paging), read/write to demand-paged pages,
//        sys_write (VGA stdout), sys_exit.
// No libc — all syscalls done inline.

typedef unsigned long  uint64_t;
typedef unsigned int   uint32_t;
typedef unsigned char  uint8_t;
typedef long           int64_t;

// ── Syscall numbers (must match kernel/syscall.h) ─────────────────────────
#define SYS_WRITE  0
#define SYS_EXIT   1
#define SYS_BRK    5

// ── Inline syscall stubs ──────────────────────────────────────────────────
static inline int64_t syscall1(uint64_t nr, uint64_t a1) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void sys_exit(int code) {
    syscall1(SYS_EXIT, (uint64_t)code);
    __asm__ volatile("hlt"); // unreachable
}

static void write_str(const char* s) {
    uint64_t len = 0;
    while (s[len]) len++;
    syscall3(SYS_WRITE, 1, (uint64_t)s, len);
}

static uint64_t brk_current(void) {
    return (uint64_t)syscall1(SYS_BRK, 0);
}

static uint64_t brk_set(uint64_t new_brk) {
    return (uint64_t)syscall1(SYS_BRK, new_brk);
}

// ── Minimal number printer (no printf) ───────────────────────────────────
static void write_u64(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (v == 0) { write_str("0"); return; }
    while (v && i > 0) { buf[--i] = '0' + (char)(v % 10); v /= 10; }
    syscall3(SYS_WRITE, 1, (uint64_t)&buf[i], (uint64_t)(20 - i));
}

// ── Test helpers ──────────────────────────────────────────────────────────
static int s_pass = 0;
static int s_fail = 0;

static void check(const char* name, int ok) {
    write_str(ok ? "[PASS] " : "[FAIL] ");
    write_str(name);
    write_str("\n");
    if (ok) s_pass++; else s_fail++;
}

// ── _start ────────────────────────────────────────────────────────────────
int main(void) {
    write_str("=== vmalloc ring-3 test ===\n");

    // ── Test 1: query current brk ─────────────────────────────────────────
    uint64_t brk0 = brk_current();
    check("brk(0) returns non-zero", brk0 != 0 && brk0 != (uint64_t)-1LL);

    // ── Test 2: grow brk by one page ─────────────────────────────────────
    uint64_t page = 4096;
    uint64_t new_brk = (brk0 + page - 1) & ~(page - 1); // align up first
    new_brk += page;
    uint64_t result = brk_set(new_brk);
    check("brk grow by 1 page", result == new_brk);

    // ── Test 3: write to demand-paged page ────────────────────────────────
    // The page was not physically mapped yet — first write triggers #PF.
    uint8_t* heap = (uint8_t*)brk0;
    // align heap pointer up to page boundary
    uint64_t heap_addr = (brk0 + page - 1) & ~(page - 1);
    uint8_t* p = (uint8_t*)heap_addr;
    p[0] = 0xAB;
    p[1] = 0xCD;
    p[4095] = 0xEF;
    check("write to demand page (no crash)", 1); // reaching here = no segfault

    // ── Test 4: read back values ──────────────────────────────────────────
    check("read back byte 0 (0xAB)", p[0] == 0xAB);
    check("read back byte 1 (0xCD)", p[1] == 0xCD);
    check("read back last byte (0xEF)", p[4095] == 0xEF);

    // ── Test 5: zero-init of demand pages ────────────────────────────────
    // Bytes we didn't write should be zero (kernel zeros on demand).
    check("demand page zero-init", p[2] == 0 && p[100] == 0 && p[4094] == 0);

    // ── Test 6: grow by multiple pages ───────────────────────────────────
    uint64_t brk1 = result;
    uint64_t brk2 = brk1 + 8 * page;
    result = brk_set(brk2);
    check("brk grow by 8 pages", result == brk2);

    // Write to the last page of the new region.
    uint8_t* q = (uint8_t*)(brk2 - page);
    q[0] = 0x42;
    check("write to 8th demand page", q[0] == 0x42);

    // ── Test 7: shrink brk ────────────────────────────────────────────────
    result = brk_set(brk1);
    check("brk shrink back", result == brk1);

    // ── Summary ───────────────────────────────────────────────────────────
    write_str("--- ");
    write_u64((uint64_t)s_pass);
    write_str(" passed, ");
    write_u64((uint64_t)s_fail);
    write_str(" failed ---\n");

    sys_exit(s_fail == 0 ? 0 : 1);
}
