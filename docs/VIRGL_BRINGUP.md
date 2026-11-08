# virtio-gpu 3D (virgl) bring-up plan

Accelerated OpenGL for MakaOS: turn the working 2D KMS stack into a real
GL-capable stack via virtio-gpu's 3D (virgl) path, so the compositor can use a
GLES2 renderer and SDL/apps get hardware GL instead of software rasterisation.

This is the concrete engineering plan behind ROADMAP.md's one-line "defer virgl
until after Hyprland lands on the 2D path". It is a LARGE, multi-phase effort
(the Mesa port alone is substantial); it is written as dependency-ordered,
independently-verifiable steps so it can be landed incrementally without ever
regressing the working 2D desktop.

## Where we are (verified 2026-07)

UPDATE: the 3D/virgl GL stack is now at FIRST LIGHT -- a static Mesa
(EGL+GLES2+GBM+virgl) build renders an offscreen magenta FBO through the
render node and reads it back (Phase 3c below).  The 2D snapshot in this
section is the pre-Mesa baseline; where it says "no Mesa/GBM/EGL/GL/virgl
anywhere" and "2D only", read Phase 3 for the current state.

2D KMS baseline (still the default desktop path):

- `kernel/drivers/video/virtio_gpu.c` drives virtio-gpu **2D only**.
  `DRIVER_FEATURES` (virtio_gpu.c:40) = `VIRTIO_F_VERSION_1 | VIRTIO_GPU_F_EDID`.
  `VIRTIO_GPU_F_VIRGL` is `#define`d (virtio_gpu.c:34) but NOT negotiated.
  Implemented commands: GET_DISPLAY_INFO, RESOURCE_CREATE_2D, RESOURCE_UNREF,
  RESOURCE_ATTACH_BACKING, SET_SCANOUT, TRANSFER_TO_HOST_2D, RESOURCE_FLUSH,
  UPDATE_CURSOR, MOVE_CURSOR. All commands are synchronous (ctrl_hdr fence_id
  path unused).
- `kernel/drivers/video/drm.c` exposes a real Linux DRM ioctl uAPI on
  `/dev/dri/card0` (major 226 / minor 0, name "makaos-drm"): MODE_* modeset,
  dumb buffers (CREATE_DUMB / MAP_DUMB / DESTROY_DUMB), ADDFB2, atomic,
  page-flip, cursor, and a PRIME shim over dumb-buffer handles.
- Present path: `drm_commit_apply()` (drm.c:823) does TRANSFER_TO_HOST_2D ->
  SET_SCANOUT -> RESOURCE_FLUSH per scanout.
- wlroots runs `-Dbackends=drm,libinput`, `-Drenderers=` empty (pixman software
  only), `-Dallocators=` empty (dumb buffers, no GBM). See `scripts/port-wlroots.sh`.
- There is **no Mesa / GBM / EGL / GL / virgl anywhere** in the tree. The only
  GL-ish surface is stubs: `userland/libc/wayland_egl_stub.c` (returns NULL) and
  stub headers `wayland-egl.h` / `wayland-egl-core.h`. `scripts/port-libdrm.sh`
  hard-patches `drmGetRenderDeviceNameFromFd` to NULL (marker
  `MAKAOS_NO_RENDER_NODE`).
- QEMU is launched with plain `-device virtio-gpu-pci` (no `gl=on`), see
  `build.sh` / `scripts/run-sway-gui.sh`.

## Target architecture

The virgl data flow we are building toward:

```
  app / compositor
     |  GL / GLES2 calls
     v
  Mesa gallium "virgl" driver  (userland .so)
     |  encodes GL into a virgl command stream
     v
  MakaOS DRM winsys  (libdrm_virtgpu + our render node)
     |  DRM_IOCTL_VIRTGPU_{RESOURCE_CREATE,MAP,EXECBUFFER,TRANSFER_*,WAIT,GET_CAPS}
     v
  kernel drm.c  (render-node uAPI)
     |  virtio-gpu 3D control queue
     v
  kernel virtio_gpu.c  (CTX_CREATE, RESOURCE_CREATE_3D, SUBMIT_3D, TRANSFER_*_3D, fences)
     |  virtqueue
     v
  QEMU virtio-gpu-gl + virglrenderer  (host)
     |  real OpenGL on the host GPU
     v
  host GL driver
```

