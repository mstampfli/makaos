# MakaOS Feature Inventory

This document lists what MakaOS can do and what runs on it. Every item was checked against the code and git history on 2026-11-08, at main commit `ebe8a14`.
Nobody has booted this exact commit yet, so "verified" always means historical evidence: an earlier commit or doc records the item running, and the code is still present.

Status legend (exactly one per item):
- **verified**: there is concrete evidence it ran. The evidence is cited: a commit hash, a test that asserts it, or a screenshot in `docs/img`.
- **implemented**: the code exists and is wired in (built by `build.sh`, registered, or installed), but no run evidence was found.
- **partial** / **stub**: some of it is missing, and the entry says what.
- **not built**: the code or port script exists, but a default `bash build.sh` does not build or install it. The entry says what enables it.

The "Missing" lists have no status; those features are simply absent.

**Warning: MakaOS is a hobby OS. It is not usable for real work, and it is insecure: it has no ASLR, plaintext passwords, no SMAP, no kernel W^X, and many fake-success syscalls. Do not put it on a network you care about or give it data you care about.**

Abbreviations: "selftest" means an in-kernel test that runs only in a `SELFTESTS=1` build. Commit 0683566 records six such boots, each reporting 89 `SELF-TEST PASSED` markers.

---

## 1. Boot and platform

### Boot (UEFI only)
- UEFI loader `boot/uefi/main.c`: verified (856bbaa booted to the login prompt at 2/4/8/16/32 GiB; ebe8a14 removed the BIOS path). It is a freestanding clang PE32+ built without gnu-efi, and it logs to COM1.
- GOP framebuffer handoff: implemented. It takes the firmware's current mode; there is no SetMode. bpp is assumed to be 32 and the pixel format is ignored.
- Kernel load: implemented. `\EFI\MAKA\kernel.bin` is a flat binary loaded into a 32 MiB window.
  - There is no size check.
  - Suspected bug: the 2 MiB align mask adds 0x1FFFF where 0x1FFFFF is intended.
- Memory map: implemented. The UEFI map is converted to e820 and capped at 128 entries; anything beyond that is silently truncated.
- Boot page tables: implemented. Identity map plus HHDM at 0xFFFF800000000000 (1 GiB pages), and the kernel at 0xFFFFFFFF80000000 (2 MiB pages).
- ACPI RSDP discovery from EFI config tables: verified (db00ecd).
- ExitBootServices with retry on a stale map key, then handoff via `boot_info_t` in RDI: implemented.
- Kernel init order (kmain): implemented. fb, IDT, PIC, ACPI, TSC, PMM/heap/VMM/pcache, LAPIC/IOAPIC, TSS/syscall/GS, CSPRNG, sched, timer, early initcalls, init kthread.

### CPU setup
- GDT plus a per-CPU TSS, with IST stacks for #DF and NMI: implemented (`kernel/arch/x86_64/tss.c`).
- IDT: implemented. Vectors 0-2, 5-14 and 16-21 are installed; #BP (3) and #OF (4) have no handler. User faults become signals and kernel faults panic.
- NX (EFER.NXE) on the BSP and APs: implemented.
- PAT entry 1 set to write-combining (used for the user framebuffer): implemented.
- SSE enabled, with eager FXSAVE/FXRSTOR of 512 B per task: implemented. The kernel itself is built `-mgeneral-regs-only`.
- SMEP when CPUID reports it: implemented. It is set in `tss_init` on the BSP and in `cpu_init_ap` (`kernel/proc/cpu.c`) on APs.
- syscall/sysret MSRs (STAR/LSTAR/SFMASK): implemented. Return to user goes through `iretq`, as a workaround for a KVM SS RPL bug.
- Per-CPU data via `IA32_GS_BASE` (`this_cpu()` reads `%gs:0`): implemented. There is no swapgs; user TLS uses FS_BASE.
- Missing: AVX/XSAVE state, SMAP, UMIP, PCID, FSGSBASE, KPTI, KASLR, and global kernel pages. docs/HANDOFF.md claims "XSAVE", which is stale.

### SMP
- AP bring-up via INIT-SIPI-SIPI and a real-to-long-mode trampoline at 0x8000: verified (5b44717; e2730c9 "APs are now fully online").
- MAX_CPUS = 64 (`kernel/include/common.h`): implemented. Default QEMU runs use `-smp 4`.
- 4-CPU SMP operation: verified (e79b1dc, sway on 4-CPU SMP; 42fc75a, io_uring selftest on 4-CPU QEMU).
- Missing: CPU hotplug.

### ACPI
- MADT parsing (LAPIC, first IOAPIC, ISA overrides, LAPIC64, x2APIC entries; XSDT preferred): verified (db00ecd). Table checksums other than the RSDP's are not checked.
- Reboot via the 8042 0xFE pulse (`SYS_REBOOT`, root only): implemented. This path does not use ACPI.
- Missing: FADT, HPET table, MCFG, DSDT/AML, ACPI power-off, sleep states.

### Timers and clocks
- TSC calibrated against PIT channel 2: implemented (039ab75). It is the only clock source; there is no invariant-TSC check and no cross-CPU sync.
- LAPIC timer in x2APIC mode, periodic 1000 Hz on every CPU: implemented.
- 8259 PIC remapped then disabled; PIT used only for calibration: implemented.
- CLOCK_REALTIME: partial. It equals CLOCK_MONOTONIC (TSC since boot) because there is no RTC.
- Missing: HPET, RTC/CMOS wall clock, TSC-deadline mode, tickless mode, hrtimers, a timer wheel.

### Interrupts
- x2APIC (required; the kernel halts without it): verified (9a1bbbe). There is no xAPIC fallback.
- IOAPIC (single): implemented. PS/2 keyboard IRQ1 and mouse IRQ12 route to the BSP.
- MSI-X / MSI, programmed per driver: implemented (99cdc15, a8db34e, 1d5d3cf). There is no generic MSI layer.
  - MSI-X: AHCI, NVMe (one vector per CPU), virtio-net, virtio-input, virtio-gpu.
  - MSI: HDA.
- Missing: interrupt remapping, VT-d.

## 2. Memory management

- Buddy physical allocator (`kernel/mm/pmm.c`): verified (f193585, selftest of order-10 uniqueness). It has double-alloc and double-free detectors.
- Per-CPU order-0 page cache (pcp): verified (0d841f1, "pcp_hits=12 ... PASSED"; 1298c11 fixed a drain race). It uses a lockless cmpxchg16b fast path, depth 64.
- Slab allocator (`slab_pcpu.c`): verified (0d841f1, 200k allocs over 4 CPUs; 3e35d60). SLUB style, per-CPU freelists, lockless remote free, SLAB_TYPESAFE_BY_RCU.
- Slab/pcp shrinker kthread, plus a synchronous shrink on OOM: implemented.
- kmalloc (`kheap.c`): implemented. 10 size classes from 8 B to 4 KiB; larger requests go to the buddy allocator.
- Page cache (`pcache.c`): implemented. 1024-bucket hash, CLOCK eviction, reclaim kthread, 7-page readahead. Read-only: no dirty tracking and no writeback.
- ext2 block cache: implemented. 64 sets x 4 ways, 1 MiB.
- VMAs: implemented. A sorted singly linked list (O(n) lookup) with RCU readers. Kinds: anon, shared (shmem), file, MMIO.
- Demand paging (anon/brk): verified (2769add; `test_vmalloc`, `test_posix1`).
- File-backed demand paging and lazy ELF loading: implemented. File mmap is verified by `test_posix1` test_mmap_file.
- COW fork with per-frame refcounts: verified (`heapalias` stress test; 7fda702, 04a86aa, 27f5c6d).
- `mmap`: partial. Only MAP_SHARED, MAP_PRIVATE, MAP_ANONYMOUS and MAP_FIXED are honored.
  - Other flags are silently ignored, so FIXED_NOREPLACE acts as a plain hint.
  - Writable MAP_SHARED on a regular file returns EINVAL.
