#!/usr/bin/env bash
# Build the DarkPlaces engine (Xonotic's engine) for MakaOS as a PIE that links
# sdl2-compat (libSDL2.a -> dlopen libSDL3.so.0 at runtime) + the codec libs.
# PRELOAD static mode: codecs linked directly (theora/curl/xmp stay runtime-
# dlopen'd, absent = those features just off).  GL resolves via
# SDL_GL_GetProcAddress -- no libGL link.  Client config: menu + video-capture
# struct (standard); libav/d0_blind_id/Windows TUs excluded.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$(pwd)"
SYSROOT="${SYSROOT:-$REPO/build/sysroot}"
DP="$REPO/build/third_party/darkplaces"
CC="$REPO/toolchain/bin/x86_64-pc-makaos-gcc"
LD="$REPO/toolchain/bin/x86_64-pc-makaos-ld"
B="$REPO/build/darkplaces_build"
L="$SYSROOT/usr/lib"

[ -d "$DP" ] || { echo "FATAL: darkplaces source missing at $DP"; exit 1; }
[ -f "$L/libSDL2.a" ] || { echo "FATAL: libSDL2.a missing (run port-sdl2-compat first)"; exit 1; }
mkdir -p "$B"

CFLAGS="--sysroot=$SYSROOT -m64 -mno-red-zone -msse2 -O2 -fPIE -fno-plt \
  -fno-stack-protector -fno-asynchronous-unwind-tables \
  -D_FILE_OFFSET_BITS=64 -D__KERNEL_STRICT_NAMES \
  -DCONFIG_MENU -DCONFIG_VIDEO_CAPTURE -DPREFER_PRELOAD -DUSE_SDL=2 \
  -isystem $SYSROOT/usr/include -isystem $SYSROOT/usr/include/SDL2 -I$DP -w"

# OBJ_SDL source list (OBJ_SND_XMP, av_backend_libav, sys_win excluded).
SRCS="builddate sys_sdl vid_sdl thread_sdl snd_sdl menu mvm_cmds \
snd_main snd_mem snd_mix snd_ogg snd_wav cap_ogg cap_avi \
bih crypto cd_shared cl_cmd cl_collision cl_demo cl_ents cl_ents4 cl_ents5 cl_ents_nq cl_ents_qw \
cl_input cl_main cl_parse cl_particles cl_screen cl_video cl_video_libavw clvm_cmds cmd collision \
com_crc16 com_ents com_ents4 com_game com_infostring com_msg common console csprogs curves cvar \
dpvsimpledecode filematch fractalnoise fs ft2 utf8lib gl_backend gl_draw gl_rmain gl_rsurf gl_textures hmac host \
image image_png jpeg keys lhnet libcurl mathlib matrixlib mdfour meshqueue \
mod_skeletal_animatevertices_sse mod_skeletal_animatevertices_generic model_alias model_brush \
model_shared model_sprite netconn palette phys polygon portals protocol prvm_cmds prvm_edict prvm_exec \
r_explosion r_lightning r_modules r_shadow r_sky r_sprites r_stats sbar sv_ccmds sv_demo sv_ents \
sv_ents4 sv_ents5 sv_ents_csqc sv_ents_nq sv_main sv_move sv_phys sv_save sv_send sv_user svbsp \
svvm_cmds sys_shared taskqueue vid_shared view wad world zone"

echo "[port-darkplaces] compiling $(echo $SRCS | wc -w) TUs"
OBJS=""
for b in $SRCS; do
    [ -f "$DP/$b.c" ] || { echo "  MISSING: $b.c"; exit 1; }
    "$CC" $CFLAGS -c "$DP/$b.c" -o "$B/$b.o"
    OBJS="$OBJS $B/$b.o"
done

echo "[port-darkplaces] linking darkplaces PIE"
# PIE: provides libc (+errno) to the dlopen'd libSDL3.so.0.  Codec libs +
# libSDL2 in a group (cross-refs); whole-archive libc so every libSDL3 import is
# in the exe dynsym; libgcc for compiler builtins.
"$LD" -m elf_x86_64_makaos -pie --export-dynamic -e _entry --build-id=none \
    -T "$REPO/userland/link-pie.ld" \
    "$L/crt0.o" $OBJS \
    --start-group \
      "$L/libSDL2.a" "$L/libpng16.a" "$L/libjpeg.a" \
      "$L/libvorbisfile.a" "$L/libvorbis.a" "$L/libogg.a" "$L/libz.a" \
    --end-group \
    --whole-archive "$L/libc.a" --no-whole-archive \
    "$REPO/toolchain/lib/gcc/x86_64-pc-makaos/14.2.0/libgcc.a" \
    -o "$B/darkplaces.elf"

echo "[port-darkplaces] linked $B/darkplaces.elf ($(stat -c%s "$B/darkplaces.elf") bytes)"
