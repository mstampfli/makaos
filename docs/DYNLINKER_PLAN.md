# MakaOS dynamic loader (dlopen) -- implementation plan

Goal: real `dlopen`/`dlsym`/`dlclose`/`dlerror` loading actual PIC `.so`s, so
sdl2-compat / DarkPlaces / Mesa DRI / (later) LWJGL+JVM work unmodified and the
static-bind hacks (SDL static-ANGLE EGL, planned sdl2-compat patch) get deleted.

## Architecture: PIE executables + in-libc runtime loader (no separate ld.so)
Decided against `-no-pie`: a `-no-pie` ET_EXEC cannot carry a usable dynsym on
this toolchain (verified: every `--export-dynamic` variant gave `PT_DYNAMIC=0`).
PIE is the correct base -- executables become ET_DYN, naturally get `.dynsym` +
`PT_DYNAMIC` + relocations, and exe + every `.so` are uniform base-relocated
objects. Load-base math is the correct mechanism, not a cost to avoid.
Executables stay self-contained (no PT_INTERP, no ld.so bootstrap); `dlopen` is
a libc routine that loads `.so` plugins and binds their imports to the PIE exe's
exported `.dynsym` (base + st_value) + previously-loaded objects.

## Toolchain reality (Phase 0 is the real gate)
The cross-toolchain was purpose-built for static `-no-pie` ELF:
`--disable-shared`, no `Scrt1.o`/dynamic-linker, and a default linker script
that forces a fixed base (so `-pie` still emitted `ET_EXEC`, `PT_DYNAMIC=0`).
`libc.a` has non-PIC `R_X86_64_32S` relocs. So Phase 0 = teach it dynamic/PIE
ELF. Most `scripts/port-*.sh` libs are already `-fPIC`.

## Phases (each with a provable milestone)
0. Toolchain emits PIE -- **DONE** (commits: PIC libc + PIE link path; kernel
   backing-file reloc fix). No binutils/gcc rebuild.
   - Link the final exe via `ld -pie` DIRECTLY (gcc's driver forces
     `-static -T makaos-link.ld` -> ET_EXEC; even `-Wl,-pie` came out ET_EXEC).
   - `USER_CFLAGS` is now `-fPIE` (NOT `-fPIC`: `-fPIE` keeps the executable's
     own `__thread` in LOCAL-EXEC / TPOFF, link-resolved; `-fPIC` would default
     `__thread` to general-dynamic and pull in `__tls_get_addr` -- a Phase 4
     thing). libc.a then carries only RELATIVE + TPOFF relocs, no 32/32S, so it
     links into a PIE; ET_EXEC apps are unaffected (PIE codegen at fixed base).
   - crt0 (`entry.asm`) was ALREADY PIC (all `[rel ...]`), no change needed.
   - `userland/link-pie.ld` = ld's stock `-pie` script + the `__tdata_end` /
     `__tbss_end` symbols `libc/tls.c` needs (the stock `.xd` omits them).
   - MILESTONE (verified headless): `piehello` -- a real libc program (crt0 +
     libc.a) built as a static PIE -- loads, relocates, runs crt0's TLS +
     `.init_array`, and libc/TLS/malloc all work. PF-KILL sentinel `0x5EC0DE1F`.
   - For `.so`s (Phase 5): `ld -shared` + a `-shared` variant of link-pie.ld.
1. Kernel ET_DYN exec: load base, map PT_LOADs, apply exe `RELATIVE` relocs,
   jump `base+entry`; aux vector (AT_PHDR/PHENT/PHNUM/BASE/ENTRY/EXECFN).
   **DONE** for RELATIVE (IRELATIVE/ifunc still TODO).
   - GOTCHA (fixed): the exec path hands elf_load_into only the first-PAGE header
     buffer; the reloc pass must read .dynamic/.rela.dyn/target pages from the
     BACKING FILE (elf_read_at), not `data`, or any PIE > 4 KiB relocates nothing.
