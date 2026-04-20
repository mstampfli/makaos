// Phase 8D: io_uring acceptance + throughput test.
//
// Acceptance:
//   - Create ring with 64 SQEs, verify mapped pointers are non-NULL.
//   - Submit IORING_OP_NOP in batches; drain CQEs; verify user_data
//     round-trips.
//   - Submit IORING_OP_READ against /etc/passwd in a batch; verify
//     bytes read match what sys_read would return, and contents
//     match a direct-syscall read.
//
// Benchmark:
//   - Measure ns per io_uring op (nop and read) vs ns per direct
//     syscall (sys_read).  Print speedup.
//
// Exit 0 on success, 1 on failure.

#include "libc.h"

static uint64_t tsc_ns(void) {
    struct { int64_t tv_sec; int64_t tv_usec; } tv;
    gettimeofday((struct timeval*)&tv, 0);
    return (uint64_t)tv.tv_sec * 1000000000ULL +
           (uint64_t)tv.tv_usec * 1000ULL;
}

static int s_pass = 0, s_fail = 0;
static void check(const char* name, int ok) {
    if (ok) { s_pass++; printf("  [PASS] %s\n", name); }
    else    { s_fail++; printf("  [FAIL] %s\n", name); }
}

int main(void) {
    printf("=== io_uring acceptance + benchmark ===\n");

    // ── 1. Setup ───────────────────────────────────────────────
    io_uring_params_t p = {0};
    int ring_fd = io_uring_setup(64, &p);
    check("io_uring_setup > 0", ring_fd > 0);
    if (ring_fd <= 0) return 1;

    io_sq_ring_hdr_t* sq = (io_sq_ring_hdr_t*)(uintptr_t)p.sq_ring_ptr;
    io_cq_ring_hdr_t* cq = (io_cq_ring_hdr_t*)(uintptr_t)p.cq_ring_ptr;
    io_sqe_t*         sqes = (io_sqe_t*)(uintptr_t)p.sqes_ptr;
    io_cqe_t*         cqes = (io_cqe_t*)(uintptr_t)p.cqes_ptr;

    check("sq_ring_ptr != NULL", sq != 0);
    check("cq_ring_ptr != NULL", cq != 0);
    check("sqes_ptr != NULL",    sqes != 0);
    check("cqes_ptr != NULL",    cqes != 0);
    check("sq_entries == 64",    p.sq_entries == 64);
    check("cq_entries == 128",   p.cq_entries == 128);

    // ── 2. NOP batch (32 ops) ──────────────────────────────────
    const uint32_t N_NOP = 32;
    uint32_t sq_mask = sq->ring_mask;
    for (uint32_t i = 0; i < N_NOP; i++) {
        io_sqe_t* s = &sqes[(sq->tail + i) & sq_mask];
        s->opcode    = IORING_OP_NOP;
        s->flags     = 0;
        s->fd        = -1;
        s->user_data = 0xBEEF0000u + i;
    }
    __atomic_store_n(&sq->tail, sq->tail + N_NOP, __ATOMIC_RELEASE);

    int submitted = io_uring_enter(ring_fd, N_NOP, N_NOP,
                                     IORING_ENTER_GETEVENTS);
    check("NOP batch submitted == 32", submitted == (int)N_NOP);

    // Drain CQEs and verify user_data round-trips.
    uint32_t cq_mask = cq->ring_mask;
    uint32_t nop_ok = 0;
    uint32_t ch = cq->head;
    uint32_t ct = __atomic_load_n(&cq->tail, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < N_NOP && ch < ct; i++, ch++) {
        io_cqe_t* c = &cqes[ch & cq_mask];
        if (c->res == 0 && c->user_data == (0xBEEF0000u + i)) nop_ok++;
    }
    __atomic_store_n(&cq->head, ch, __ATOMIC_RELEASE);
    check("NOP batch: 32 CQEs with correct user_data", nop_ok == N_NOP);

    // ── 3. READ batch against /etc/passwd ───────────────────────
    int pwd_fd = open("/etc/passwd", 0, 0);  // O_RDONLY
    check("open(/etc/passwd)", pwd_fd >= 0);
    if (pwd_fd < 0) return 1;

    static char sync_buf[256];
    int sync_n = read(pwd_fd, sync_buf, sizeof(sync_buf));
    check("direct read > 0", sync_n > 0);
    lseek(pwd_fd, 0, SEEK_SET);

    static char uring_buf[256];
    io_sqe_t* s = &sqes[sq->tail & sq_mask];
    s->opcode    = IORING_OP_READ;
    s->flags     = 0;
    s->fd        = pwd_fd;
    s->off       = 0;           // pread at offset 0
    s->addr      = (uint64_t)(uintptr_t)uring_buf;
    s->len       = sizeof(uring_buf);
    s->user_data = 0xDEADBEEFu;
    __atomic_store_n(&sq->tail, sq->tail + 1, __ATOMIC_RELEASE);

    submitted = io_uring_enter(ring_fd, 1, 1, IORING_ENTER_GETEVENTS);
    check("READ submit == 1", submitted == 1);

    ch = cq->head;
    io_cqe_t* rc = &cqes[ch & cq_mask];
    check("READ cqe.user_data matches",  rc->user_data == 0xDEADBEEFu);
    check("READ cqe.res matches sync",   rc->res == sync_n);
    int match = 1;
    for (int i = 0; i < sync_n; i++)
        if (sync_buf[i] != uring_buf[i]) { match = 0; break; }
    check("READ content matches sync",   match);
    __atomic_store_n(&cq->head, ch + 1, __ATOMIC_RELEASE);

    // ── 4. Benchmark: 1000 direct reads vs 1 batched enter ─────
    const uint32_t BENCH_N = 1000;

    uint64_t t0 = tsc_ns();
    for (uint32_t i = 0; i < BENCH_N; i++) {
        lseek(pwd_fd, 0, SEEK_SET);
        (void)read(pwd_fd, sync_buf, sizeof(sync_buf));
    }
    uint64_t t1 = tsc_ns();
    uint64_t direct_ns = t1 - t0;

    // Fill 1000 SQEs in one batch.  Need ring big enough; 64-entry
    // ring requires N=64 chunks.
    uint64_t uring_ns = 0;
    {
        uint32_t done = 0;
        uint64_t a = tsc_ns();
        while (done < BENCH_N) {
            uint32_t chunk = 32;  // conservatively well under 64
            if (chunk > BENCH_N - done) chunk = BENCH_N - done;
            uint32_t t_start = sq->tail;
            for (uint32_t i = 0; i < chunk; i++) {
                io_sqe_t* s2 = &sqes[(t_start + i) & sq_mask];
                s2->opcode    = IORING_OP_READ;
                s2->flags     = 0;
                s2->fd        = pwd_fd;
                s2->off       = 0;
                s2->addr      = (uint64_t)(uintptr_t)uring_buf;
                s2->len       = sizeof(uring_buf);
                s2->user_data = done + i;
            }
            __atomic_store_n(&sq->tail, t_start + chunk, __ATOMIC_RELEASE);
            (void)io_uring_enter(ring_fd, chunk, chunk,
                                  IORING_ENTER_GETEVENTS);
            // Drain
            uint32_t ch2 = cq->head;
            uint32_t ct2 = __atomic_load_n(&cq->tail, __ATOMIC_ACQUIRE);
            while (ch2 != ct2) ch2++;
            __atomic_store_n(&cq->head, ch2, __ATOMIC_RELEASE);
            done += chunk;
        }
        uring_ns = tsc_ns() - a;
    }

    printf("direct  %u reads: %lu ns (%lu ns/op)\n",
           (unsigned)BENCH_N,
           (uint64_t)direct_ns,
           (uint64_t)(direct_ns / BENCH_N));
    printf("uring   %u reads: %lu ns (%lu ns/op)\n",
           (unsigned)BENCH_N,
           (uint64_t)uring_ns,
           (uint64_t)(uring_ns / BENCH_N));
    if (uring_ns > 0) {
        uint64_t speedup_10x = (direct_ns * 10) / uring_ns;
        printf("speedup: %lu.%lux\n",
               (uint64_t)(speedup_10x / 10),
               (uint64_t)(speedup_10x % 10));
    }

    close(pwd_fd);
    close(ring_fd);

    printf("--- %d passed, %d failed ---\n", s_pass, s_fail);
    return s_fail == 0 ? 0 : 1;
}