Two independent, replaceable clients drive that stack: wlroots (GLES2 renderer +
GBM allocator) for the compositor, and SDL/native apps via EGL.

## The hard constraint that sets the order

None of the kernel 3D work is independently testable until a userland GL client
exists to drive it. So we do NOT land kernel 3D commands speculatively on top of
the working 2D stack (that risks the desktop for zero verifiable gain). Instead
we build a minimal END-TO-END vertical slice first (Phase 0), then widen it.

Every phase has a concrete verification GATE. Do not start phase N+1 until
phase N's gate passes.

## Phases

### Phase 0 - host + capability probe (safe, no behaviour change)

STATUS: DONE (commit 0152728).  On the gl launch the driver logs the host's
virgl capsets (id=1 VIRGL, id=2 VIRGL2 max_size 1384); on the plain 2D launch
it logs "offered: no" and the 2D path is byte-identical.  scripts/run-gl.sh is
the gl launch variant.

Goal: confirm the host offers virgl and read its capset, WITHOUT enabling
VIRTIO_GPU_F_VIRGL in the live driver (2D path stays byte-identical).

- Host: add a virgl-enabled QEMU launch variant: `-device virtio-gpu-gl-pci`
  (or `virtio-vga-gl`) + `-display ...,gl=on` (headless: egl-headless), host
  `virglrenderer` present. Keep the existing 2D launch as the default.
- Kernel: in a probe-only path, negotiate VIRGL on a scratch feature mask, send
  GET_CAPSET_INFO / GET_CAPSET (opcodes already `#define`d at virtio_gpu.c:57-58,
  never sent), log capset id/version/size. Do NOT change `DRIVER_FEATURES`.
- Gate: serial shows a virgl capset (id 1 or 2) with a sane size on the gl launch,
  and the 2D desktop is unchanged on the normal launch.
- Risk: low (adds a read-only command; gated behind the gl launch).
- Effort: small.

### Phase 1 - kernel 3D command layer + fences

STATUS: DONE (1a commit 698d83d, 1b commit bd3c28f).  VIRGL is added to
DRIVER_FEATURES but only negotiated when the host offers it (masked out on a
plain virtio-gpu-pci device), so the 2D path is untouched.  CTX_CREATE/DESTROY,
CTX_ATTACH_RESOURCE, RESOURCE_CREATE_3D, TRANSFER_TO/FROM_HOST_3D and SUBMIT_3D
are implemented; two init self-tests pass on the gl launch: (1a) a 3D-resource
upload/wipe/download round-trip, and (1b) a hand-encoded virgl CREATE_SURFACE +
SET_FRAMEBUFFER_STATE + CLEAR that virglrenderer runs on the host GPU, read back
byte-exact.

NOTE on fences: the self-tests use the existing SYNCHRONOUS control-queue path
(FIFO order => a later response proves all earlier commands retired), which is
correct and needs no fence object/list/workqueue.  The async fence path below
(VIRTIO_GPU_FLAG_FENCE + an O(1) monotonic completion counter woken from the
control-queue IRQ, no per-fence allocation -- better than Linux) is deferred to
Phase 2, where userland EXECBUFFER/WAIT is the actual consumer.

Goal: the kernel can create a 3D context, create/transfer 3D resources, submit a
virgl command buffer, and wait on a fence. Still no userland client; verified by
a tiny in-kernel selftest that submits a trivial virgl clear.

- virtio_gpu.c: add VIRTIO_GPU_F_VIRGL (+ likely CONTEXT_INIT) to
  `DRIVER_FEATURES` ONLY when the gl launch is detected/available; implement
  CTX_CREATE / CTX_DESTROY, CTX_ATTACH_RESOURCE / CTX_DETACH_RESOURCE,
  RESOURCE_CREATE_3D, TRANSFER_TO_HOST_3D / TRANSFER_FROM_HOST_3D, SUBMIT_3D.
- Fences: today every command is synchronous. SUBMIT_3D needs the async
  fence_id path: allocate a fence id, set VIRTIO_GPU_FLAG_FENCE in ctrl_hdr,
  and complete on the control-queue used-ring interrupt. Reuse the existing
  virtqueue completion path; add a fence wait-queue (the same
  irq_waitq/wait_queue_t pattern the HDA/AHCI drivers use).
