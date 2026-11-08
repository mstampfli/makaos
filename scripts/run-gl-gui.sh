#!/usr/bin/env bash
# Boot MakaOS under QEMU with the virtio-gpu 3D (virgl) device in a VISIBLE,
# GL-accelerated window -- the windowed counterpart of run-gl.sh (which is
# headless via egl-headless).  Use this to WATCH the GLES2/virgl compositor
# (tinywl / sway) run and interact with it (keyboard + absolute mouse).
#
#   AUTOLOGIN=1 AUTOLOGIN_SPEC=root:/bin/tinywl NO_QEMU=1 bash build.sh
#   bash scripts/run-gl-gui.sh
#
# HOST GL CONTEXT (the tricky bit on hybrid-NVIDIA laptops): QEMU's virtio-gpu-gl
# needs a host OpenGL >= 3.0 context.  On a hybrid laptop the default GL provider
# is the Intel iGPU / a software context, which can fail ("Unable to create
# OpenGL context >= 3.0" -> "virgl could not be initialized").  The standard fix
# is NVIDIA PRIME render offload: point the GL context at the NVIDIA GPU (GL 4.x)
# via __NV_PRIME_RENDER_OFFLOAD / __GLX_VENDOR_LIBRARY_NAME.  virtio-gpu is the
# ONLY display device (no -vga std), so the window shows the compositor head
# directly -- no Ctrl+Alt+<n> head switching.
#
# If it still fails, try (one at a time) -- and tell me which works:
#   GLPROVIDER=intel bash scripts/run-gl-gui.sh                 # use the Intel iGPU instead
#   DISP='sdl,gl=on' bash scripts/run-gl-gui.sh                 # SDL instead of GTK
#   DISP='gtk,gl=es' bash scripts/run-gl-gui.sh                 # request GLES, not desktop GL
#   GDK_BACKEND=wayland SDL_VIDEODRIVER=wayland bash scripts/run-gl-gui.sh   # native Wayland
# Serial (kernel log) -> build/serial-gl.txt.  Quit: close the window or
# `pkill qemu-system-x86` from another terminal.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Which host GPU provides QEMU's OpenGL context.  DEFAULT = nvidia: this desktop
# renders on the RTX 5060 (Blackwell), and open-source mesa (nouveau/iris) cannot
# drive Blackwell at all -- only the proprietary NVIDIA stack (GL 4.6 / EGL /
# Vulkan) can, so it is the only provider that yields a working window here.
# GLPROVIDER=intel keeps GL entirely on the Intel iGPU via mesa (safe, but the
# window's GL must reach the iGPU -- works when the Intel iGPU drives the display,
# e.g. BIOS Hybrid mode, or via DRI_PRIME reverse-PRIME if the compositor imports).
case "${GLPROVIDER:-nvidia}" in
  intel)
    export __EGL_VENDOR_LIBRARY_FILENAMES="${__EGL_VENDOR_LIBRARY_FILENAMES:-/usr/share/glvnd/egl_vendor.d/50_mesa.json}"
    export __GLX_VENDOR_LIBRARY_NAME="${__GLX_VENDOR_LIBRARY_NAME:-mesa}"
    # Pin mesa to the Intel iris driver so it never probes the NVIDIA card and
    # tries to load nouveau_dri.so (which fails hard when the proprietary NVIDIA
    # driver is loaded: "glx: failed to create dri3 screen").
    export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-iris}"
    # On a hybrid laptop whose desktop renders on the NVIDIA GPU (e.g. this
    # RTX 5060 Blackwell, which open-source mesa CANNOT drive), the QEMU window's
    # GL context would land on that GPU and mesa dies.  Force QEMU to RENDER on
    # the Intel i915 render node (safe, well supported) via DRI_PRIME; the
    # Wayland compositor imports the resulting dma-buf (reverse-PRIME).  This
    # keeps every GL call off the NVIDIA driver -- no Blackwell GL, no freeze.
    if [ -z "${DRI_PRIME:-}" ]; then
      for _rn in /sys/class/drm/renderD*; do
        [ "$(basename "$(readlink -f "$_rn/device/driver")" 2>/dev/null)" = i915 ] || continue
        _pci="$(basename "$(readlink -f "$_rn/device")")"   # e.g. 0000:00:02.0
        export DRI_PRIME="pci-${_pci//[:.]/_}"              # -> pci-0000_00_02_0
        break
      done
    fi ;;
  nvidia)
    export __NV_PRIME_RENDER_OFFLOAD="${__NV_PRIME_RENDER_OFFLOAD:-1}"
    export __GLX_VENDOR_LIBRARY_NAME="${__GLX_VENDOR_LIBRARY_NAME:-nvidia}"
    export __EGL_VENDOR_LIBRARY_FILENAMES="${__EGL_VENDOR_LIBRARY_FILENAMES:-/usr/share/glvnd/egl_vendor.d/10_nvidia.json}" ;;
  *) : ;;   # leave the GL vendor env untouched
esac

SERIAL="${SERIAL:-build/serial-gl.txt}"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS="build/OVMF_VARS_gl.fd"
cp "/usr/share/OVMF/OVMF_VARS_4M.fd" "$OVMF_VARS"

# SDL opens a window on this host (GTK hit eglMakeCurrent errors); override with
# DISP=... (see header).
DISPLAY_OPT="${DISP:-sdl,gl=on}"

# In a Wayland session, force the toolkit onto its NATIVE Wayland backend so the
# window's GL context comes from the session's own EGL (the NVIDIA EGL vendor
# selected above) instead of falling back to Xwayland + GLX + DRI3, which probes
# GPUs via mesa and dies on nouveau/iris.  Override by exporting SDL_VIDEODRIVER /
# GDK_BACKEND yourself (e.g. SDL_VIDEODRIVER=x11 for the classic GLX path).
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
  export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}"
  export GDK_BACKEND="${GDK_BACKEND:-wayland}"
fi

pkill -9 -f "qemu-system-x86_64.*disk\.img" 2>/dev/null || true

echo "[run-gl-gui] virtio-gpu-gl + $DISPLAY_OPT  (GLPROVIDER=${GLPROVIDER:-nvidia}, host GL EGL=${__EGL_VENDOR_LIBRARY_FILENAMES:-default})"
echo "[run-gl-gui] serial -> $SERIAL"
exec qemu-system-x86_64 \
  -accel kvm -cpu host -smp 4 -m 1024M -nodefaults -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive format=raw,file=build/disk.img,if=none,id=hd0,snapshot=on \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -device virtio-gpu-gl-pci,id=vgpu \
  -display "$DISPLAY_OPT" \
  -device virtio-tablet-pci \
  -audiodev pa,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -serial file:"$SERIAL" \
  -no-reboot -no-shutdown \
  "$@"
