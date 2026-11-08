# Porting a JVM to MakaOS — plan

Goal: run real Java on MakaOS. MakaOS has a **custom syscall ABI** (write=0,
exit=1, ... — not Linux-numbered), so the JVM is **ported from source** against
MakaOS's Linux-flavored libc, exactly like every other port. We are NOT running
precompiled Linux JDK binaries (that would need a Linux syscall personality —
out of scope).

## Readiness assessment (verified 2026-07-11)

Ready:
- **Threads** — pthreads (`pthread_create/join`, C11 threads). ✓
- **mmap + PROT_EXEC** — anonymous + file-backed executable mappings (dlopen
  already loads + runs `libSDL3.so`); needed for JIT code pages. ✓
- **dlopen** — real dynamic loading of `.so` (SDL3, sdl2-compat). JNI native
  libs + `libjvm.so` load this way. ✓
- **libffi** — already ported (`scripts/port-libffi.sh`, incl. `java_raw_api.c`).
  This is the JNI/foreign-call substrate. ✓
- **libstdc++** — ported, PIC. ✓
- **Filesystem** — ext2 rootfs for the JDK's `lib/modules` / class files. ✓
- **TLS** — `__thread` works; errno is now a POSIX macro. ✓
- **Time** — `clock_gettime` / monotonic. ✓ (confirm CLOCK_MONOTONIC resolution)

Gaps (the real work):
- **⚠ SA_SIGINFO signal delivery — THE gate.** MakaOS delivers signals as a
  simple `sa_handler(int)`: the kernel sets `rdi=signum` but does NOT populate
  `rsi=siginfo*` / `rdx=ucontext*`. HotSpot *requires* `SA_SIGINFO` — it reads
  `siginfo->si_addr` (fault address for implicit null checks) and
  `ucontext->uc_mcontext.gregs[]` (to read/advance the faulting PC for
  safepoint-poll and null-check resumption). The register-save mechanism already
  exists (`kernel/proc/signal.h sigframe_t` saves rip/rsp/rflags/GPRs/FPU and
  restores them on sigreturn), so the work is bounded: deliver a real
  `siginfo_t` in rsi and a **glibc-compatible `ucontext_t`** in rdx, populate
  `uc_mcontext.gregs[]` from the sigframe, and honor handler modifications on
  sigreturn. Kernel + libc change.
- **⚠ os_linux assumptions in OpenJDK.** `/proc/self/maps` (memory probing),
  `sysconf` (page size / nproc — partly present), NPTL pthread internals, vDSO
  (`clock_gettime` fast path). Each surfaces during the os-layer port; most map
  to existing MakaOS facilities or small additions.
- **Custom syscall ABI** — recompile from source (a port). ✓ by design.

## Phased plan

### Phase 0 — Prerequisites / de-risk (kernel + libc)
1. **SA_SIGINFO + glibc `ucontext`.** Wire `siginfo_t*` (rsi) and `ucontext_t*`
   (rdx) into signal delivery; define glibc-layout `ucontext_t` / `mcontext_t` /
   `gregs[REG_*]` in libc; map `sigframe_t <-> uc_mcontext.gregs`; make
   `sys_sigreturn` restore from the (possibly handler-modified) context. Add a
   selftest: a SIGSEGV handler reads `si_addr`, advances `gregs[REG_RIP]` past
   the faulting instruction, and execution resumes. This is generally correct
   POSIX, not JVM-specific.
2. Confirm thread stack **guard pages**, `clock_gettime(CLOCK_MONOTONIC)`, large
   anon mmap, and `mprotect` (for the safepoint polling page).

### Phase 1 — First light with a SMALL JVM (fast milestone, de-risk)
Port **Avian** (preferred: small, C++, bundles its own class library, minimal OS
deps, near-static binary) or **JamVM**. These do **explicit** null checks +
cooperative safepoints, so they likely run on MakaOS's *current* simple-signal
support — proving the OS can host a JVM (threads + class loading + GC +
interpreter/JIT) before the OpenJDK marathon. Goal: `Hello.class` prints.

### Phase 2 — OpenJDK Zero (the real JDK, no JIT)
Cross-compile OpenJDK with the **Zero** interpreter to `x86_64-pc-makaos`.
- Create the OS/CPU layer: `os_makaos.cpp` + `os_cpu/makaos_x86` derived from
  `os_linux*` (thread mgmt, signals -> our SA_SIGINFO/ucontext, memory, time,
  dlopen).