- `munmap`, including partial unmap with VMA split and TLB shootdown: implemented.
- `brk`: verified (`test_posix1`, `test_vmalloc`).
- `mprotect`, `madvise`, `msync`: stub. There is no syscall; libc returns success and does nothing.
- `mincore`: stub. libc returns ENOSYS.
- POSIX shm (`shm_open`/`shm_unlink`, `kernel/mm/shmem.c`): implemented. Objects are refcounted, demand-paged, resizable with ftruncate, and limited to 1 GiB.
- `memfd_create`: partial. It is a libc shm_open plus an immediate unlink, and seals are ignored.
- SysV shm: stub. `shmget`/`shmat` return ENOSYS.
- DMA frame pinning: implemented. `vmm_get_user_pages` breaks COW and then pins; `shmem_create_dma` provides pinned scatter-gather backing for DRM.
- TLB shootdown via IPIs scoped by a per-mm cpumask: verified (28ea4f2; b183a70 deadlock fix). More than 32 pages becomes a CR3 reload.
- Fixed address-space layout: implemented. PIE base 0x400000, stack top 0x7FFFFFFFE000 (8 MiB), mmap base 0x7FF000000000 growing down.
- Missing: vmalloc (despite the test name), mremap, user huge pages, swap, a VMA tree.

## 3. Processes, threads, scheduling

- `fork` (full COW): verified (`test_posix1`, `smp_test`).
- `vfork`: stub. libc calls fork.
- Threads via native `SYS_THREAD`: verified (27afb67, e2c82c7). Flags are SHARE_MM and SHARE_FILES; there is no `clone`. Thread groups (tgid) share signal dispositions.
- `exec`: implemented. Closes CLOEXEC fds; checks unveil, permissions and the exec bit.
- Native `SYS_SPAWN` with CRED/UMASK/PLEDGE/UNVEIL attributes: implemented.
- `posix_spawn` in libc (fork+exec, file actions): implemented (58eb65e).
- TLS via `SYS_SET_FS` (FS_BASE), variant-II static TLS in libc: verified (cbe02f5, dltest TLS bit, 17/17 runs). There is no arch_prctl.
- `set_cleartid`, futex-woken on thread exit: implemented.
- Futex: partial. Only WAIT and WAKE are supported.
  - Other ops return ENOSYS: no REQUEUE, PI, bitset or robust lists.
  - Futexes are process-private only.
- `wait`: partial. Supports pid > 0 or -1 and WNOHANG only; there is no WUNTRACED or process-group wait. `waitid` is emulated in libc.
- Resource limits: partial. Hardcoded FD_MAX 4096, 4096 tasks per non-root user, PID_MAX 65535. libc getrlimit/setrlimit report "unlimited".
- SIGCHLD to the parent on exit: implemented.

### Scheduler
- MLFQ, 4 levels, quanta 8/16/32/64 ticks, boost every 1000 ticks, 1000 Hz tick: implemented (`kernel/proc/sched.c`).
- Per-CPU runqueues built on Chase-Lev work-stealing deques (512 slots per level): verified (4c9e43a; 3a41f27 "chaselev self-test PASSED").
- Load balancing: implemented. An idle CPU steals from up to 4 random victims, at most 16 tasks each; new tasks are placed round-robin; wakeups prefer the last CPU.
- SMP scheduling: verified (5e7a7a0; `smp_test`, 4874bd8).
- Kernel and user preemption from the timer, honoring a per-CPU preempt depth, plus a reschedule IPI: implemented.
- Scheduler stall watchdog (dumps state after about 8 s): not built (SELFTESTS=1 only; b6debe9).
- Missing: CPU affinity, nice/setpriority, real-time scheduling classes.

### Signals
- `sigaction`, `sigprocmask`, basic delivery: verified (`test_posix1`, `sigtest`).
- SA_SIGINFO with a glibc-layout ucontext, and catchable SEGV/BUS/FPE/ILL/TRAP with si_code: verified (fc475b2, 4b7a34f; SIGTEST=1 boot).
- `sigaltstack` / SA_ONSTACK: verified (43c410c, "All four sigtest paths pass").
- `pthread_kill` and thread-directed signals: verified (e2c82c7).
- Asynchronous delivery: partial. Handlers run only on the syscall-return path, so a CPU-bound loop does not see a signal until its next syscall.
- `sigsuspend`: stub. libc swaps the mask and then sleeps.
- Signals 32-63: stub. `kill` and `sigaction` accept them, but they are never delivered (NSIG=32).
- Missing: real-time signal queueing; SA_RESTART, SA_NODEFER, SA_RESETHAND, SA_NOCLDSTOP.

### Sessions and job control
- `setsid`, `setpgid`, `getpgid`, `getpgrp`, `getsid`, `tcgetpgrp`, `tcsetpgrp` with hardening: implemented (4475680, 7b666c9, 3d78f74).
- SIGTTIN/SIGTTOU generation on tty/pty, and ^Z sends SIGTSTP to the foreground group: implemented.
- Stop and continue: stub. There is no STOPPED task state, and the default action of SIGSTOP, SIGTSTP and SIGCONT is to terminate. Job control works only for programs that catch these signals.

### ELF loading and dynamic linking
- Static ET_EXEC, lazily demand-paged: implemented.
- Static PIE, with R_X86_64_RELATIVE applied by the kernel: verified (f620641, `pietest` "PIE-RELOC-OK"). The load bias is always 0x400000.
- Auxiliary vector (PHDR, PAGESZ, ENTRY, RANDOM, ...): partial. AT_UID/EUID/GID/EGID are hardcoded to 0, and there is no AT_SECURE.
- `dlopen`/`dlsym`/`dlclose`/`dlerror`: verified (ab25871, cbe02f5, 7ab6938, `dltest` sentinels; 184ca56 loaded the real 83 MB libSDL3.so with about 26k relocations).
  - The loader is in libc (`userland/libc/dlfcn.c`): the main program is a PIE that statically contains libc and exports its dynsym.
  - Binding is eager, and DT_NEEDED is resolved recursively.
  - Relocations handled: RELATIVE, GLOB_DAT, JUMP_SLOT, 64, DTPMOD64, DTPOFF64.
  - Symbol lookup uses SysV hash, with a linear scan for GNU_HASH-only objects (37b69cb).
- TLS in DSOs (general-dynamic, `__tls_get_addr`, up to 32 modules): verified (cbe02f5, 03f4371, `tlsdso`).
- `dladdr`, `RTLD_NEXT`, `dlvsym`, `dl_iterate_phdr`: partial / stub.
  - dladdr works only for dlopen'd objects.
  - RTLD_NEXT does not skip the caller.
  - dlvsym ignores versions.
  - dl_iterate_phdr visits nothing.
- Missing:
  - PT_INTERP and a separate ld.so, so dynamically linked executables cannot be exec'd.
  - IFUNC, COPY relocations, symbol versioning, DT_INIT/DT_FINI.
  - Initial-exec TLS (TPOFF64) in a .so.
  - Core dumps, ptrace.

## 4. Syscalls

- Table: 119 entries, `s_syscall_table[128]` in `kernel/syscall/syscall.c`. SYS_* numbers 0-118 have no gaps; slots 119-127 and out-of-range numbers return ENOSYS. Status: implemented.
- The numbering is MakaOS-native, not the Linux x86-64 ABI. Some path arguments are (ptr, len), and `readdir` is path-based (there is no getdents).
- Pledge is checked in the dispatcher before every handler; a violation sends SIGKILL.

