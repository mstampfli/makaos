#!/usr/bin/env bash
# ── MakaOS libvorbis / vorbisfile / vorbisenc port ────────────────────
# Ogg Vorbis audio decode (+file demux, +enc) for Xonotic/DarkPlaces PRELOAD.
# Depends on libogg (port-libogg.sh).  Manual source-list cross-compile.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SRC="$BUILD_DIR/third_party/libvorbis-1.3.7"
SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
log() { printf '[port-libvorbis] %s\n' "$*" >&2; }
[ -d "$SRC" ] || { log "FATAL: $SRC not found"; exit 1; }
[ -f "$SYSROOT/usr/lib/libogg.a" ] || { log "FATAL: build libogg first"; exit 1; }

mkdir -p "$SYSROOT/usr/include/vorbis"
cp "$SRC/include/vorbis/codec.h"      "$SYSROOT/usr/include/vorbis/"
cp "$SRC/include/vorbis/vorbisfile.h" "$SYSROOT/usr/include/vorbis/"
cp "$SRC/include/vorbis/vorbisenc.h"  "$SYSROOT/usr/include/vorbis/"

OBJDIR="$BUILD_DIR/vorbis_objs"; mkdir -p "$OBJDIR"
CFLAGS=(-O2 -fPIC -ffast-math -I "$SYSROOT/usr/include" -I "$SRC/include" -I "$SRC/lib")

compile() {  # $1 = name (used as objdir subdir), rest = source basenames (in lib/)
    local grp="$1"; shift
    local objs=()
    for s in "$@"; do
        local o="$OBJDIR/${grp}_$s.o"; objs+=("$o")
        "$CROSS_CC" "${CFLAGS[@]}" -c "$SRC/lib/$s.c" -o "$o"
    done
    printf '%s\n' "${objs[@]}"
}

VORBIS_SRCS=(mdct smallft block envelope window lsp lpc analysis synthesis psy
             info floor1 floor0 res0 mapping0 registry codebook sharedbook
             lookup bitrate)
mapfile -t vobjs < <(compile vorbis "${VORBIS_SRCS[@]}")
rm -f "$SYSROOT/usr/lib/libvorbis.a"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libvorbis.a" "${vobjs[@]}"

mapfile -t vfobjs < <(compile vorbisfile vorbisfile)
rm -f "$SYSROOT/usr/lib/libvorbisfile.a"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libvorbisfile.a" "${vfobjs[@]}"

mapfile -t veobjs < <(compile vorbisenc vorbisenc)
rm -f "$SYSROOT/usr/lib/libvorbisenc.a"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libvorbisenc.a" "${veobjs[@]}"

for n in vorbis vorbisfile vorbisenc; do
    req="ogg"; [ "$n" = vorbis ] || req="vorbis"
    cat > "$SYSROOT/usr/lib/pkgconfig/$n.pc" <<EOF
prefix=/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: $n
Description: Vorbis audio ($n)
Version: 1.3.7
Requires.private: $req
Libs: -L\${libdir} -l$n
Cflags: -I\${includedir}
EOF
done
log "done -- libvorbis.a $(stat -c%s "$SYSROOT/usr/lib/libvorbis.a"), +file +enc"
