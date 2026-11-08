# Xonotic (DarkPlaces engine) port -- status & plan

Goal: run Xonotic (open-source arena FPS, DarkPlaces engine) natively on MakaOS
through the SDL3 + Mesa/virgl desktop-GL stack.

## Verification honesty
This is a GRAPHICAL game. Automated verification here is HEADLESS ONLY (build
clean, launch in-guest, GL context created, assets load, frames rendered
offscreen without crashing, via `run-gl.sh` + serial). Whether it *looks* like
Xonotic is a human visual check via `run-gl-gui.sh` -- never claimed "verified
working" from a headless run alone. The windowed/NVIDIA path is never launched
from automation (it froze the host GPU before).

## Feasibility: GOOD (large but tractable)
- DarkPlaces has a **PRELOAD static build** (`LDFLAGS_UNIXSDL_PRELOAD`) that
  links codec libs directly instead of `dlopen` -- sidesteps MakaOS having no
  dynamic loader.
- DarkPlaces' portability surface is almost all standard POSIX MakaOS already
  supports (sockets, dirent, sys/mman, signal, pthreads). Only frictions:
  `dlopen` (1 file, off in PRELOAD), `backtrace`/`execinfo.h` (2 files, stub).
- SDL2->SDL3 bridged by **sdl2-compat** (2 TUs, builds over our SDL3).

## DONE (this pass)
- libogg           -> scripts/port-libogg.sh    (libogg.a)
- libvorbis(+file+enc) -> scripts/port-libvorbis.sh
- libjpeg (IJG 9e) -> scripts/port-libjpeg.sh   (libjpeg.a)
- (already had: zlib, libpng16, freetype, SDL3, GLESv2/EGL, Mesa/virgl)

## REMAINING (ordered)
1. **sdl2-compat** -- **DONE, UNMODIFIED** (the static-bind-patch idea below is
   obsolete: MakaOS now HAS a real dlopen loader, see docs/DYNLINKER_PLAN.md).
   sdl2-compat's native path -- `dlopen("libSDL3.so.0")` + a dlsym'd
   `SDL3_##fn` jump table -- just works.  Built via scripts/port-sdl2-compat.sh
   as `libSDL2.a`, pinned to `release-2.30.50` (the tag targeting our SDL3 3.2.0;
   newer sdl2-compat needs 3.2.4's integer_x/y wheel API).  Proven by sdl2test
   (CR2=0x5D20001F): SDL_Init loads SDL3 and the API round-trips, headless.
   The SDL2/SDL3 `SDL_*` clash is a non-issue here: the shim dlsyms SDL3 per
   libSDL3 handle (not global), libSDL3 imports no `SDL_*` from the exe, and the
   shim's `dlopen(NULL)` quirk-probe is `#ifdef __linux__` (skipped on MakaOS).
   DarkPlaces links `-lSDL2` (this libSDL2.a) and reaches SDL3 at runtime, no hack.
   [OBSOLETE ORIGINAL NOTE kept for context: "MakaOS has no dlopen, so this needs
   a static-symbol-bind patch..." -- superseded by the loader.]
2. **libGL desktop front-end** -- **N/A**: DarkPlaces resolves GL via
   SDL_GL_GetProcAddress (confirmed), no libGL link.
3. **Disk image bump**: chosen path is MINIMAL data first (image stays ~512 MiB-
   1 GiB, engine + a tiny data/); the ~2 GiB full-Xonotic bump comes later.
4. **Build DarkPlaces** -- **DONE** (scripts/port-darkplaces.sh): the 114-TU SDL
   client compiles + links as a PIE (4.6 MB, /bin/darkplaces).  NO dlopen patch
   needed (the loader exists; PRELOAD statically links codecs anyway).  Links
   sdl2-compat (libSDL2.a -> dlopen libSDL3.so.0) + z/jpeg/png/ogg/vorbis;
   theora/curl/xmp stay runtime-dlopen'd (absent = off); libav/d0_blind_id/
   Windows TUs excluded.  Its POSIX surface was already covered -- only THREE
   small libc additions were needed: memccpy, select (SYS_SELECT wrapper), the
   glibc __jmp_buf_tag tag; plus a real latent fix (the exe now self-applies its
   initial-exec TLS relocs so libc's errno works -- see DYNLINKER_PLAN.md).
5. **Assets** -- NEXT: no game data yet, so DarkPlaces exits at "Couldn't load
   gfx/mainmenu".  Minimal-data-first (user's call): a small data set to render
   real content + a visible frame.  For DarkPlaces-Quake (id1) that's the Quake
   shareware pak0.pak; for Xonotic proper, `-game xonotic` + a minimal Xonotic
   data subset.
6. **Headless launch** -- **GL CONTEXT ACHIEVED (2026-07-10)**.  dplaunch (sets
   SDL_VIDEODRIVER=offscreen + basedir) booted under scripts/run-gl.sh
   (virtio-gpu-gl, rendernode renderD129) brings up the FULL stack: DarkPlaces ->
   sdl2-compat -> dlopen(libSDL3.so.0) -> SDL3 -> Mesa/virgl -> the NVIDIA
   RTX 5060.  Serial: GL_RENDERER "virgl (NVIDIA GeForce RTX 5060)", GL_VERSION
   4.2 Core, GLSL 420, "OpenGL backend started", GL draw commands submitted (host
   vrend log shows them).  No crash; exits only on the missing gfx/mainmenu.
   OPEN: host vrend rejects a cube-map with array size 1 ("Illegal command
   buffer") -- a virgl compat quirk to chase once content renders.
   Then hand the windowed run (run-gl-gui.sh) to the user for the visual check.

## Risks / unknowns
- sdl2-compat may exercise SDL3 entry points our build stubbed.
- DarkPlaces GL path assumes desktop GL 2.x+/GLSL -- we have GL 3.3 core +
  compat, so OK, but shader-path quirks possible.
- Asset size vs image size; net/getaddrinfo for the master server (menu can run
  offline).
- This is multi-session work; each engine build surfaces new libc gaps.