| Group (count) | Syscalls |
|---|---|
| Process/threads (18) | exit fork exec wait getpid getppid spawn thread kill sched_yield nproc set_fs set_cleartid futex reboot times getrusage uname |
| Memory (6) | brk mmap munmap shm_open shm_unlink slabinfo |
| Files (25) | read write open close lseek dup dup2 fcntl stat fstat access unlink rename truncate ftruncate chmod fchmod chown fchown readlink symlink link umask ioctl pipe |
| Directories (5) | readdir getcwd chdir mkdir rmdir |
| IO multiplexing/event fds (12) | select poll epoll_create epoll_ctl epoll_wait io_uring_setup io_uring_enter io_uring_register eventfd timerfd_create timerfd_settime timerfd_gettime |
| Sockets (15) | socket bind listen accept connect sendto recvfrom setsockopt shutdown socketpair sendmsg recvmsg getpeerpid net_ifconfig net_mac |
| Signals (5) | sigaction sigprocmask sigreturn sigaltstack signalfd |
| Time (3) | clock_ns nanosleep gettod |
| IDs/creds/sessions (19) | getuid geteuid getgid getegid getgroups setgroups setuid setgid seteuid setegid setreuid setregid setpgid getpgid getpgrp setsid getsid tcgetpgrp tcsetpgrp |
| TTY (1) | openpty |
| Graphics (3) | fb_blit fb_info fb_map |
| MakaOS security (7) | pledge unveil unveil_lock restrict_fd sendfd recvfd register_policy_agent |

Fake-success and stub syscalls found in the kernel:
- `chown`, `fchown`, `fchmod`: stub. They return 0 and store nothing.
- `chmod`: stub. The mode is never written to ext2; the only side effect is a ksec notification.
- `readlink`: stub. Returns "/bin/bash" for every path.
- `symlink`, `link`: stub. Both always return EPERM.
- `getrusage`: stub. Returns all zeros.
- `times`: partial. Only utime (ticks since boot) is filled in.
- `setsockopt`: partial.
  - Only SO_BROADCAST is honored on AF_INET.
  - SO_REUSEADDR, SO_RCVBUF, SO_SNDBUF, SO_KEEPALIVE, SO_RCVTIMEO, SO_LINGER and others return 0 and do nothing.
  - On AF_UNIX every option returns 0.
  - TCP_NODELAY returns ENOPROTOOPT.
- `fcntl`: partial. Supports DUPFD, DUPFD_CLOEXEC, GETFD/SETFD and GETFL/SETFL. Lock commands return EINVAL, and libc fakes them as success.
- `ioctl` TCSBRK, TCXONC, TCFLSH, TIOCEXCL, TIOCNXCL: stub (no-op, returns 0).
- `open(O_CREAT)`, `mkdir`: partial. The mode argument is ignored; new files are 0644 and directories 0755.
- `fstat`: partial. No timestamps; st_nlink is synthesized.
- `futex`: partial. WAIT and WAKE only.

Fake-success in libc only: `flock`, `fsync`, `mprotect`, `msync`, `madvise` return 0. `getsockopt(SO_PEERCRED)` reports uid 0. `getsockname`/`getpeername` return ENOTSUP, and `inotify_*` return ENOSYS.

Missing from the table: getsockopt, getsockname, getpeername, pread/pwrite/readv/writev (libc emulates these), the openat family, getdents, fsync, pselect/ppoll, mprotect, mount, clone, ptrace, getrandom.

## 5. Filesystems and VFS

### VFS internals
- File objects with atomic refcounts and fdget/fdput pinning; dynamic fd table; FD_CLOEXEC: implemented.
- Dentry cache: verified (f645b0a "[dcache_test] SELF-TEST PASSED"; dcache_race and dcache_held selftests). RCU hash keyed by (parent ino, name), resizable, LRU shrink.
- Inode cache: implemented. 2-level radix tree with a per-leaf seqlock.
- Mount table: partial. There is no mount/umount syscall; `/proc` and `/dev` are hardcoded prefixes, and everything else goes to a single ext2 root.
- Permission checks from mode bits: verified (34e1c6b, enforced on a booted image as uid 1000). Extended ACLs are a hook only.

### ext2 (root filesystem, on AHCI)
- Read, including direct, single- and double-indirect blocks: verified (every boot reads /bin and /etc; 4343b65 and later sessions).
- Write, create, block allocation: verified (b1dd61a fixed O_CREAT in /tmp, then exercised by the sway/GLFW runs in b094ac6).
- `unlink`, `mkdir`, `rmdir` (empty only), `rename` (cross-directory with ".." repoint and rollback): verified (selftest ext2_perm_op, rename_under; c8f028f, a7fd088, 1c77834).
- Truncate / O_TRUNC: implemented.
- Large files: partial. i_size is 32-bit and there is no triple-indirect, so files max out at about 64 MiB on the default 1 KiB-block image.
- Missing: symlinks, hardlinks, timestamps, stored chmod/chown, fsync, xattrs.

### FAT32
- `kernel/fs/fat32.c`: not built. It is in `KERNEL_EXCLUDE`; remove it from that list to compile it (it would still need wiring).

### Virtual filesystems and device nodes
- `/dev` (static table in `virtfs.c`): implemented. Nodes:
  - `tty`, `tty0`
  - `kbd`, `kbdraw`, `vga`, `mouse`, `dsp`
  - `null`, `zero`, `urandom`, `random`
  - `input/event0..2`
  - `dri/card0`, `dri/renderD128`
  - `ptmx`, `pts/N`
- `/proc`: partial. `self` plus `/proc/<pid>/{status,cmdline,maps,fd/}`. There is no meminfo, cpuinfo, uptime or mounts, and `/proc/self/exe` reads as "/bin/bash".
- `/tmp`: implemented as a plain ext2 directory (mode 1777); it is not a tmpfs.
- Missing: `/sys`, `/run`, `/dev/shm` (shm_open uses an in-kernel name table), tmpfs, devpts as a real filesystem, NFS.

### Pipes, ptys, IPC fds
- Anonymous pipes: verified (pipe_refcount and pipe_race selftests; 0683566). 4 KiB ring, SIGPIPE/EPIPE, O_NONBLOCK.
- PTYs (`/dev/ptmx` to `/dev/pts/N`, `openpty` syscall, full line discipline on the slave): verified (pty selftests; foot running bash in a pty, docs/img/foot-on-dwl-2026-06-11.png).
- eventfd (semaphore, nonblock, pollable): verified (selftest, 0cdcf33).
- timerfd (MONOTONIC/REALTIME, absolute, periodic): verified (selftest, b77d8bd).
- signalfd (Linux siginfo layout, 32-signal mask): verified (selftest, 85e57fc).
- Missing: FIFOs and mknod, flock and fcntl locks, inotify, xattrs.

## 6. Networking

- NIC: virtio-net only (see section 7).
- skb-like buffers (headroom, tailroom, refcount): implemented (`kernel/net/skbuff.h`).
- Ethernet framing (IPv4, ARP): implemented.
- ARP (RCU cache, request/reply, gratuitous): verified (81fec8e, followed by working HTTP).
- IPv4: partial. TTL 64, DF set, one default gateway. Fragments are rejected and there is no routing table.
- ICMP echo reply: implemented. No ping client and no raw sockets.
- UDP (ephemeral bind, SO_BROADCAST): verified (DHCP af642e0; DNS 2031676).
- TCP: verified (81fec8e real-internet HTTP; 2031676 HTTPS; tcp_* selftests). Partial in depth:
  - Supported: the full 11-state machine, 4 s TIME_WAIT, listen backlog 8.
  - Retransmit uses a fixed 1 s RTO with no backoff and no RTT estimation.
  - MSS is fixed at 1460 and no TCP options are sent; there is no window scaling and no SACK.
  - No congestion control and no keepalive.
  - Out-of-order segments are dropped.