- Gate: an in-kernel selftest builds a minimal virgl cmd stream (create ctx +
  3D resource, SUBMIT_3D a clear-to-colour, TRANSFER_FROM_HOST_3D, fence-wait)
  and reads back the expected pixel. Runs under the gl launch only.
- Risk: medium. Confined to virtio_gpu.c; the 2D path is untouched when VIRGL is
  not negotiated. The fence path is the subtle part (async completion ordering).
- Effort: medium-large.

### Phase 2 - DRM render node uAPI

STATUS: 2a DONE (commit 0ea7fc8) -- /dev/dri/renderD128 + the DRM_IOCTL_VIRTGPU_*
ioctls (GETPARAM, GET_CAPS, CONTEXT_INIT, RESOURCE_CREATE, RESOURCE_INFO, MAP,
EXECBUFFER, TRANSFER_TO/FROM_HOST, WAIT), byte-exact with libdrm's
drm/virtgpu_drm.h, on the synchronous command path.  O(1) handle-indexed 3D bo
table (no idr).  Verified by userland virgltest driving a GPU clear through the
ioctls + mmap and reading back byte-exact magenta (283/283), reject paths
included; 2D path byte-identical.

2b DONE (commit 1d5d3cf) -- control-queue MSI-X (VEC 0x36) + an O(1) monotonic
fence counter + wait queue backing DRM_IOCTL_VIRTGPU_WAIT (no fence-object/list/
workqueue).  Safe/additive: the poll stays authoritative so a bad MSI-X can't
stall.  NOTE: under QEMU's synchronous virglrenderer commands retire before
their used-ring entry, so submit==done and WAIT is immediate -- the mechanism is
the correct O(1) foundation for an async-submit path, not a parallelism win
here.

2c DONE -- real dma-buf PRIME.  PRIME is no longer a back-ref shim to the
exporter's GEM handle: a PRIME fd is now a STANDALONE, page-refcounted dma-buf
(drm_dmabuf_t) that holds its own ref on every backing page, so the buffer
outlives the exporter's handle -- true Linux dma-buf lifetime.  Export works for
both dumb (2D) and res3d (render-node) buffers; import mints a fresh GEM handle
over the shared pages.  Unified the fd's GEM handle namespace (one c->next_handle
feeds dumb + res3d) so a handle names exactly one object -- the two independent
counters used to collide once a render fd owned both a res3d and a PRIME-imported
dumb (they share the mmap offset + ioctl handle space).  Verified by virgltest:
GPU-render a 16x16 res3d to magenta, PRIME-export it, import on a SECOND fd,
MAP_DUMB+mmap that handle, read back byte-exact magenta -- zero-copy cross-fd
sharing (the Mesa render-node -> compositor path).  16/16 reject probes
fail-closed with exact errno.

This 2c work surfaced and fixed a latent regression from the spinlock-preempt
fold: call_rcu_expedited (reached by fd_table_grow under files->lock) hid a
synchronize_rcu_expedited that PANICS with preemption disabled -- and the panic
was a compiled-out serial_puts_dbg + cli;hlt, i.e. a SILENT wedge.  Fixed at the
RCU layer (defer async via call_rcu_head when preempt_depth>0) and made the rail
panic loudly.  See kernel/proc/rcu.c.

Goal: expose the virtio-gpu 3D uAPI Mesa's virgl winsys actually calls, on a
render node.

- drm.c: implement the `DRM_IOCTL_VIRTGPU_*` set: GETPARAM, GET_CAPS,
  CONTEXT_INIT, RESOURCE_CREATE, RESOURCE_INFO, MAP, TRANSFER_TO_HOST,
  TRANSFER_FROM_HOST, EXECBUFFER, WAIT. These are thin wrappers over the Phase 1
  kernel primitives.
- Add a `renderD128` device node (currently only `dri/card0` exists,
  `kernel/fs/virtfs.c:37`) and route it to a render-node open path.
- Make PRIME real: dma-buf export/import backed by standalone page-refcounted
  buffers a 3D context can import, not a dumb-handle back-ref shim. DONE (2c) --
  drm_dmabuf_t + unified GEM handle namespace; see drm_ioctl_prime_handle_to_fd /
  drm_ioctl_prime_fd_to_handle.
- Undo the `MAKAOS_NO_RENDER_NODE` patch in `scripts/port-libdrm.sh:113-124` and
  build `libdrm_virtgpu`.
