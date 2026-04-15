// Phase 9-6 SMP wait-queue reproducer.
//
// Exercises every kernel path the phase 9-6 audit touches, end to end,
// with deterministic pass/fail. Designed to be run on `-smp 4` with
// `pick_home_cpu()` returning round-robin: every fork/wait, pipe,
// epoll_wait, AF_UNIX send/recv, and parallel disk read crosses CPU
// boundaries and hammers the sleep/wake ordering invariants.
//
// Exit code 0 = all iterations passed.  Non-zero = the stage that
// failed (1..5).
//
// Usage (from bash):  smp_test           — default iteration counts
//                     smp_test quick     — small counts, for CI/boot
//                     smp_test soak      — 10× counts, for a long run
//
// The test is dependency-free: it only uses libc syscalls, no
// external coreutils (yes/seq/head/... don't exist in the image).

#include "libc.h"

// ── result plumbing ──────────────────────────────────────────────────────

static int g_fail_stage = 0;   // first non-zero stage that failed

static void fail(int stage, const char* msg) {
    if (!g_fail_stage) g_fail_stage = stage;
    printf("[smp_test] STAGE %d FAIL: %s (errno=%d)\n", stage, msg, errno);
}

// Sleep for approx `ms` milliseconds.  Short sleeps inside the child
// widen the race window that the parent is about to hit; we use them
// sparingly because the point of the test is to *not* rely on timing.
static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (int64_t)(ms % 1000) * 1000000 };
    nanosleep(&ts, 0);
}

// ── stage 1: waitpid / zombie reap across CPUs ───────────────────────────
//
// Forks N children that each exit immediately with a distinct code,
// then reaps them in the order they were spawned.  Under round-robin
// placement every child lands on an AP; the reaper (parent) lives on
// whichever CPU it was placed.  The race we're hunting:
//
//   parent: walks children list, sees child RUNNING, heads for sched_sleep
//   child : on AP sets state=ZOMBIE, reparents under parent's children
//           list head, signal_send(parent, SIGCHLD) → sched_wake(parent)
//
// Phase 9-5's wake_pending flag catches the scheduler-level half; the
// 9-6a audit is that the children-list walk itself observes the child's
// transition correctly (release store in signal_send's rq_lock drop
// pairs with the acquire load in parent's next walk).

static int stage_waitpid(int iters, int children_per_iter) {
    printf("[smp_test] stage 1: waitpid (%d iters × %d children)\n",
           iters, children_per_iter);
    for (int i = 0; i < iters; i++) {
        int pids[64];
        if (children_per_iter > 64) children_per_iter = 64;
        for (int c = 0; c < children_per_iter; c++) {
            int pid = fork();
            if (pid < 0) { fail(1, "fork"); return 1; }
            if (pid == 0) {
                // Tiny amount of work so the child doesn't always
                // beat the parent to the wait.
                volatile int x = 0;
                for (int k = 0; k < 200; k++) x += k;
                _exit((c & 0x7F) | 0x01);
            }
            pids[c] = pid;
        }
        for (int c = 0; c < children_per_iter; c++) {
            int st = 0;
            int r  = waitpid(pids[c], &st, 0);
            if (r != pids[c]) { fail(1, "waitpid(pid) != pid"); return 1; }
            if (!WIFEXITED(st)) { fail(1, "child !WIFEXITED"); return 1; }
            int expect = (c & 0x7F) | 0x01;
            if (WEXITSTATUS(st) != expect) {
                fail(1, "wrong exit status"); return 1;
            }
        }
        if ((i & 0x3F) == 0x3F)
            printf("[smp_test]   stage 1 iter %d/%d\n", i + 1, iters);
    }
    return 0;
}

// ── stage 2: pipe round-trip across CPUs ─────────────────────────────────
//
// Parent creates a pipe, forks, then ping-pongs a byte through it.
// Child reads → writes, parent writes → reads.  Both sides sleep on
// the opposite wait queue, so every iteration exercises rd_waitq and
// wr_waitq lost-wakeup windows symmetrically.