- DHCP client `/bin/net` (RFC 2131, renew, writes resolv.conf, started by svcmgr): verified (af642e0, five boots got 10.0.2.15).
- DNS resolver in libc (A + CNAME, UDP with TCP fallback): verified (2031676). It is IPv4 only, and the libc.h inline `getaddrinfo` used by in-tree apps is an ENOSYS stub.
- TLS via mbedTLS 3.6.2 (`http_get`, TLS 1.3): verified (2031676). There is no CA bundle, so `-k` is required and certificates are not validated.
- `select`, `poll`: implemented.
- `epoll` (create, ctl, wait): verified (used by the Wayland stack under sway, b094ac6; epoll_watch_refcount selftest). EPOLLET is accepted but behaves level-triggered.
- io_uring: verified (42fc75a, "[io_uring_test] SELF-TEST PASSED" on 4 CPUs).
  - Shared SQ/CQ rings, async workers, SQPOLL thread, registered files, linked SQEs, CQ overflow queue.
  - Opcodes: NOP, READ, WRITE, CLOSE, OPENAT, SEND, RECV, CONNECT, ACCEPT, DRM_COMMIT.
  - FSYNC is a no-op; TIMEOUT and POLL_ADD return EINVAL.
- `/bin/test_io_uring` userland test (NOP, READ): implemented (c4c92b2 ran it; no pass line recorded).

### Unix sockets
- SOCK_STREAM (8 KiB ring, backlog 16): verified (Wayland clients under sway; 95dc7ab, b094ac6).
- SOCK_DGRAM: implemented (unix_dgram_peer selftest).
- `socketpair` (STREAM, DGRAM): verified (selftest, 4d776c1).
- SCM_RIGHTS (up to 253 fds): verified (95dc7ab; libwayland under sway; scm_rights selftest). Native `sendfd`/`recvfd` syscalls also carry per-fd rights masks.
- Naming: implemented. An in-kernel string table; bind creates no filesystem node.
- Missing: SOCK_SEQPACKET, abstract namespace, SCM_CREDENTIALS, a real SO_PEERCRED.

Missing networking overall: IPv6, a loopback interface / 127.0.0.1, netlink, AF_PACKET, raw sockets, IP reassembly, getsockname/getpeername, any NIC other than virtio-net.

## 7. Drivers

### PCI
- Config space via port I/O 0xCF8/0xCFC; brute-force bus/dev/fn scan returning the first class match: partial. There is no device list, bridge walk, BAR assignment or ECAM.
- Capability walk and bus-master enable: implemented (99cdc15, a3c5c55).
- Modern virtio-PCI transport (shared header): implemented. There is no legacy virtio transport.

### Storage
- AHCI (`kernel/drivers/storage/ahci.c`): verified (the root disk in every QEMU config; every booted session such as 4343b65).
  - DMA READ/WRITE EXT, up to 248 PRDT entries.
  - MSI-X with MSI fallback, polling backup.
  - Zero-copy user reads with frame pinning.
  - Uses only the first SATA disk; no ATAPI and no FLUSH.
- AHCI NCQ: stub. The code exists but is forced off (one slot, serialized) because NCQ stalled in QEMU.
- NVMe (`nvme.c`): verified as a driver (0309423, smoke read plus 10k-read stress with 0 mismatches). Partial:
  - Admin queue plus one I/O queue pair per CPU, MSI-X.
  - Only 2-PRP transfers (8 KiB max).
  - The filesystem cannot use it, and no run script attaches an NVMe disk.
- ATA PIO/IDE (`ata_poll.c`, `ata_driver_irq.c`): not built (KERNEL_EXCLUDE).
- Block layer: missing. ext2 calls `ahci_*` directly; there are no request queues, no bio, no partition registry.

### Video, DRM, KMS
- virtio-gpu 2D (display info, 2D resources, scatter-gather backing, scanout, flush): verified (docs/img/foot-on-dwl-2026-06-11.png; 4343b65).
- virtio-gpu 3D / virgl (contexts, 3D resources, SUBMIT_3D, capsets, fences): verified.
  - Evidence: 374f224 gltest FBO readback; 9a4cc57 tinywl GLES2; 830d580 and ab7c641 SDL3 GL; c151daf DarkPlaces.
  - It needs `scripts/run-gl.sh` or `run-gl-gui.sh`; the default QEMU device is 2D only.
- `/dev/dri/card0` KMS: implemented. Ioctls:
  - resources, connector, encoder, crtc, plane, property and blob
  - SETCRTC, PAGE_FLIP, atomic (including TEST_ONLY)
  - ADDFB2/RMFB/GETFB
  - dumb buffers, GEM_CLOSE
  - cursor, master/auth
  - Leases are not supported.
- `/dev/dri/renderD128` virtgpu render node (GETPARAM, GET_CAPS, CONTEXT_INIT, RESOURCE_CREATE, MAP, EXECBUFFER, TRANSFER, WAIT): verified (0ea7fc8, `virgltest`).
- PRIME / dma-buf import and export: implemented (1567a8f; ADDFB2 of virgl resources 3cedd9f).
- Modes: partial. One synthesized 60 Hz mode per output, plus a hand-built EDID.
- Page flip and vblank: partial. Flips are synchronous and the completion event is synthesized; there are no real vblank interrupts.
- Hardware cursor plane: verified at kernel level (b198ef2, 205d46c). wlroots sessions set `WLR_NO_HARDWARE_CURSORS=1`.
- GPU buffer pinning with per-task and global pin budgets: implemented (e095550, aba707a).
- Mock DRM backend: implemented, test only (drm_mock selftest).
- UEFI GOP framebuffer (`fb.c`), later replaced by a virtio-gpu-backed fbcon: implemented.
- Kernel text console: partial. 8x16 font, scrolling, `\f \r \n \b` only; no ANSI escape parsing.

### Console, TTY, serial
- Console tty0 with an N_TTY-style line discipline: verified (the login console on every boot). Supports canonical mode, ERASE/KILL/EOF, ECHO, ISIG (^C, ^Z), ICRNL, ONLCR and raw VMIN.
- termios/tty ioctls (TCGETS/TCSETS*, TIOCG/SWINSZ, TIOCG/SPGRP, TIOCSCTTY, TIOCSCONSOWN): implemented. There are no VT_* or KD* ioctls.
- PTY driver: verified (see section 5).
- Serial 16550 COM1: partial. Polled TX only, used for the kernel log; console mirroring needs `CONSOLE_SERIAL=1`. There is no RX and no serial getty.

### Input
- PS/2 keyboard (IRQ1, scancode set 1, hardcoded Swiss-German console keymap): implemented.
- PS/2 mouse (IRQ12, 3-byte packets, no wheel): implemented.
- virtio-input (first device only, MSI-X, appears as event2): verified (205d46c, tablet under QEMU SDL).
- evdev `/dev/input/event0..2` (EVIOCG* queries, EVIOCGRAB, FIONREAD): verified (consumed by libinput under dwl/sway; 9e3c705, 4343b65). No force feedback and no multitouch slots.
- Kernel input mux behind `/dev/kbd` and `/dev/kbdraw`: implemented.

### Audio
- Intel HDA (CORB/RIRB, first DAC-to-pin path, cyclic BDL DMA, MSI): verified (0cba199, 440 Hz from SDL3 captured to wav in QEMU). Playback only, fixed 48 kHz S16LE stereo, no mixer.
- `/dev/dsp`: partial. write and poll only; there are no OSS ioctls.
- AC97 (`ac97.c`): not built (KERNEL_EXCLUDE).