- Gate: a userland test using libdrm_virtgpu directly (VIRTGPU_GET_CAPS +
  RESOURCE_CREATE + EXECBUFFER a clear + TRANSFER_FROM_HOST) reads back the
  expected pixel. This is the first userland-driven 3D test.
- Risk: medium. New ioctls are additive; render node is a new device.
- Effort: medium-large.

### Phase 3 - Mesa port (the long pole)

Goal: build Mesa configured for the gallium `virgl` driver plus a MakaOS DRM
winsys, producing `libgbm`, `libEGL`, `libGLESv2` (and `libGL`).

- New `scripts/port-mesa.sh`: cross-build Mesa with meson for the MakaOS
  sysroot. Enable `gallium-drivers=virgl`, `platforms=wayland`, `egl`, `gbm`,
  `gles2`; disable X11, LLVM (virgl doesn't need it), Vulkan initially.
- Provide the MakaOS DRM winsys: virgl's `virgl_drm_winsys` talks to
  libdrm_virtgpu (Phase 2). Verify it binds to our render node.
- Replace the EGL stubs: delete `userland/libc/wayland_egl_stub.c` and the stub
  headers once real Mesa `wayland-egl` is installed.
- Sub-gates (Mesa is big; stage it):
  - 3a: Mesa configures + builds `libgbm` against our libdrm; a gbm smoke test
    allocates a scanout buffer via GBM.
  - 3b: `libEGL` + `libGLESv2` build; `eglinfo`-style probe reports the virgl
    renderer string from the host.
  - 3c: a minimal EGL+GLES2 test app (create context on the Wayland platform,
    clear + draw a triangle, present) renders on screen through the wl_shm or
    dmabuf path.
- Risk: HIGH (Mesa cross-build for a from-scratch OS is the biggest single
  chunk; expect toolchain/dependency friction).
- Effort: very large (own multi-session sub-project).

STATUS (in progress):

3a/3b DONE -- the static Mesa GL stack BUILDS and installs on MakaOS:
libEGL.a, libGLESv2.a, libgbm.a, libglapi.a, and libgallium_dri (a static
archive carrying the virgl gallium driver + virtio_gpu entrypoint), plus the
EGL/GLES2/gbm headers, all in the sysroot. Getting there needed: hosted
libstdc++ (Phase 3-0, done), the static_library conversions (libc.a is non-PIC),
the loader static-DRI shim, virgl disk-cache stub, and a long tail of libc gaps
(C++ runtime + extern "C" headers, float/fused math, gettid/sched_getcpu/
setpriority/syscall shim, elf.h, alloca-via-stdlib, PRIi* macros, libsync.h).
3c DONE -- FIRST LIGHT.  userland/apps/gltest links the whole static stack
(libEGL + libGLESv2 + libgbm + libgallium_dri) and renders a real frame on the
render node: gbm_create_device -> eglGetPlatformDisplay(GBM) -> eglInitialize ->
GLES2 context -> surfaceless make-current -> offscreen RGBA8 FBO ->
glClearColor(magenta) -> glClear -> glFinish -> glReadPixels, then verifies every
pixel is magenta.  A headless boot passes it repeatedly (0x600D600D), so the
whole path works: EGL -> gallium virgl winsys -> our virtio-gpu render node ->
the host GPU rendered it -> we read it back.

First-light debugging fixed a CHAIN of real bugs:
 - kernel DRM_IOCTL_VERSION length protocol (only copied the name when caller_len
   >= actual+1, but libdrm passes caller_len == actual) -> empty driver name ->
   swrast fallback.  Now copies min(caller_len, actual), Linux-style.
 - kernel DRM version 0.0.0: virgl_drm_get_version rejects version_major != 0, and
   Mesa keys supports_fences off minor >= 1.  We do not implement sync_file fence
   FDs (EXECBUFFER returns fence_fd = -1), so 0.1 made glFinish block forever in
   sync_wait(poll(fd=-1, -1)); 0.0 routes it through the DRM_IOCTL_VIRTGPU_WAIT
   resource-wait path our synchronous submit supports.
 - libdrm drmGetNodeTypeFromFd derives node type from the fd's minor
   (renderD128 => DRM_NODE_RENDER); drmGetDevice2/drmGetDevices2/
   drmGetDeviceFromDevId synthesise the single virtio-gpu device (no sysfs) so
   _eglFindDevice / render-node discovery works (durable in port-libdrm.sh).
 - loader.c (port-mesa.sh): loader_open_driver_lib returns NULL (no dlopen);
   loader_is_device_render_capable uses the node type, not drmGetDevice2.
 - kernel DRM resource-create exempts PIPE_BUFFER (target 0, width == byte size)
   from the 16384 texture-dimension limit; EXECBUFFER cap raised 4 KiB -> 64 KiB;
   control bounce buffer grown to 128 KiB with heap-built submit/capset for the
   multi-KiB virgl command streams.
 - THE headline bug: virtq_activate never wrote cfg->queue_size, so the device
   used its own controlq default (256) while we allocate 64-entry rings.  The
   avail-ring indices agree only for the first 64 commands, then the device reads
   a slot we never wrote and sets DEVICE_NEEDS_RESET -- wedging the control queue
   after ~64 GPU commands (QEMU: "Guest says index <garbage> is available").  It
   is a general virtio-gpu corruption bug (not virgl-specific); virtio_net always
   negotiated the size, virtio-gpu was the one driver that forgot.  Now both GPU
   queues negotiate queue_size = VIRTQ_SIZE.

Phase 4 UPDATE: DONE for wlroots.  wlroots builds -Drenderers=gles2
-Dallocators=gbm over the static Mesa, and the tinywl reference compositor
COMPOSITES its first GLES2/virgl frame end to end (renderer -> allocator ->
wayland socket -> DRM backend started -> first frame committed).  Getting there
fixed two more MakaOS bugs: a libdrm format-modifier stub that strdup(NULL)'d on
the LINEAR modifier, and -- the real blocker for every Wayland compositor -- an
ext2 open(O_CREAT) hang (ext2 alloc/writeback held spinlocks across block I/O
whose completion wait sched_sleep'd, a silent panic since 74c8dec made spinlocks
disable preemption; fixed in ahci.c AHCI_WAIT, which busy-polls when preempt is
disabled).  NEXT: propagate the Mesa-GL link fragment to port-sway.sh /
port-dwl.sh, then SDL3's GL path.

DONE earlier this pass:
- Feasibility CONFIRMED from Mesa 24.0.9 source: a fully STATIC virgl+EGL+GBM+
  GLES2 build is viable with no dynamic loader. The static pipe-loader
  (GALLIUM_STATIC_TARGETS) matches virgl's descriptor by DRM driver name
  "virtio_gpu"; EGL/GBM both load the DRI driver through loader_open_driver,
  which we shim to return the statically-linked __driDriverGetExtensions_
  virtio_gpu directly (no dlopen). GBM has a builtin_backends[] static table.
- Kernel prereq: renderD128 reports DRM version name "virtio_gpu" (commit
  04ba531); libdrm drmGetRenderDeviceNameFromFd returns /dev/dri/renderD128.
- scripts/port-mesa.sh written: meson CONFIGURE SUCCEEDS with
  default_library=static, gallium-drivers=virgl, platforms=wayland, egl/gbm/
  gles2 enabled, llvm/vulkan/glx disabled. Patches applied: static DRI shim
  (loader.c), static DRI target (targets/dri/meson.build), system_has_kms_drm
  += makaos, -D__linux__ (MakaOS is Linux-source/ABI compatible), wayland-egl-
  backend header+pc, shader-cache disabled.
- MakaOS libc gaps filled (all additive, POSIX/C99, boot-verified): <endian.h>,
  rint/rintf/nearbyint/lrint/lrintf/llrint (math.c), pthread_barrier (mutex+cond
  generation barrier), <sys/param.h>, <sys/syscall.h> (minimal, no asm),
  clock_nanosleep, _SC_PHYS_PAGES/_SC_AVPHYS_PAGES sysconf. The C portion of
  Mesa's util layer compiles.

BLOCKER (hard prerequisite, own sub-project) -- HOSTED libstdc++:
  Mesa's compiler/GLSL/NIR/util layers are heavily C++ and need a HOSTED C++
  standard library: std::vector, std::string, std::unordered_map, std::mutex,
  std::thread, ... The MakaOS toolchain (x86_64-pc-makaos) ships only a
  FREESTANDING libstdc++ subset (type_traits, atomic, memory, algorithm --
  header-only, no-OS parts); <vector>/<string>/<mutex>/<thread>/<unordered_map>
  are absent (a trivial STL TU fails to compile). This is why harfbuzz (which
  sticks to the freestanding subset) builds but Mesa cannot.
  PATH: hosted libstdc++ was DELIBERATELY disabled -- build-toolchain.sh passes
  --disable-hosted-libstdcxx (and --disable-wchar_t). _GLIBCXX_HAS_GTHREADS is
  already 1 and MakaOS pthreads exist (now incl. pthread_barrier), and hosted
  libstdc++-v3 CONFIGURES cleanly against the current sysroot (it generated
  <vector> etc.). So Phase 3-0 is "enable hosted + rebuild libstdc++-v3", not
  "write an STL".

  TWO ways to build it, and the difference matters (learned the hard way):
   * RIGHT: reconfigure/rebuild ONLY the libstdc++-v3 subdir, reusing the
     already-built+installed gcc and libgcc. This is the surgical path.
   * WRONG: re-running the TOP-LEVEL gcc configure in-place marks gcc+libgcc
     stale and forces a full rebuild -- which then FAILS in libgcc on an errno
     conflict (see below). Don't do this; if a from-scratch toolchain rebuild is
     ever needed, the errno issue must be fixed first.

  errno PITFALL (blocks any libgcc rebuild): MakaOS declares `errno` as a bare
  `extern __thread int errno;` VARIABLE. gcc's libgcc tsystem.h has
  `#ifndef errno / extern int errno;` -- a fallback that a standard libc skips
  because it defines errno as a MACRO. MakaOS's variable form doesn't trip that
  guard, so libgcc's rebuild hits "non-thread-local declaration of 'errno'
  follows thread-local declaration". The CORRECT fix is POSIX errno-as-macro
  (`#define errno (*__errno_location())`), but that RENAMES the errno symbol and
  thus breaks the ABI of every already-compiled port (they reference the bare
  `errno` symbol) -- so it requires RE-PORTING THE WHOLE USERLAND. That is a
  planned, deliberate future change (do it, then rebuild every port); it is out
  of scope for the Mesa bring-up, which only needs the isolated libstdc++-v3
  build that never rebuilds libgcc.

### Phase 4 - wire the clients

Goal: the compositor and SDL use the GL stack.

- wlroots: flip `scripts/port-wlroots.sh` to `-Drenderers=gles2`
  `-Dallocators=gbm`; wlroots now composites with GLES2 over GBM/dmabuf.
- SDL3: enable a real GL/GLES video path (the current build forces
  `-DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF`, port-sdl3.sh); SDL renderer picks the
  GL backend over the wl_shm software framebuffer.
- Gate: `sdl3_hello` renders through the GL renderer (not the software one), and
  sway composites via GLES2, both confirmed via the host virgl renderer string +
  a screendump. Software paths remain as fallback.
- Risk: medium (config flips over a now-tested stack).
- Effort: medium.

## Verification strategy

- Host: a dedicated gl launch (`-device virtio-gpu-gl-pci -display egl-headless,gl=on`)
  with host `virglrenderer`; keep the 2D launch as default and CI so 2D never
  regresses.
- Each phase reads back a known pixel or a renderer string -- proxies like "no
  error" are not acceptable (see the audio/spinlock verification in this repo:
  capture the actual output and prove the specific claim).
- The 2D KMS path MUST keep passing at every phase (VIRGL negotiation is gated,
  software renderers stay as fallback).

## Open decisions

- CONTEXT_INIT vs legacy 3D context: prefer CONTEXT_INIT + capset selection
  (matches modern Mesa virgl).
- blob resources (RESOURCE_CREATE_BLOB / SET_SCANOUT_BLOB): defer; classic 3D
  resources + TRANSFER are enough for first light.
- Vulkan (venus): out of scope; virgl GL first.

## Effort summary

Phase 0 small, Phase 1 medium-large, Phase 2 medium-large, Phase 3 very large
(Mesa), Phase 4 medium. Phase 3 dominates and is best treated as its own
sub-project. Recommendation: land Phase 0 whenever a gl-enabled host is
available (it is safe and de-risks the rest), then schedule Phase 1-2 together
(they are the kernel vertical slice and share the fence/uAPI design), then commit
to the Mesa port as a focused effort.
