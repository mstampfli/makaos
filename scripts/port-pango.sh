#!/usr/bin/env bash
# ── MakaOS pango port ──────────────────────────────────────────────────
#
# Text layout/shaping over glib + harfbuzz + fontconfig + freetype +
# fribidi + cairo.  sway renders every titlebar / swaybar / swaynag
# string through pangocairo.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PANGO_VERSION="1.54.0"
PANGO_URL="https://download.gnome.org/sources/pango/1.54/pango-${PANGO_VERSION}.tar.xz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
PANGO_SRC="$THIRD_PARTY/pango-${PANGO_VERSION}"
PANGO_TARBALL="$THIRD_PARTY/pango-${PANGO_VERSION}.tar.xz"
PANGO_BUILD="$BUILD_DIR/pango_build"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

log() { printf '[port-pango] %s\n' "$*" >&2; }

export PKG_CONFIG_PATH="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$PANGO_TARBALL" ] || {
        log "downloading pango ${PANGO_VERSION}"
        curl -fSL --retry 3 -o "$PANGO_TARBALL" "$PANGO_URL"
    }
    [ -d "$PANGO_SRC" ] || {
        log "extracting pango"
        tar -xJf "$PANGO_TARBALL" -C "$THIRD_PARTY"
    }
}

build() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$PANGO_BUILD"
    fi
    if [ ! -d "$PANGO_BUILD" ]; then
        log "meson setup"
        meson setup "$PANGO_BUILD" "$PANGO_SRC" \
            --cross-file "$REPO_ROOT/scripts/makaos-meson-cross.ini" \
            --prefix=/usr \
            --libdir=lib \
            -Ddefault_library=static \
            -Dintrospection=disabled \
            -Ddocumentation=false \
            -Dbuild-testsuite=false \
            -Dbuild-examples=false \
            -Dfontconfig=enabled \
            -Dfreetype=enabled \
            -Dcairo=enabled \
            -Dxft=disabled \
            -Dlibthai=disabled \
            2>&1 | tee "$BUILD_DIR/pango_meson.log" | tail -3
    fi
    log "ninja build"
    ninja -C "$PANGO_BUILD" 2>&1 | tail -3
    log "installing into $SYSROOT"
    DESTDIR="$SYSROOT" ninja -C "$PANGO_BUILD" install > /dev/null
    log "done — $(ls "$SYSROOT"/usr/lib/libpango-1.0.a)"
}

# MakaOS: pango shapes text via HarfBuzz's NATIVE OT cmap path, which
# returns glyph 0 (.notdef) for every codepoint in this port — so all
# pango-rendered text (swaybar/swaynag) came out as tofu boxes while
# foot/tofi (FreeType-backed hb_ft path) render perfectly.  Route pango's
# glyph lookup through FreeType's cmap (the proven-good path) by attaching
# hb_ft funcs to the shaping font.  Idempotent (marker-gated).
patch_pango() {
    # ── Patch 1: FreeType-backed shaping (pangofc-font.c) ─────────────────
    local f="$PANGO_SRC/pango/pangofc-font.c"
    if grep -q "shape via FreeType cmap" "$f" 2>/dev/null; then
        log "pangofc-font.c already patched"
    else
        python3 - "$f" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("#include <hb-ot.h>\n",
              "#include <hb-ot.h>\n#include <hb-ft.h>   /* MakaOS: shape via FreeType cmap, not HB native OT */\n", 1)
needle = "  hb_font = hb_font_create (hb_face);\n"
ins = ("  hb_font = hb_font_create (hb_face);\n"
       "  /* MakaOS: HarfBuzz native OT cmap returns glyph 0 for every codepoint\n"
       "   * in this port (all pango text rendered tofu); route glyph lookup\n"
       "   * through FreeType's cmap, the path foot/fcft prove works. */\n"
       "  hb_ft_font_set_funcs (hb_font);\n")
assert needle in s, "port-pango: hb_font_create anchor not found"
s = s.replace(needle, ins, 1)
open(p, 'w').write(s)
PY
        log "patched pangofc-font.c → FreeType-backed shaping"
    fi

    # ── Patch 2: initialize PangoLayoutRun offsets (taskbar height fix) ────
    # insert_run() does g_slice_new (no zeroing) and only sets ->item/->glyphs;
    # ->y_offset/->start_x_offset/->end_x_offset are otherwise set ONLY by
    # apply_baseline_shift(), which runs only for baseline-shifted text.  For
    # ordinary runs y_offset was read uninitialized (y_offset = run->y_offset),
    # so the line logical-extent height accumulated a junk value -> swaybar's
    # bar height blew up to ~10000px and the taskbar never mapped.  Latent
    # upstream (relies on malloc returning zeroed memory); exposed by MakaOS's
    # dirty free-list reuse.  Zero them at creation.
    local g="$PANGO_SRC/pango/pango-layout.c"
    if grep -q "MakaOS: zero PangoLayoutRun offsets" "$g" 2>/dev/null; then
        log "pango-layout.c already patched"
    else
        python3 - "$g" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
needle = "  PangoLayoutRun *run = g_slice_new (PangoLayoutRun);\n\n  run->item = run_item;\n"
ins = ("  PangoLayoutRun *run = g_slice_new (PangoLayoutRun);\n\n"
       "  run->item = run_item;\n"
       "  /* MakaOS: zero PangoLayoutRun offsets - g_slice_new does not zero and\n"
       "   * apply_baseline_shift() only sets these for baseline-shifted text, so\n"
       "   * ordinary runs otherwise read y_offset uninitialized and corrupt the\n"
       "   * line logical height (taskbar never mapped). */\n"
       "  run->y_offset = 0;\n"
       "  run->start_x_offset = 0;\n"
       "  run->end_x_offset = 0;\n")
assert needle in s, "port-pango: insert_run anchor not found"
s = s.replace(needle, ins, 1)
open(p, 'w').write(s)
PY
        log "patched pango-layout.c → zero PangoLayoutRun offsets (taskbar fix)"
    fi
}

main() {
    fetch
    patch_pango
    build
}
main "$@"