### Network
- virtio-net: verified (81fec8e, real-internet HTTP). Modern PCI, MSI-X RX, synchronous polled TX, single queue, no checksum or TSO offload.

Missing drivers: USB of any kind (no xHCI/EHCI), e1000/rtl8139/any non-virtio NIC, wifi, real GPUs (i915/amdgpu/nouveau), bochs DRM, virtio-blk, virtio-sound, audio capture.

## 8. Security

MakaOS is not secure. No security audit has been done. KSEC.md and docs/SECURITY_V2.md say so; SECURITY_V2 is design only.

### Implemented
- Users and credentials (ruid/euid/suid, gids, 32 supplementary groups, down-only setuid family): implemented.
- File permission checks from mode bits: verified (34e1c6b; `vfs_check_perm` on open, create, unlink, rename, mkdir, rmdir, truncate, access and the path walk; root bypasses all but exec). But chmod and chown are fake (section 4), and files are always created 0644 (requested mode and umask ignored), owned by the creating euid/egid.
- `login` against `/etc/passwd` + `/etc/shadow`: verified (1eeee16, 6989a76). Passwords are plaintext (see Missing).
- setuid escalation through the ksec policy agent (fail-closed): implemented.
  - The `/bin/ksec` daemon is installed but never started, so escalation always fails for non-root users.
  - While no agent is registered, any root process can register itself as the agent.
- pledge (20 groups, SIGKILL on violation): partial. The dispatcher defaults to allow, so unlisted syscalls pass under every pledge. Examples: io_uring, futex, sendmsg/recvmsg, eventfd, timerfd, openpty, net_ifconfig.
- unveil (enforced on path syscalls; `restrict` tool, spawn attributes): verified (96b129a; unveil_gate selftest).
- Per-fd rights bitmask (read/write/seek/ioctl/mmap/send_fd): implemented. RIGHT_POLL and RIGHT_EXEC are not enforced.
- PROT_EXEC refused on non-executable files: verified (`dltest` bit 6).
- NX on user non-exec pages: implemented.
- SMEP on BSP and APs: implemented. No SMEP test was found.
- copy_to/from_user with access_ok and an exception-table fixup: verified (b183a70; copy_user, access_ok and copy_path_user selftests).
- Kernel CSPRNG (SHA-256 pool + ChaCha20, RDRAND/RDSEED/TSC/IRQ entropy), reachable only via `/dev/urandom` and `/dev/random`: implemented. `/dev/random` never blocks.
- Render-node ioctl allowlist, root-only reboot, console ownership revoked at logout: implemented (34e1c6b).
- Fork-bomb cap of 4096 tasks per non-root user: implemented.
- libc `__stack_chk_fail`: implemented. It only matters for ported code built with `-fstack-protector`, such as OpenJDK.

### Missing (plainly)
- ASLR: user stack, mmap and PIE base are all fixed.
- KASLR: the kernel is always at 0xFFFFFFFF80000000.
- SMAP, UMIP, KPTI / Meltdown mitigation.
- Kernel W^X: the kernel image and HHDM are mapped RW without NX.
- User W^X: W+X mappings are allowed, and `mprotect` is a no-op that reports success.
- Stack protector for MakaOS's own code: kernel and libc are built `-fno-stack-protector`. No FORTIFY_SOURCE, no RELRO.
- Password hashing: `/etc/shadow` holds plaintext `root:toor` and `maka:maka`, compared with strcmp, with no rate limiting. `AUTOLOGIN=1` skips login entirely.
- Userland randomness: libc `getrandom`/`arc4random` are an LCG seeded from the clock, and that LCG also seeds the stack guard. AT_RANDOM is TSC plus an LCG. There is no getrandom syscall.
- TLS certificate validation: there is no CA bundle.
- SO_PEERCRED: faked in libc, always uid 0.
- seccomp, namespaces, capabilities, MAC.

## 9. libc and compatibility

### Custom libc (`userland/libc`, about 21k lines, static `libc.a`)
- stdio (FILE*, printf with %f, one scanf engine, popen, fmemopen, open_memstream): implemented. There is no FILE locking; flockfile is a no-op.
- string, strings, stdlib (strtod family, qsort, env, mkstemp, posix_memalign, atexit, __cxa_atexit): implemented.
- malloc: implemented but simple. A brk bump allocator, one first-fit free list, one global lock, no coalescing.
- pthreads (create/join/detach, mutex, cond, rwlock, barrier, spin, keys, once, atfork, pthread_kill): verified (`test_posix1` 2281bef; sway/JVM runs).
- pthread cancellation: stub. `pthread_cancel` sends SIGKILL with no cleanup; testcancel is empty.
- C11 `threads.h`: implemented.
- Unnamed semaphores: implemented. Named `sem_open`: stub (ENOSYS).
- locale: stub (C / C.UTF-8 only). wchar: partial (UTF-8 conversions; ASCII-only wctype). iconv: stub.
- libm (own implementation, C99 extras): implemented. fenv: stub.
- time: partial. strftime, gmtime/localtime and mktime exist, but there is no timezone database (everything is UTC).
- BSD sockets, sendmsg/recvmsg, getifaddrs, inet_pton/ntop (IPv4): implemented.
- DNS resolver: verified (2031676). getnameinfo, getservbyname: stub.
- setjmp/longjmp, sigsetjmp: implemented.
- getcontext/makecontext/swapcontext: missing (declared only).
- termios, openpty, posix_openpt: implemented. VT100 termcap (used by bash): implemented.
- regex (own NFA ERE), fnmatch, getopt/getopt_long, wordexp (no globbing), fts, scandir, posix_spawn: implemented. glob: missing.
- pwd (parses /etc/passwd): implemented. grp: stub (always NULL). shadow/crypt: missing.
- syslog: stub (writes to stderr). utmpx: stub.
- Wrappers for epoll, eventfd, timerfd, signalfd, io_uring, shm, pledge/unveil: implemented. inotify: stub (ENOSYS).
- Static TLS (variant II, errno is `__thread`): verified (cbe02f5).
- C++: implemented. libstdc++ is rebuilt as PIC by `scripts/build-libstdcxx-pic.sh`, but only when SDL3 and Mesa are present. `cxxabi_compat.c`: not built (nothing compiles it).

### Dynamic loading
- See section 3: libc-internal dlopen, verified. There is no ld.so and no PT_INTERP.

### glibc-ABI compatibility (for prebuilt natives such as LWJGL)
- `glibc_compat.c`: verified indirectly (LWJGL natives loaded in the JVM, 37b69cb).
  - *64 aliases, `__xstat` family, `__ctype_b_loc`, `__snprintf_chk`, `__isoc23_*`, `secure_getenv`, `_dl_find_object`.
  - glibc core sonames are treated as already satisfied, and `__GLIBC__` 2.31 is faked in features.h.
- `__libc_start_main` bridge: partial. fini and rtld_fini are ignored.
- `jdk_compat.c` (umask, sigsuspend, getrusage, fchdir, mprotect-as-no-op): implemented.
- Missing: symbol versioning, IFUNC, `process_vm_readv` (ENOSYS).

### Own rewrites of Linux userspace libs (`userland/compat/linux`, built via `scripts/run-port-chain.sh`)
- libudev: stub. Hardcoded card0 and event0-2; the monitor never fires, so there is no hotplug. `port-libudev-zero.sh` (upstream 1.0.3) is an alternative installed by hand.
- libseat: stub. Always grants the seat and opens devices directly; no VT switching or revoke.
- mtdev: stub (pass-through).
- libiconv: stub (identity between UTF-8 and ASCII only).
- libevdev (compat copy): not built. It was superseded by upstream libevdev 1.13.1.