static int stage_pipe(int rounds) {
    printf("[smp_test] stage 2: pipe ping-pong (%d rounds)\n", rounds);

    int p2c[2], c2p[2];
    if (pipe(p2c) < 0) { fail(2, "pipe p2c"); return 2; }
    if (pipe(c2p) < 0) { fail(2, "pipe c2p"); return 2; }

    int child = fork();
    if (child < 0) { fail(2, "fork"); return 2; }
    if (child == 0) {
        close(p2c[1]);
        close(c2p[0]);
        for (int i = 0; i < rounds; i++) {
            unsigned char b = 0;
            ssize_t n = read(p2c[0], &b, 1);
            if (n != 1) _exit(11);
            b++;
            if (write(c2p[1], &b, 1) != 1) _exit(12);
        }
        close(p2c[0]);
        close(c2p[1]);
        _exit(0);
    }

    close(p2c[0]);
    close(c2p[1]);
    unsigned char b = 0;
    for (int i = 0; i < rounds; i++) {
        if (write(p2c[1], &b, 1) != 1) { fail(2, "parent write"); return 2; }
        unsigned char r = 0;
        ssize_t n = read(c2p[0], &r, 1);
        if (n != 1) { fail(2, "parent read"); return 2; }
        if (r != (unsigned char)(b + 1)) {
            fail(2, "pipe value mismatch"); return 2;
        }
        b = r;
    }
    close(p2c[1]);
    close(c2p[0]);

    int st = 0;
    waitpid(child, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fail(2, "pipe child bad exit"); return 2;
    }
    return 0;
}

// ── stage 3: epoll_wait wakeup ordering ──────────────────────────────────
//
// Parent creates a pipe, registers the read end with epoll, then forks
// a child that writes after a brief stall.  Parent calls epoll_wait with
// a generous timeout; correct behaviour is to be woken by the child's
// write every iteration.  The race is the one documented in the 9-6c
// plan: state->has_ready flag ordering against a concurrent writer on
// another CPU.

static int stage_epoll(int rounds) {
    printf("[smp_test] stage 3: epoll_wait wakeup (%d rounds)\n", rounds);

    int epfd = epoll_create1(0);
    if (epfd < 0) { fail(3, "epoll_create1"); return 3; }

    for (int i = 0; i < rounds; i++) {
        int fds[2];
        if (pipe(fds) < 0) { fail(3, "pipe"); close(epfd); return 3; }

        epoll_event_t ev = { 0 };
        ev.events   = EPOLLIN;
        ev.data.u64 = 0xA5A5A5A5u;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev) < 0) {
            fail(3, "epoll_ctl ADD"); close(fds[0]); close(fds[1]);
            close(epfd); return 3;
        }

        int child = fork();
        if (child < 0) {
            fail(3, "fork"); close(fds[0]); close(fds[1]);
            close(epfd); return 3;
        }
        if (child == 0) {
            close(fds[0]);
            // Every few rounds stall briefly so the parent definitely
            // reaches epoll_wait before the writer fires — that's the
            // window where a lost wakeup leaves the parent stuck.
            if ((i & 0x7) == 0) msleep(1);
            unsigned char byte = (unsigned char)i;
            if (write(fds[1], &byte, 1) != 1) _exit(21);
            close(fds[1]);
            _exit(0);
        }
        close(fds[1]);

        epoll_event_t out[4];
        // 5-second cap — if we hit this, it's a real hang, not a
        // legitimate wait.
        int n = epoll_wait(epfd, out, 4, 5000);
        if (n <= 0) {
            fail(3, "epoll_wait returned no events");
            close(fds[0]); close(epfd); return 3;
        }
        if (out[0].data.u64 != 0xA5A5A5A5u) {
            fail(3, "epoll event.data mismatch");
            close(fds[0]); close(epfd); return 3;
        }
        unsigned char got = 0;
        if (read(fds[0], &got, 1) != 1) {
            fail(3, "epoll pipe read"); close(fds[0]); close(epfd);
            return 3;
        }
        if (got != (unsigned char)i) {
            fail(3, "epoll byte mismatch"); close(fds[0]); close(epfd);
            return 3;
        }

        epoll_ctl(epfd, EPOLL_CTL_DEL, fds[0], 0);
        close(fds[0]);

        int st = 0;
        waitpid(child, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            fail(3, "epoll child bad exit"); close(epfd); return 3;
        }
    }
    close(epfd);
    return 0;
}

// ── stage 4: AF_UNIX SOCK_STREAM send/recv ───────────────────────────────
//
// Parent binds and listens on a filesystem path, forks a child that
// connects back and echoes `rounds` bytes.  Parent accepts and drives
// the same ping-pong.  Every accept+connect+send+recv path goes through
// unix_sock.c's wait queues.

