#!/usr/bin/env bash
# ── MakaOS libjpeg (IJG v9e) port ─────────────────────────────────────
# JPEG decode/encode for Xonotic/DarkPlaces textures (PRELOAD links -ljpeg).
# Manual source-list cross-compile; jconfig.h provided (no ./configure).
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
SRC="$BUILD_DIR/third_party/jpeg-9e"
SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"
CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
log() { printf '[port-libjpeg] %s\n' "$*" >&2; }
[ -d "$SRC" ] || { log "FATAL: $SRC not found"; exit 1; }

# jconfig.h — POSIX/GCC resolution of the IJG feature template.
cat > "$SRC/jconfig.h" <<'EOF'
#define HAVE_PROTOTYPES
#define HAVE_UNSIGNED_CHAR
#define HAVE_UNSIGNED_SHORT
#define HAVE_STDDEF_H
#define HAVE_STDLIB_H
#define HAVE_LOCALE_H
#undef  NEED_BSD_STRINGS
#undef  NEED_SYS_TYPES_H
#undef  NEED_FAR_POINTERS
#undef  NEED_SHORT_EXTERNAL_NAMES
#undef  INCOMPLETE_TYPES_BROKEN
#ifdef  JPEG_INTERNALS
#undef  RIGHT_SHIFT_IS_UNSIGNED
#endif
#define INLINE __inline__
EOF

SRCS=(jaricom jcapimin jcapistd jcarith jccoefct jccolor jcdctmgr jchuff jcinit
      jcmainct jcmarker jcmaster jcomapi jcparam jcprepct jcsample jctrans
      jdapimin jdapistd jdarith jdatadst jdatasrc jdcoefct jdcolor jddctmgr
      jdhuff jdinput jdmainct jdmarker jdmaster jdmerge jdpostct jdsample
      jdtrans jerror jfdctflt jfdctfst jfdctint jidctflt jidctfst jidctint
      jquant1 jquant2 jutils jmemmgr jmemnobs)

OBJDIR="$BUILD_DIR/jpeg_objs"; mkdir -p "$OBJDIR"
CFLAGS=(-O2 -fPIC -I "$SRC")
objs=()
for s in "${SRCS[@]}"; do
    o="$OBJDIR/$s.o"; objs+=("$o")
    "$CROSS_CC" "${CFLAGS[@]}" -c "$SRC/$s.c" -o "$o"
done
rm -f "$SYSROOT/usr/lib/libjpeg.a"
"$CROSS_AR" rcs "$SYSROOT/usr/lib/libjpeg.a" "${objs[@]}"

cp "$SRC/jpeglib.h" "$SRC/jconfig.h" "$SRC/jmorecfg.h" "$SRC/jerror.h" \
   "$SYSROOT/usr/include/"
cat > "$SYSROOT/usr/lib/pkgconfig/libjpeg.pc" <<EOF
prefix=/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libjpeg
Description: IJG JPEG library
Version: 9.5
Libs: -L\${libdir} -ljpeg
Cflags: -I\${includedir}
EOF
log "done -- $(stat -c%s "$SYSROOT/usr/lib/libjpeg.a") bytes -> libjpeg.a"