## 10. Userland programs written for MakaOS (`userland/apps`)

"Default" means a plain `bash build.sh` installs it. "If ported" means it is installed only when the third-party libraries are already in the sysroot.

| Program | What it does | Status |
|---|---|---|
| bash | Build glue for GNU bash 5.2; /bin/bash and /bin/sh, the login shell | verified (1248658); default (build.sh fetches the bash 5.2 source; `BASH_SRC` overrides). Without it, login falls back to /bin/shell |
| cat | cat | implemented (default) |
| clear | clear screen | implemented (default) |
| dhcpcd | old copy of the DHCP client | not built (not referenced by build.sh; superseded by `net`) |
| display | native framebuffer display server `/bin/makadisplay`, `/bin/makaterm` terminal, `/bin/demo_client` | implemented (default; 2c46d26, bc5214e). No run evidence |
| dltest | test: dlopen regression with sentinel bits | verified (ab25871, cbe02f5, 7ab6938) |
| doom | doomgeneric platform layer on MakaDisplay | not built (needs `./build_doom.sh` and the uninitialized doomgeneric submodule). Old evidence: 16412aa "doom running". doom1.wad is installed by default |
| dplaunch | sets SDL env and execs DarkPlaces | verified (c151daf); if ported (SDL2 + darkplaces) |
| dsobad | test: .so with an unresolvable symbol (reject path) | verified (7ab6938) |
| dsotest, dsotest2 | test: DT_NEEDED chain libraries | verified (360b17f, ab25871) |
| echo | echo | implemented (default) |
| glfwtest | test: GLFW Wayland window | verified (b094ac6; re-verified 2026-11-09: "OpenGL ES 3.1 Mesa 24.0.9", renderer virgl on D3D12, 20 frames, SMOKE TEST OK); if ported (GLFW + Mesa), autostart with GLFWTEST=1, or Alt+g in sway |
| gltest | test: offscreen GL FBO via virgl | verified (374f224); if ported (Mesa) |
| heapalias | test: frame-aliasing / COW stress reproducer | verified (04a86aa) |
| hello | printf/getpid demo | implemented (default) |
| helloraw | test: raw-syscall hello, no libc | implemented (default) |
| home | framebuffer home screen and snake game | implemented (default) |
| http_get | HTTP/HTTPS client on mbedTLS | verified (2031676) |
| jdktest | test: Hello.java for the JDK | verified (cafa1ba, 6cc0937); JDKTEST=1 only |
| ksec | ksec policy-agent daemon | implemented (default); never started |
| login | login prompt, spawned by the kernel | verified (1eeee16, 6989a76) |
| ls | ls | implemented (default) |
| lwjgltest | test: LWJGL HelloWindow under the JDK | verified (37b69cb); LWJGLTEST=1 only, inputs pre-staged by hand |
| makaclock | swaybar status clock | verified (f70d9a0, 2cbdc9c) |
| mclaunch | Minecraft launcher (Java) | not built (MCTEST=1); Minecraft itself has never been shown running |
| mkdir, mv, rm | file utilities | implemented (default) |
| net | DHCP client, writes resolv.conf, started by svcmgr | verified (af642e0) |
| piehello | test: static-PIE libc program | implemented (default) |
| pietest | test: PIE relocation check | verified (f620641) |
| ps | process list from /proc | implemented (default) |
| reboot | reboot via syscall | implemented (default) |
| restrict | run a command under pledge/unveil | implemented (default; ea7ac2a) |
| sdl2test | test: SDL2 via sdl2-compat on libSDL3.so | verified (8ca73ca); if ported |
| sdl3_audio | test: 440 Hz through SDL3 to HDA | verified (0cba199); if ported |
| sdl3_gl, sdl3_gl3 | test: SDL3 GLES2 / GL 3.3 windows | verified (830d580, ab7c641); if ported |
| sdl3_hello | test: SDL3 software window on sway | verified (a39c0ad; re-verified 2026-11-09, window seen under GPU sway); if ported, Alt+s in sway; shows 5 s, then exits |
| sdltest | test: dlopen of libSDL3.so | verified (184ca56); if ported |
| shell | native line-editing shell (not the login shell) | implemented (default) |
| sigtest | test: SA_SIGINFO / sigaltstack / thread signals | verified (fc475b2, 43c410c, e2c82c7); SIGTEST=1 only |
| smp_test | test: SMP wait-queue / fork reproducer | implemented (default; 4874bd8, no recorded pass line) |
| svcmgr | service supervisor (`/etc/services/*.svc` DAG), spawned by the kernel | verified (1eeee16) |
| test_io_uring | test: io_uring NOP/READ | implemented (default; no pass line recorded) |
| test_posix1 | test: POSIX battery | verified (2281bef "fully green") |
| test_vmalloc | test: brk demand paging, installed as /bin/vmalloc | implemented (default) |
| tlsdso | test: dynamic-TLS .so | verified (cbe02f5) |
| tone | test: 440 Hz PCM to /dev/dsp | implemented (default) |
| uname | fixed uname output | implemented (default) |
| unixsock_test | test: AF_UNIX + epoll regression | implemented (default; e6e31a9) |
| virgltest | test: render-node clear to magenta | verified (0ea7fc8) |

## 11. Third-party software

Plain `bash build.sh` builds only mbedtls and the "tier 2" static libraries (zlib, expat, libffi, pixman, libxkbcommon, freetype, harfbuzz, libdrm).
Everything else comes from `scripts/run-port-chain.sh` ("chain") or from a port script run by hand ("manual"). `build.sh` then installs whatever it finds in the sysroot.
**A fresh-checkout default image therefore has no GUI stack.** All entries below that are not built by default are effectively "not built" for a default image. The status shown is the historical run evidence once they are built.