static int stage_unix(int rounds) {
    printf("[smp_test] stage 4: AF_UNIX ping-pong (%d rounds)\n", rounds);

    const char* path = "/tmp/smp_test.sock";
    unlink(path);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { fail(4, "socket"); return 4; }

    struct sockaddr_un sa;
    sa.sun_family = AF_UNIX;
    int i = 0;
    while (path[i] && i < UNIX_PATH_MAX - 1) {
        sa.sun_path[i] = path[i]; i++;
    }
    sa.sun_path[i] = '\0';

    if (bind(srv, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        fail(4, "bind"); close(srv); return 4;
    }
    if (listen(srv, 4) < 0) {
        fail(4, "listen"); close(srv); unlink(path); return 4;
    }

    int child = fork();
    if (child < 0) { fail(4, "fork"); close(srv); unlink(path); return 4; }
    if (child == 0) {
        close(srv);
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (c < 0) _exit(31);
        if (connect(c, (struct sockaddr*)&sa, sizeof(sa)) < 0) _exit(32);
        for (int r = 0; r < rounds; r++) {
            unsigned char b = 0;
            if (read(c, &b, 1) != 1) _exit(33);
            b ^= 0xFF;
            if (write(c, &b, 1) != 1) _exit(34);
        }
        close(c);
        _exit(0);
    }

    int cli = accept(srv, 0, 0);
    if (cli < 0) {
        fail(4, "accept"); close(srv); unlink(path);
        waitpid(child, 0, 0); return 4;
    }

    for (int r = 0; r < rounds; r++) {
        unsigned char snd = (unsigned char)(r * 7 + 1);
        if (write(cli, &snd, 1) != 1) {
            fail(4, "parent write"); close(cli); close(srv);
            unlink(path); waitpid(child, 0, 0); return 4;
        }
        unsigned char rcv = 0;
        if (read(cli, &rcv, 1) != 1) {
            fail(4, "parent read"); close(cli); close(srv);
            unlink(path); waitpid(child, 0, 0); return 4;
        }
        if (rcv != (unsigned char)(snd ^ 0xFF)) {
            fail(4, "unix echo mismatch"); close(cli); close(srv);
            unlink(path); waitpid(child, 0, 0); return 4;
        }
    }
    close(cli);
    close(srv);
    unlink(path);

    int st = 0;
    waitpid(child, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fail(4, "unix child bad exit"); return 4;
    }
    return 0;
}

// ── stage 5: parallel AHCI reads ─────────────────────────────────────────
//
// Forks N children that each read a file cover-to-cover from the ext2
// image.  Every read lands in ahci_submit → ahci_io_thread rendezvous;
// with round-robin placement and N > 1 both the submitters and the
// io_thread waiter are on arbitrary CPUs.  Phase 9-6f's audit target.

static int stage_ahci(int workers, int bytes_each) {
    printf("[smp_test] stage 5: parallel ext2/AHCI read (%d workers)\n",
           workers);
    const char* files[] = {
        "/bin/ls", "/bin/cat", "/bin/echo", "/bin/bash",
        "/bin/ps", "/bin/mkdir", "/bin/rm", "/bin/mv"
    };
    const int nfiles = (int)(sizeof(files) / sizeof(files[0]));

    int pids[8];
    if (workers > 8) workers = 8;

    for (int w = 0; w < workers; w++) {
        int pid = fork();
        if (pid < 0) { fail(5, "fork"); return 5; }
        if (pid == 0) {
            const char* path = files[w % nfiles];
            int fd = open(path, O_RDONLY);
            if (fd < 0) _exit(41);
            char buf[512];
            int total = 0;
            while (total < bytes_each) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n < 0) _exit(42);
                if (n == 0) break;
                total += (int)n;
            }
            close(fd);
            _exit(0);
        }
        pids[w] = pid;
    }
    for (int w = 0; w < workers; w++) {
        int st = 0;
        int r  = waitpid(pids[w], &st, 0);
        if (r != pids[w] || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            fail(5, "worker bad exit"); return 5;
        }
    }
    return 0;
}

// ── main ────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    int wait_iters = 200, wait_fanout = 4;
    int pipe_rounds = 2000;
    int epoll_rounds = 200;
    int unix_rounds = 2000;
    int ahci_workers = 4, ahci_bytes = 65536;

    if (argc >= 2) {
        const char* m = argv[1];
        if (!strcmp(m, "quick")) {
            wait_iters = 20; wait_fanout = 4;
            pipe_rounds = 200;
            epoll_rounds = 20;
            unix_rounds = 200;
            ahci_workers = 2; ahci_bytes = 4096;
        } else if (!strcmp(m, "soak")) {
            wait_iters = 2000; wait_fanout = 8;
            pipe_rounds = 20000;
            epoll_rounds = 2000;
            unix_rounds = 20000;
            ahci_workers = 8; ahci_bytes = 65536;
        }
    }

    printf("[smp_test] begin (pid=%d)\n", getpid());

    if (stage_waitpid(wait_iters, wait_fanout) != 0) goto done;
    if (stage_pipe(pipe_rounds)                  != 0) goto done;
    if (stage_epoll(epoll_rounds)                != 0) goto done;
    if (stage_unix(unix_rounds)                  != 0) goto done;
    if (stage_ahci(ahci_workers, ahci_bytes)     != 0) goto done;

done:
    if (g_fail_stage == 0) {
        printf("[smp_test] OK — all stages passed\n");
        return 0;
    }
    printf("[smp_test] FAIL at stage %d\n", g_fail_stage);
    return g_fail_stage;
}