- Build `java.base` + native libs (`libjava`, `libjvm`, `libnet`, `libnio`,
  `libzip`) against MakaOS libc.
- Goal: `java -version`, then `java Hello`. Zero sidesteps the JIT port entirely.

### Phase 3 — HotSpot JIT (performance)
Enable C1 (client) then C2, emitting x86-64 into PROT_EXEC mmap. Depends on
Phase 0's SA_SIGINFO/ucontext being solid (implicit null checks + SEGV-based
safepoint polling). Goal: real-speed Java.

### Phase 4 — JDK completeness
Fill shared libc/syscall gaps as OpenJDK hits them (in the shared libc/kernel per
the `userland/compat/linux/` discipline — never per-port). Timezone DB, fonts
(fontconfig/freetype already ported), networking (NIO), crypto.

## Recommended immediate order
Phase 0 (SA_SIGINFO/ucontext) -> Phase 1 (Avian first-light) in parallel, then
commit to Phase 2 (OpenJDK Zero). Phase 0 is the highest-leverage, most-reusable
piece (it is correct POSIX signal handling regardless of the JVM).

## Biggest risks
- The `os_linux -> os_makaos` port (signals/ucontext, thread management) is the
  crux of Phase 2.
- OpenJDK's build system cross-compiling to a novel target is finicky (expect
  `--openjdk-target`, a bootstrap JDK on the host, and configure hacking).
- Latent glibc-specific assumptions surfacing one at a time.

---

## Update (2026-07-11): recon done, ordering confirmed, Phase-0 design locked

Avian recon (source cloned to build/third_party/avian; host `javac 25` + `java`
present; zlib/make/g++ present) found that **Avian's own posix.cpp uses
`SA_SIGINFO` + `ucontext`** (`src/system/posix.cpp:653` sets `sa_flags=SA_SIGINFO`;
`:965` casts the 3rd arg to `ucontext_t*`) to coordinate its stop-the-world GC
(signal a thread, read its ucontext to walk the stack) and for SEGV null-checks.
So the signal layer is NOT skippable for Avian -- it **is** "the missing stuff,"
and it's the same as the OpenJDK Phase 0.  Confirmed order: **signal layer ->
Avian -> OpenJDK.**

Missing-stuff checklist (verified):
- [x] libc `<ucontext.h>` -- glibc-layout `ucontext_t`/`mcontext_t`/`gregs[REG_*]`
      (done: userland/libc/include/ucontext.h).