| Software | Version | What | Route | Status |
|---|---|---|---|---|
| GNU bash | 5.2 | shell | default (fetched by build.sh) | verified (1248658) |
| DOOM (doomgeneric) | - | game | build_doom.sh + submodule | not built; old evidence 16412aa |
| mbedTLS | 3.6.2 | TLS library | default | verified (2031676) |
| zlib | 1.3.1 | compression | default | implemented |
| expat | 2.6.4 | XML | default | implemented |
| libffi | 3.4.6 | FFI (wayland) | default | implemented |
| pixman | 0.42.2 | 2D raster | default | implemented |
| libxkbcommon | 1.7.0 | keymaps | default | verified (b61c271, a0a032e) |
| freetype | 2.13.3 | fonts | default | implemented |
| harfbuzz | 8.5.0 | text shaping | default | implemented |
| libdrm | 2.4.125 | DRM userspace | default | verified (374f224) |
| wayland | 1.23.1 | protocol libs | chain | verified (4343b65) |
| wayland-protocols | 1.38 | protocol XML | chain | implemented |
| wlroots | 0.18.1 | compositor library (gles2 renderer, gbm allocator) | chain | verified (4343b65, e79b1dc) |
| sway (+swaybar, swaynag, swaymsg) | 1.10.1 | tiling compositor | chain | verified (e79b1dc, 95dc7ab, 3b58d13) |
| dwl | 0.7 | compositor | chain | verified (4343b65, docs/img screenshots) |
| tinywl | wlroots 0.18.1 | minimal compositor | chain | verified (9a4cc57, GLES2 over virgl) |
| foot | 1.20.2 | terminal | chain | verified (4343b65, 95dc7ab, screenshots) |
| fcft | 3.3.1 | font lib for foot | chain | verified via foot |
| tllist | 1.1.0 | header lib | chain | implemented |
| libutf8proc | 2.9.0 | unicode | chain | implemented |
| fontconfig | 2.15.0 | font config | chain | verified via foot |
| swaybg | 1.2.1 | wallpaper | chain | verified (e79b1dc, a3cd70a) |
| tofi | 0.9.1 | launcher | chain | verified (a3cd70a) |
| fuzzel | 1.11.1 | launcher | not in chain | not built (run port-fuzzel.sh and add an install line) |
| wlr-randr | 0.5.0 | output config CLI | chain | implemented (no run evidence) |
| cairo | 1.18.2 | 2D graphics | chain | verified via swaybar (e79b1dc) |
| pango | 1.54.0 | text layout | chain | verified (43c7550, 3b58d13) |
| glib | 2.82.2 | utility lib | chain | implemented |
| fribidi | 1.0.16 | bidi | chain | implemented |
| pcre2 | 10.44 | regex | chain | implemented |
| json-c | 0.18 | JSON (sway IPC) | chain | implemented |
| libpng | 1.6.44 | PNG | chain | implemented |
| libdisplay-info | 0.2.0 | EDID parsing | chain | implemented |
| hwdata | host copy | pnp.ids | chain | stub (copies host data) |
| libinput | 1.26.2 | input | chain | verified (4343b65, a0a032e) |
| libevdev | 1.13.1 | evdev lib | chain | implemented |
| mtdev, libseat, libudev, libiconv | own rewrites | compat | chain | stub (section 9) |
| libudev-zero | 1.0.3 | udev replacement | manual | not built (manual) |
| Mesa | 24.0.9 | GL: virgl gallium only, EGL, GBM, GLES2, desktop GL | port chain (before wlroots) | verified (374f224, 830d580; re-verified 2026-11-09: sway renders through it, glfwtest reports GLES 3.1) |
| GLFW | 3.4 | GL windowing (Wayland) | manual | verified (b094ac6, 37b69cb) |
| SDL3 | 3.2.0 | media library, custom MakaOS audio backend | chain/manual | verified (184ca56, a39c0ad, 830d580, 0cba199) |
| sdl2-compat | 2.30.50 | SDL2 API on SDL3 | auto if SDL3 + Mesa present | verified (8ca73ca) |
| DarkPlaces | git 5b08123 | Quake/Xonotic engine | manual (port-darkplaces.sh, pinned commit) | verified (c151daf, renders Quake demos via virgl). Under sway it hits virgl handle errors (4cf29f7). 2026-11-09: builds; not run (needs Quake data) |
| Quake shareware data | - | pak0.pak | fetch-quake-shareware.sh | verified with DarkPlaces (c151daf) |
| libjpeg, libogg, libvorbis | 9e, 1.3.5, 1.3.7 | DarkPlaces deps | manual | implemented |
| Avian JVM | - | small JVM | manual, AVIANTEST=1 | verified (46f31ef "prints Hello World"; re-verified 2026-11-09: "Hello from Avian JVM on MakaOS") |
| OpenJDK | 17.0.19 Zero | JVM (interpreter only) | manual, JDKTEST/LWJGLTEST/MCTEST | verified (cafa1ba, 6cc0937: `java -version`, Hello; re-verified 2026-11-09: Hello, 800 MB heap allocated and touched, `2 + 2 = 4`) |
| LWJGL | prebuilt | Java GL bindings | LWJGLTEST=1, staged by hand | verified (37b69cb, GL 4.2 window) |
| Minecraft | 1.20.4 | game client | MCTEST=1 + fetch-minecraft.sh | not built; never shown reaching gameplay (docs/JVM_PORT_PLAN.md "unverified") |

## 12. Desktop

- Default session: verified. The kernel spawns `/bin/login` and `/bin/svcmgr`, and login drops to bash on the framebuffer console.
  - The desktop starts by typing `sway` (or `dwl`, `tinywl`) once the ports are built.
  - `AUTOLOGIN=1` makes login exec `/bin/sway` as root without a password.
- Native compositor `makadisplay` + `makaterm` (custom Unix-socket protocol, ANSI terminal on a pty): implemented (default). No run evidence or screenshot.
- sway 1.10.1 with swaybar, makaclock status, tofi menu and foot terminal: verified (e79b1dc on 4-CPU SMP; 95dc7ab "foot opens a terminal with a live bash prompt under sway"). Re-verified 2026-11-09 from a fresh clone, without KVM: GPU rendering via virgl (wallpaper, swaybar, foot with bash, SDL3 and GLFW windows) and the software renderer on 2D virtio-gpu (`WLR_RENDERER=pixman`). sway takes 45 to 110 s to show its first frame under software emulation. No sway screenshot is in docs/img.
  - Config: `/etc/sway/config` is generated by build.sh, with a Swiss keymap from `/etc/keymap.xkb` and Mod1+Return for foot.
- dwl 0.7 + foot: verified (docs/img/foot-on-dwl-2026-06-11.png, foot-on-dwl-threaded-2026-06-11.png).
- tinywl: verified (9a4cc57).
- Wallpaper swaybg: verified (e79b1dc). The default image is `/usr/share/backgrounds/sway/wallpaper.png`.
- Launcher tofi in stdin mode (`ls /bin | tofi`, because there is no shebang support): verified (a3cd70a). fuzzel: not built.
- Display settings: partial. wlr-randr is installed, but DRM exposes one mode per output, so there is nothing to switch to. `userland/apps/display` is the makadisplay server, not a settings tool.
- Input stack: verified (a0a032e "keyboard and pointer both work"). It runs PS/2 or virtio-input, then evdev, then libinput, libevdev and mtdev, with the libudev shim (static device table) feeding wlroots.
- Seat: stub. libseat always grants; there is no VT switching.
- GL path: verified (374f224, 830d580, ab7c641, b094ac6, c151daf). Mesa virgl, then the render node, then virtio-gpu 3D.
  - It needs the virgl QEMU launch (`scripts/run-gl.sh` / `run-gl-gui.sh`).
  - There is no llvmpipe/softpipe, so there is no software GL fallback.
  - Open issue: GL clients under sway hit "Illegal resource" virgl errors (ab7c641, 4cf29f7).
  - Unverified: wlroots is built with only the gles2 renderer, so whether sway works on the default 2D virtio-gpu launch was not confirmed.
- Fonts: implemented. DejaVu and xkeyboard-config are copied from the host at build time; without them foot shows blank text.
- Missing: XWayland, a display manager, a session switcher, hardware cursors in wlroots sessions (disabled on purpose).

## 13. Debugging and development

- GDB through QEMU's gdbstub (`-gdb tcp::1234`, `gdb.sh`, `scripts/debug-pf.sh`, `scripts/auto-repro-pf.sh`): verified as used (c658a2d, 8d3b85a). There is no in-kernel GDB stub.
- Serial log on COM1 to `build/serial.txt`: verified (the evidence source for most run commits). `serial_*_dbg` is compiled out (`KERNEL_DEBUG_SERIAL=0`).
- kprintf (%s %c %d %u %lu %x %p, no width or precision): implemented.
- Structured log `pr_*` with a 256-entry ring dumped on panic: implemented. Only 33 call sites use it.
- Per-CPU trace ring (512 entries, `TRACE()`): implemented. 10 call sites; DEBUGGING.md's claim that nothing calls it is stale.
- Frame-pointer stack walker with a symbol table generated by `nm` at link time (two-pass link): implemented. There is no DWARF unwinding.
- Panic: implemented. It halts other CPUs via IPI and dumps registers, the stack walk, the task, and the log and trace rings, then hangs for GDB. No captured panic dump was found in history.
- Exception fixup table (one entry: the uaccess copy): verified (b183a70).
- Boot self-test battery (91 selftests called from `kernel/main.c`): verified (0683566, f193585, 0aa4584, docs/AUTOFIX_LOG.md). Not built by default: `SELFTESTS=1`.
  - Primitives: checked arithmetic, Chase-Lev, slab, kheap, pmm, RCU typesafe.
  - User access: uaccess, mmap ranges, user pointers.
  - Processes: creds, task exit, fd table, exec teardown, ksec, unveil, signal permissions, ELF bounds.
  - Filesystem: ext2 ops and geometry, dcache, virtfs.
  - I/O: io_uring, eventfd, timerfd, pipes, epoll, signalfd.
  - Networking: sockets, unix, SCM_RIGHTS, TCP ring/accept/synreap/drain, UDP.
  - Drivers: AHCI PRDT, virtio, NVMe, HDA, DRM mock and sizes.
  - TTY and input: tty, pty, evdev.
