# MakaOS Port Roadmap — path to Hyprland + Ladybird

End state: a MakaOS install boots directly into **Hyprland** (wayland
compositor, wlroots-based), runs modern Wayland apps via **SDL3**, has
**curl** for network tools, and runs a full browser — **Ladybird** —
with real JavaScript.

The ordering below is the dependency DAG.  Each tier must be complete
before the next begins; within a tier, ports are independent.

---

## Tier 1 — libc expansions + C++ toolchain

| Item | Notes |
|------|-------|
| pthread (mutex/cond/thread/key/tls) | needed by wayland server, libstdc++, everything |
| epoll / poll / timerfd / eventfd / signalfd | round out what exists; wayland event loop |
| socketpair + SCM_RIGHTS fd passing | wayland IPC |
| mmap MAP_SHARED + POSIX shm polish | wl_shm buffer sharing |
| dlopen / dlsym | went further than planned: a real PIE + dlopen loader (DT_NEEDED, dynamic TLS, real `libSDL3.so`); see docs/DYNLINKER_PLAN.md |
| setlocale / strcoll / nl_langinfo stubs | ICU, curl, libxml2 |
| C++ toolchain rebuild (--enable-languages=c,c++ + libstdc++) | unlocks every C++ port |

## Tier 2 — foundation libraries

All installed into `$SYSROOT/usr/{lib,include}` via a `scripts/port-FOO.sh`.

- **libffi** — wayland message dispatch, many runtimes
- **zlib** — curl, http/2, fonts, PNGs, pretty much everything
- **libexpat** — XML parsing (fontconfig, wayland-scanner, many)
- **pixman** — 2D software compositing (wlroots renderer)
- **libxkbcommon** — keyboard layouts (wayland)
- **freetype** — TrueType/OpenType rasterisation
- **harfbuzz** — complex-script shaping (needs freetype)
- **fontconfig** — font discovery (needs freetype + expat)

## Tier 2.5 — kernel DRM/KMS subsystem on virtio-gpu

**Strategy:** implement a **virtio-gpu** driver as the kernel's first
real DRM device, then expose the Linux-compatible DRM/KMS uAPI on top
of it.  virtio-gpu is a standard PCI virtio device emulated by QEMU
(`-device virtio-gpu-pci`), has a clean, fully-public spec, supports
real modesetting + cursor + page-flip + hardware scanout via host-
resident resources, and optionally 3D/virgl for GL acceleration.  It
replaces the flat GOP framebuffer we currently scanout to — the GOP
path stays as a fallback for pre-virtio-gpu consoles.

### Tier 2.5a — virtio-gpu driver
`kernel/drivers/video/virtio_gpu.c`
- PCI enumeration + virtio common (virtqueue setup, feature negotiation,
  MSI-X or legacy IRQ).
- Control queue: `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`,
  `SET_SCANOUT`, `TRANSFER_TO_HOST_2D`, `RESOURCE_FLUSH`,
  `GET_DISPLAY_INFO`, `RESOURCE_UNREF`, `UPDATE_CURSOR`, `MOVE_CURSOR`.
- Cursor queue (secondary virtqueue, same format as control).
- Multi-scanout support (virtio-gpu reports N displays — one per
  connector in the DRM wrapper).
- 3D: originally deferred until after Hyprland; done anyway in July
  2026 (kernel 3D commands + fences, DRM render node, static Mesa/
  GBM/EGL, wlroots and SDL3 on GLES2/virgl). Status and plan in
  docs/VIRGL_BRINGUP.md.

### Tier 2.5b — DRM/KMS uAPI layer
`kernel/drivers/video/drm.c` + `/dev/dri/card0` chardev
- Implements the Linux DRM ioctl surface libdrm expects:
  `GET_CAP`, `MODE_GETRESOURCES`, `MODE_GETCONNECTOR`, `MODE_GETCRTC`,
  `MODE_GETENCODER`, `MODE_SETCRTC`, `MODE_ADDFB2`, `MODE_RMFB`,
  `MODE_PAGE_FLIP`, `MODE_CREATE_DUMB`, `MODE_MAP_DUMB`,
  `MODE_DESTROY_DUMB`, `SET_MASTER`, `DROP_MASTER`, `MODE_GETGAMMA`,
  `PRIME_HANDLE_TO_FD`, `PRIME_FD_TO_HANDLE`.
