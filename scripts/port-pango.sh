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
    # ── Patch 1: FreeType-backed shaping, sized correctly (pangofc-font.c) ──
    # Native HarfBuzz OT cmap returns glyph 0 (.notdef) for every codepoint in
    # this port, so pango text rendered as tofu.  Route glyph lookup through
    # FreeType's cmap.  CRITICAL: attach the FT funcs AFTER hb_font_set_scale /
    # hb_font_set_ptem -- hb_ft_font_set_funcs sizes the backing FT_Face from
    # the hb font's CURRENT scale, so attaching before the scale is set leaves
    # the FT_Face at its default size and glyphs+advances come out microscopic
    # and cramped.
    local f="$PANGO_SRC/pango/pangofc-font.c"
    if grep -q "attach FreeType cmap funcs AFTER" "$f" 2>/dev/null; then
        log "pangofc-font.c already patched"
    else
        python3 - "$f" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("#include <hb-ot.h>\n",
              "#include <hb-ot.h>\n#include <hb-ft.h>   /* MakaOS: shape via FreeType cmap, not HB native OT */\n", 1)
needle = "  hb_font_set_ptem (hb_font, point_size);\n"
ins = ("  hb_font_set_ptem (hb_font, point_size);\n"
       "  /* MakaOS: attach FreeType cmap funcs AFTER the scale/ptem are set so\n"
       "   * hb_ft sizes the backing FT_Face to the correct pixel size (native HB\n"
       "   * OT cmap returns glyph 0 = tofu; attaching before the scale leaves the\n"
       "   * FT_Face at its default size -> tiny, cramped text). */\n"
       "  hb_ft_font_set_funcs (hb_font);\n")
assert needle in s, "port-pango: hb_font_set_ptem anchor not found"
s = s.replace(needle, ins, 1)
open(p, 'w').write(s)
PY
        log "patched pangofc-font.c → FreeType-backed shaping (scale-first)"
    fi

    # ── Patch 3: default font resolution to 96 dpi (pangofc-fontmap.c) ─────
    # The PangoCairoFc fontmap's get_resolution vfunc returns an unset (-1)
    # resolution in this port, so point sizes (e.g. "DejaVu Sans Mono 11")
    # convert to pixels via a negative/garbage dpi and render microscopic.
    # Fall back to the standard 96 dpi when the vfunc reports unset.
    local m="$PANGO_SRC/pango/pangofc-fontmap.c"
    if grep -q "Fall back to the standard 96 dpi" "$m" 2>/dev/null; then
        log "pangofc-fontmap.c already patched"
    else
        python3 - "$m" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
needle = ("  if (PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap)->get_resolution)\n"
          "    return PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap)->get_resolution (fcfontmap, context);\n")
ins = ("  if (PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap)->get_resolution)\n"
       "    {\n"
       "      double r = PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap)->get_resolution (fcfontmap, context);\n"
       "      /* MakaOS: the PangoCairoFc fontmap returns an unset (-1) resolution,\n"
       "       * so point sizes scale by a negative/garbage dpi -> microscopic text.\n"
       "       * Fall back to the standard 96 dpi when unset. */\n"
       "      if (r <= 0.0)\n"
       "        r = 96.0;\n"
       "      return r;\n"
       "    }\n")
assert needle in s, "port-pango: get_resolution anchor not found"
s = s.replace(needle, ins, 1)
open(p, 'w').write(s)
PY
        log "patched pangofc-fontmap.c → default resolution 96 dpi"
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