- virgl selftests: implemented. They always run when the host offers virgl.
- Stress harnesses (pread, NVMe, AHCI in `main.c`): implemented but never called.
- `ASSERT` macros: defined, unused.
- Build and boot flags (`build.sh`, all off by default):

| Flag | Effect | Status |
|---|---|---|
| SELFTESTS=1 | boot self-test battery + scheduler watchdog | verified (0683566) |
| CONSOLE_SERIAL=1 | mirror user console to serial | implemented (4420e00) |
| SIGTEST=1 | build, install, boot-run /bin/sigtest | verified (fc475b2) |
| AVIANTEST=1 | boot-run Avian on Hello.class | verified (46f31ef) |
| JDKTEST=1 | 1 GiB rootfs, stage /jdk, boot-run java Hello | verified (cafa1ba, 6cc0937) |
| LWJGLTEST=1 | stage LWJGL, autostart under sway | verified (37b69cb) |
| MCTEST=1 | 3 GiB rootfs, stage Minecraft (MC_VER=1.20.4) | implemented (c6252b3; no gameplay evidence) |
| GLFWTEST=1 | autostart glfwtest in sway | verified (b094ac6) |
| AUTOLOGIN=1 / AUTOLOGIN_SPEC | passwordless login to a command (default root:/bin/sway) | implemented (fb69893) |
| NO_QEMU=1 | do not launch QEMU after building | implemented |
| MINIMAL=1 | kernel + libc + core in-tree programs only (login, /bin/shell, svcmgr, net, ls, ps, coreutils); no ports, no cross-compiler needed | verified (2026-11-09: builds in 20 to 75 s from a fresh clone; boots to a root login under QEMU 9.2 TCG) |
| REBUILD_MBEDTLS, JDK_VARIANT, BASH_SRC | build knobs | implemented |

- Run scripts:
  - `run.sh`: KVM, 4 CPUs, OVMF, AHCI, virtio-vga, virtio-tablet, HDA, virtio-net with host 18080 forwarded to guest 80. The kernel needs x2APIC: KVM provides it, and so does software emulation (TCG) from QEMU 9.0 on (8.2 does not). `scripts/qemu-host.sh` picks KVM when usable, else a QEMU 9+ with `-cpu max,+x2apic`, else refuses with a clear message. Verified 2026-11-09: QEMU 9.2.0 TCG boots both the minimal and the full image to a root login.
  - `run-sway-gui.sh`.
  - `scripts/run-gl.sh` and `run-gl-gui.sh` (virgl).
  - `scripts/run-sdltest.sh`, `run-quake-shot.sh`, `compositor-test.sh`, `boot.sh`.
- Missing: KASAN-style sanitizers, memory poisoning, an NMI watchdog, loadable modules, an in-kernel debugger.

## Kernel internals (mechanisms)

- Spinlock (CAS test-and-set; disables preemption; trylock and irqsave variants): implemented (`kernel/include/smp.h`).
- Ticket lock: implemented (defined, zero users).
- Seqlock: implemented (DRM state, ext2 inode-cache leaves).
- Wait queues (lock-free MPSC push, xchg drain, wait groups): implemented (`kernel/wait.c`). Exercised by `smp_test`.
- `irq_wait` slots (256 per-slot wait queues for driver IRQs): implemented.
- Preempt counter (per-CPU, `%gs`-relative), and sleeping with preemption disabled panics: implemented (0e04789).
- RCU, QSBR style (read lock = preempt off; quiescent state on context switch or idle; expedited via IPI; call_rcu with a 10 ms GP kthread; rcu_barrier): verified (8c0abb3 fix + slab_typesafe selftest).
- Per-CPU `cpu_t g_cpus[64]` via GS_BASE: implemented.
- IPIs (reschedule, function call, TLB flush, halt): verified (9a1bbbe, e2730c9).
- TLB shootdown: verified (section 2).
- Chase-Lev deque: verified (section 3).
- MLFQ with steal-on-idle balancing: implemented (section 3).
- Buddy allocator, pcp, slab, shrinker, kmalloc: see section 2.
- Page cache, ext2 block cache, dentry cache, inode cache: see sections 2 and 5.
- VMA list with RCU readers; COW per-frame refcount and 16-bit pin count: implemented / verified (section 2).
- uaccess (`_access_ok`, copy_to/from_user with prefault, fixup-protected `rep movsb`): verified (selftests).
- Checked arithmetic helpers (`checked.h`: ckd_add/mul, index_ok, in_range): verified (checked selftest).
- Initcalls: two levels (EARLY in kmain, SUBSYS in the init kthread), a dependency DAG with Kahn sort, panic on a cycle: implemented (2232121).
- Intrusive FIFO list (`ilist.h`): implemented. SPSC ring (`spsc.h`): defined, unused.
- Kernel stacks (8 KiB plus an unmapped guard page, slot recycling, up to 65536): implemented.
- DMA: per-driver PRDT/descriptor building with pinned frames (AHCI, virtio, DRM): implemented. There is no generic DMA API and no IOMMU.
- Block request queues / NCQ: missing (section 7).
- File refcounting, fdget/fdput: implemented (section 5).
- Net stack: skb buffers, TCP state machine, fixed-RTO retransmit thread, no congestion control: see section 6.
- Missing: MCS/qspinlocks, rwlocks, sleeping mutexes, semaphores, hrtimers/timer wheel, a generic MSI layer, a block layer, an oops path (kernel faults either fix up or panic), a module loader.

## 14. Known not working or missing

Notable gaps a reader would expect from an OS:
- Hardware: USB, non-virtio NICs, wifi, real GPUs, virtio-blk, audio capture, ACPI power-off/sleep, HPET/RTC (no wall clock), AVX state.
- Storage: no block layer, no NCQ, NVMe not usable as root, ext2 only (no symlinks, hardlinks, timestamps, files over about 64 MiB), FAT32 not built, no mount syscall, no tmpfs or sysfs.
- Processes: no stop/continue (SIGTSTP kills), no real-time signals, asynchronous signals delivered only at syscall return, futex WAIT/WAKE only, no ptrace, no core dumps, no CPU affinity, no PT_INTERP / ld.so.
- Memory: no mprotect (silent success), no mremap, no swap, no huge pages, no writable shared file mappings, no ASLR.
- Networking: no IPv6, no loopback, fixed-RTO TCP with no congestion control or window scaling, most socket options faked, no TLS certificate validation.
- ABI: not Linux-ABI compatible; Linux binaries do not run directly. glibc-ABI compat is limited to what the JVM and LWJGL needed.
- Userland: no glob, no ucontext switching, no timezones, no locales, no shebang support, plaintext passwords.
- Desktop: needs ports built by hand; GPU compositing needs the virgl QEMU launch; GL clients under sway show virgl resource errors; no XWayland; one display mode.
- Minecraft: staging exists, but it has never been shown running.
