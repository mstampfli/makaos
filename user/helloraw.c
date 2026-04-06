// Minimal ring-3 hello — zero libc, raw inline syscall only.
typedef unsigned long long uint64_t;

static inline void raw_write(const char* s, uint64_t len) {
    __asm__ volatile(
        "syscall"
        :
        : "a"((uint64_t)0),   // SYS_WRITE = 0
          "D"((uint64_t)1),   // fd = 1 (stdout)
          "S"(s),             // buf
          "d"(len)            // len
        : "rcx", "r11", "memory"
    );
}

static inline void raw_exit(void) {
    __asm__ volatile(
        "syscall"
        :
        : "a"((uint64_t)1),   // SYS_EXIT = 1
          "D"((uint64_t)0)
        : "rcx", "r11"
    );
}

void _start(void) {
    raw_write("hello from ELF!\n", 16);
    raw_exit();
}