2. libc `dlopen`/`dlsym`: map `.so`, base-relocate, resolve imports, run init.
   MILESTONE 1 **DONE**: no-import `.so` (dsotest) -> dltest dlopen/dlsym/call/
   dlclose end to end (SysV hash; `ld -shared` emits SysV HASH not GNU_HASH).
   Segments map file-backed MAP_PRIVATE per-PT_LOAD (text R+X, data R+W, BSS
   zeroed) -- no mprotect, W^X holds.  Handles RELATIVE + GLOB_DAT/JUMP_SLOT/64.
   - KERNEL GAPS this needed (fixed, deny-by-default): open() now grants
     RIGHT_MMAP_X only for files the caller may EXECUTE (acl_check); PROT_WRITE
     MAP_PRIVATE needs only RIGHT_MMAP_R (COW never writes the file).
   - GOTCHA: a FAILED background build leaves a stale disk.img -> QEMU runs the
     old kernel and every result lies.  Build in the FOREGROUND (or verify the
     kernel binary changed) before trusting a headless run.
   MILESTONE 2 **DONE**: a `.so`'s libc imports (GLOB_DAT/JUMP_SLOT vs UND syms)
   bind against the PIE exe's exported `.dynsym` -- dsotest's dso_strlen() calls
   the exe's strlen.  The exe scope is derived from the auxv PHDRs libc saves
   (__libc_phdr...): exe base = AT_PHDR - PT_PHDR vaddr; dynsym via PT_DYNAMIC.
   TODO: RELRO mprotect (no mprotect syscall yet).
3. init/fini + `DT_NEEDED` deps + `dlopen(NULL)` + `dlclose` refcount.
   **DONE**: `DT_NEEDED` loads deps recursively (/lib/<name>), published-before-
   deps so circular chains dedup; `dlopen(NULL)` global scope; path dedup +
   refcount; `DT_FINI_ARRAY` on dlclose.  Tested: libdso2.so as a DT_NEEDED dep,
   dlopen(NULL)+dlsym.  TODO: per-object dep list so dlclose unloads deps;
   `dlerror` is not yet per-thread (TLS) -- fine until threads dlopen.
4. Dynamic TLS -- **DONE** (the hard part).  `__tls_get_addr(module,offset)` +
   a per-thread DTV (a `__thread` array in the static TLS -> lock-free hot path);
   the loader registers each `.so`'s `PT_TLS` as a module and resolves
   `DTPMOD64`->id / `DTPOFF64`->offset.  The `.so` binds `__tls_get_addr` against
   the exe's exported one.  General-dynamic only: `TPOFF64` (initial-exec) in a
   `.so` is rejected.  Module id + offset bounded (V12).  Tested: tlsdso.so's
   `__thread` counter is per-thread across a spawned thread (CR2=0x5EC03FFF).
   CROSS-MODULE `__thread` **DONE** (Phase 5 needed it for libc's `errno`): a
   GD-TLS reloc against an UNDEFINED symbol resolves to its DEFINING module via
   `tls_resolve` (loaded `.so`s, then the exe).  The main exe is reserved TLS
   module id 1 (`is_static`): `__tls_get_addr(1, off)` returns the per-thread
   static-TLS address `tp - tls_size + off` (matching the exe's own local-exec
   access to the same var), where `tls_size = __makaos_static_tls_size()` from
   libc.  So a `.so` and the exe share libc's `errno` per thread.
   TODO: TLS unload on dlclose (module slots + per-thread blocks currently leak).
5. Build `libSDL3.so`; sdl2-compat builds unmodified (its dlopen(libSDL3) works);
   then DarkPlaces dynamic loading + Xonotic; foundation for LWJGL/JVM.
   **DONE (real):** the loader loads the actual 83 MB `libSDL3.so.0` end to end
   (sdltest, CR2=0x5D10001F) and **sdl2-compat runs UNMODIFIED over it**
   (sdl2test, CR2=0x5D20001F): a plain SDL2 program links `libSDL2.a`
   (sdl2-compat `release-2.30.50`, the tag that targets our SDL3 3.2.0), whose
   `SDL_Init` `dlopen("libSDL3.so.0")` + dlsym-fills the SDL3 jump table and
   forwards to SDL3 -- SDL_Init/GetVersion/WasInit/GetError/Quit all round-trip.
   No static-bind hack: `dlopen` of a BARE soname now resolves via /lib (same
   rule as DT_NEEDED), and the exe provides libc (+ errno) to the .so.
   - KNOWN LIMITATION (deferred): the loader ignores RTLD_LOCAL -- every dlopen'd
     object joins the global resolve scope.  Harmless for the current paths
     (nothing depends on local-scope isolation; sdl2-compat dlsyms SDL3 per-
     handle, and libSDL3 imports no SDL_* from the exe), but real RTLD_LOCAL
     wants per-handle symbol visibility.  Revisit if a plugin needs isolation.
   NEXT: DarkPlaces (PRELOAD static, link -lSDL2) + Xonotic assets.

## Risks
- Phase 0 toolchain surgery (PIE script may be missing from ld; PIC libc/crt).
- TLS (Phase 4) is the mountain.
- Relocation/ifunc coverage: audit `libSDL3.so`'s `.rela` early
  (IRELATIVE/COPY/ordering).
- Multi-session; each phase surfaces new libc gaps.