- Translates DRM concepts → virtio-gpu resources:
  - dumb buffer = `RESOURCE_CREATE_2D` + anonymous host-backing pages
  - ADDFB2 = handle wrapper (no upload until a SETCRTC references it)
  - SETCRTC / PAGE_FLIP = `SET_SCANOUT` + `TRANSFER_TO_HOST_2D` +
    `RESOURCE_FLUSH`
- PRIME fd pair connects dumb-buffer handles to dmabuf fds for
  wayland's `wl_shm` / `linux-dmabuf-v1` protocols.
- Real vblank IRQ: virtio-gpu signals resource-flush completion on
  the control queue — we use that as the vsync source instead of a
  timer hack.

### Why virtio-gpu first, not a bare-metal GPU
- We're a VM-first kernel; QEMU + virtio-gpu is the deployment
  target.  A physical-GPU driver is orders of magnitude more code
  and hardware-specific.
- The DRM uAPI surface is identical either way — Hyprland can't tell
  the difference between virtio-gpu and a real Intel iGPU as long
  as the ioctls return sane data.
- virtio-gpu gets us real page-flip, real vblank, real multi-monitor,
  and 3D-on-the-host (later) without any hardware reverse-engineering.

## Tier 3 — DRM userland + input

- **libdrm** — upstream, ioctl wrappers.  Ports cleanly once 2.5 lands.
- **libudev-zero** — 300-line libudev replacement; hardcodes our GPU
  + evdev devices.
- **libinput** — reads `/dev/input/event*` (we already emit Linux-
  compatible `struct input_event`), enumerates via libudev-zero.

## Tier 4 — Wayland core

- **wayland** (libwayland-server, libwayland-client, wayland-scanner)
- **wayland-protocols** (XML protocol descriptions)

## Tier 5 — wlroots

Compositor library.  Configures with the pixman software renderer
(no GPU).  Depends on 2.5 + 3 + 4.  (Since July 2026 it also renders
through the GLES2/GBM renderer on virgl.)

## Tier 6 — Hyprland

The end-state compositor.  Its own build system (cmake-ish + Meson).
Brings hyprlang (config parser).

## Tier 7 — app-side

- **SDL3** with Wayland backend → any SDL3 app runs.
- **BSD pledge/unveil wrappers** in libc for ported OpenBSD apps.
- **curl** + libcurl (needs zlib + mbedtls ✓ + libpsl + nghttp2).
  Replaces `http_get`.

## Tier 8 — browser

**Ladybird** (SerenityOS origin, now standalone).  C++23, LibJS engine
(own ES2023), LibWeb (own DOM+CSS+HTML).  ~500 KLOC.

Extra deps it pulls in:
- libpng, libjpeg-turbo, libwebp, libavif
- brotli
- ICU (unicode segmentation/collation)
- Skia (2D — large) OR substitute pixman path
- Qt6 shell (big) OR their LibGUI shell (small, fits us better)

---

**Current progress (checked against main, 2026-11-08):**
- Tier 0 (toolchain + sysroot): done. Cross-gcc `x86_64-pc-makaos-gcc`
  (C and C++, hosted libstdc++), libc with split standard headers,
  mbedTLS ported. `http_get` speaks HTTPS, but there is no CA bundle:
  certificate verification only passes in its explicit insecure mode
  (`MBEDTLS_SSL_VERIFY_NONE`), so it is not a trustworthy TLS client.
- Tier 1: done, except OpenBSD-style `pledge()`/`unveil()` libc wrappers
  (see Tier 7). Locale is C / C.UTF-8 only.
- Tier 2: done (all eight have `scripts/port-*.sh`).
- Tier 2.5: done. virtio-gpu 2D and 3D (virgl), DRM/KMS uAPI with
  atomic modeset, hardware cursor, PRIME and a render node.
- Tiers 3, 4, 5: done (libdrm, libudev-zero, libinput, wayland,
  wayland-protocols, wlroots 0.18.1).
- Tier 6: Hyprland NOT ported. The desktop runs **sway** 1.10.1 on
  wlroots instead (docs/DESKTOP.md).
- Tier 7: SDL3 with the Wayland backend done (plus sdl2-compat, GLFW).
  libc pledge/unveil wrappers not done (pledge/unveil exist only as
  `spawn()` attributes). curl not ported.
- Tier 8 (Ladybird): not started.
- Off-roadmap work since June 2026: a JVM stack (Avian, OpenJDK 17 Zero,
  LWJGL; docs/JVM_PORT_PLAN.md) with glibc-ABI compatibility shims for
  prebuilt `.so` files, and DarkPlaces/Quake (docs/XONOTIC_PORT.md).
