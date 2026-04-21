#!/usr/bin/env bash
# ── Generate pkg-config .pc files for sysroot libs ────────────────────
#
# Every port in this repo ships .a + headers into sysroot.  This helper
# writes the matching .pc descriptors so cross-builds (meson,
# autotools) can resolve `dependency('foo')` via pkg-config.
#
# Re-runnable — overwrites existing files.  Keep in sync with new ports
# as they land.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSROOT="${SYSROOT:-$REPO_ROOT/build/sysroot}"
PC_DIR="$SYSROOT/usr/lib/pkgconfig"
mkdir -p "$PC_DIR"

write_pc() {
    local name="$1" desc="$2" version="$3" libs="$4" cflags="${5:-}"
    local fname="$PC_DIR/${name}.pc"
    {
        printf 'prefix=/usr\n'
        printf 'libdir=${prefix}/lib\n'
        printf 'includedir=${prefix}/include\n'
        printf '\n'
        printf 'Name: %s\n'        "$name"
        printf 'Description: %s\n' "$desc"
        printf 'Version: %s\n'     "$version"
        printf 'Libs: -L${libdir} %s\n' "$libs"
        printf 'Cflags: -I${includedir} %s\n' "$cflags"
    } > "$fname"
    echo "[pc] wrote $fname"
}

# ── Tier 2 ports ──────────────────────────────────────────────────
write_pc zlib           "zlib compression library"        "1.3.1"    "-lz"
write_pc expat          "expat XML parser"                "2.6.4"    "-lexpat"
write_pc libffi         "foreign function interface"      "3.4.6"    "-lffi"
write_pc pixman-1       "pixman image library"            "0.43.4"   "-lpixman-1"               "-I\${includedir}/pixman-1"
write_pc xkbcommon      "xkbcommon keyboard layouts"      "1.7.0"    "-lxkbcommon"
write_pc freetype2      "freetype font rasterizer"        "2.13.3"   "-lfreetype -lz"           "-I\${includedir}/freetype2"
write_pc harfbuzz       "harfbuzz text shaping"           "8.5.0"    "-lharfbuzz"
write_pc fontconfig     "fontconfig font discovery"       "2.15.0"   "-lfontconfig -lfreetype -lexpat"
write_pc libdrm         "direct rendering manager"        "2.4.125"  "-ldrm"                    "-I\${includedir}/libdrm"

# ── Tier 4 (wayland) ──────────────────────────────────────────────
write_pc wayland-client "Wayland client library"          "1.23.1"   "-lwayland-client -lffi"
write_pc wayland-server "Wayland server library"          "1.23.1"   "-lwayland-server -lffi"

# The wayland-scanner .pc describes the host-tool.  wlroots' meson
# queries it for the "wayland_scanner" variable so it can run the
# tool during generation steps.
cat > "$PC_DIR/wayland-scanner.pc" <<'PC'
prefix=/usr
bindir=${prefix}/bin

Name: wayland-scanner
Description: Wayland protocol scanner (host tool, symlinked into sysroot)
Version: 1.23.1
wayland_scanner=${bindir}/wayland-scanner
PC
echo "[pc] wrote $PC_DIR/wayland-scanner.pc"

# ── Tier 5 deps (already self-generated but covered here too) ─────
# libseat.pc and libudev.pc are written by their own port scripts —
# overwrite them here to guarantee shape consistency.
write_pc libseat        "MakaOS libseat stub (always-grant)" "0.9.1" "-lseat"
write_pc libudev        "MakaOS native libudev equivalent"   "255"   "-ludev"

echo "[pc] all done"