- [x] Kernel: `SA_SIGINFO` delivery -- **DONE + verified** (sigtest PASS, both
      paths).  The rt_sigframe `{ pretcode, ucontext, siginfo }` is built by a
      single source-neutral builder (`build_rt_frame`/`build_simple_frame` in
      kernel/proc/signal.c) that reads a `sig_uctx_t` and emits a
      `sig_redirect_t`, fed from EITHER a syscall kframe (syscall-return
      delivery) OR an exception trap frame.  `sys_sigreturn` restores from
      `uc.uc_mcontext.gregs[]` so a handler's `gregs[REG_RIP]` edit takes effect.
      Crucially, synchronous faults now deliver to handlers too:
        - `isr_common_entry` (isr_stubs.asm) passes the C handler a pointer to
          the REAL on-stack `trap_frame_t` (idt.h), not a copy, so edits to
          ip/sp/flags + GPRs take effect on POP_GPRS + iretq.
        - `signal_deliver_fault(sig, iframe)` (signal.c) redirects the trap frame
          into a catchable, unblocked handler, else forces SIG_DFL + terminates
          (Linux force_sig semantics).  Wired into every synchronous exception
          (#PF via vmm.c, #DE/#UD/#GP/... via idt.c).  The #PF path suppresses
          its PF-KILL banner for a deliverable fault (`signal_fault_deliverable`).
        - `sys_sigaction` now permits catching SIGSEGV/SIGBUS/SIGFPE/SIGILL/
          SIGTRAP (only SIGKILL/SIGSTOP uncatchable, POSIX).
      Regression test: userland/apps/sigtest (build `SIGTEST=1`) exercises both
      the syscall-return path (raise SIGUSR1) and the fault path (NULL deref,
      si_addr==0, gregs[REG_RIP] redirect resumes at recovery()).
- [x] `pthread_kill` -- **DONE** (commits 27afb67 + e2c82c7). Thread-directed via
      kill(tid) (pid == tid; no separate tgkill syscall needed). This EXPOSED and
      fixed a real POSIX bug: signal dispositions were per-task, so a pthread got
      a zeroed handler table; moved them into a refcounted shared `sighand_t`
      (per-process, like task_mm_t/task_files_t). A JVM installs handlers in one
      thread that all mutator threads must honour.
- [x] `sigaltstack` + `SA_ONSTACK` -- **DONE** (commit 43c410c). Per-thread alt
      stack via sys_sigaltstack; both frame builders route through
      sig_stack_base() so a SA_ONSTACK handler (JVM StackOverflowError) runs on
      the alt stack for BOTH syscall-return and synchronous-fault delivery. fork
      inherits, execve disables. libc sigaltstack()/stack_t/SS_* in <signal.h>.
      sigtest Path 4 asserts the handler's rsp is inside the alt stack.

The signal layer (SA_SIGINFO/ucontext + pthread_kill/shared-dispositions +
sigaltstack) is COMPLETE and verified by userland/apps/sigtest (4 paths, all pass
under a `SIGTEST=1` boot). Next is Phase 1 (Avian first-light).

## Phase 1 status (2026-07-12): Avian prints Hello World on MakaOS

`scripts/port-avian.sh` cross-builds Avian (interpreter) to a MakaOS x86-64 PIE
`avian` + classpath.jar; `AVIANTEST=1 bash build.sh` installs and boot-spawns it.
The JVM runs end to end: it loads and links classes from classpath.jar, runs the
bytecode interpreter, and executes `Hello.main`, which prints
`Hello from Avian JVM on MakaOS` and continues cleanly. Boot the AVIANTEST image
at **-m 1024M** (MakaOS hangs very early at 2048M, a separate RAM-size bug).

Three fixes past the "JVM starts but no main" state:

1. **`NoClassDefFoundError: java/lang/String$1`** was not a jar/IO/hash bug: the
   boot classloader had an empty finder. Fix: `kernel/main.c` passes
   `-Xbootclasspath:/avian/classpath.jar` in the boot-spawn argv.
2. **`UnsatisfiedLinkError` on native methods** (Avian_java_lang_System_arraycopy):
   Avian resolves builtins via `dlsym(dlopen(NULL), ...)`, i.e. the main exe's
   dynamic symbol table. Avian's own `make` links a static ET_EXEC with no
   PT_DYNAMIC/DT_HASH, so those symbols cannot be found. Fix: `port-avian.sh`
   bypasses that link and does an `ld` PIE link (`-pie --export-dynamic
   --hash-style=sysv -T userland/link-pie.ld`, libc.a before libstdc++.a). MakaOS
   maps the ET_DYN at VMM_USER_CODE_BASE; elf.c applies R_X86_64_RELATIVE.
3. **`abort()` in `findInterfaceMethod`** during HashMap bootstrap: an
   `invokeinterface` of a `java.lang.Object` method (Avian's own HashMap.java calls
   `cell.hashCode()` on a `Cell` interface reference) resolves the target to
   Object, which is not an interface and is absent from the receiver's interface
   table, so the itable scan falls through to abort. This is a genuine Avian
   interpreter+JIT gap. Fix (patched into the vendored source by port-avian.sh, in
   the shared `findInterfaceMethod` so it covers both): if the resolved method's
   class is not an interface, dispatch it virtually, per the JVMS invokeinterface
   rules.

Debugging note: at `-O3`, `__builtin_return_address` in a `NO_RETURN` abort plus
the interpreter's `vmRun` assembly break the C frame chain, so line attribution is
unreliable (resolveClassInObject / baseSize / stringHash were all falsely blamed).
Capturing the call site via `__builtin_FILE()/__builtin_LINE()` default args on
`expect`/`abort` pinned it exactly. The final build is a clean, fully-optimized
`-O3` (no `-O0` or `-fno-strict-aliasing` needed).

Earlier build gotchas (already encoded in port-avian.sh): `java-version=8` (Avian's
old `javac 1.x` version parse breaks on a modern JDK), `rdynamic=-Wl,--export-dynamic`
(the makaos gcc rejects bare `-rdynamic`), libc `extern "C"` header guards +
netinet/ip.h + ctime_r (commit 6d1c9fe -- Avian is the first big C++ port), and
two tiny Avian portability patches (posix.cpp limits.h, java-lang.cpp sysctl).
