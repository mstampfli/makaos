#!/usr/bin/env bash
# Link the real libSDL3.so: SDL3 (whole-archive so all of SDL is present) + the
# Mesa GL frontend + wayland stack, as ONE ET_DYN shared object.  libc, libstdc++,
# libm/rt/pthread/dl are left UNDEFINED on purpose -- the dlopen loader binds them
# against the PIE executable's exported .dynsym at load time (the exe-scope model).
set -euo pipefail
cd "$(dirname "$0")/.."
LD="$(pwd)/toolchain/bin/x86_64-pc-makaos-ld"
SYSROOT="build/sysroot"
L="$SYSROOT/usr/lib"
OUT="${1:-build/libSDL3.so}"

"$LD" -shared -m elf_x86_64_makaos --build-id=none -soname libSDL3.so.0 \
    --gc-sections --allow-multiple-definition \
    -L "$L" \
    build/dso_handle.o \
    --whole-archive "$L/libSDL3.a" --no-whole-archive \
    --start-group \
      "$L/dri/virtio_gpu_dri.so" \
      -lEGL -lGLESv2 -lGLESv1_CM -lgbm -lglapi \
      -lwayland-egl -lwayland-client -lwayland-cursor -lwayland-server \
      -lxkbcommon -lffi -lexpat -ldrm -lz \
      "$(dirname "$0")/../toolchain/x86_64-pc-makaos/lib/libstdc++.a" \
      "$(dirname "$0")/../toolchain/x86_64-pc-makaos/lib/libsupc++.a" \
      "$(dirname "$0")/../toolchain/lib/gcc/x86_64-pc-makaos/14.2.0/libgcc.a" \
    --end-group \
    -o "$OUT"
echo "linked $OUT"
