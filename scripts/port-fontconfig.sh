#!/usr/bin/env bash
# ── MakaOS fontconfig port (cross-gcc + sysroot) ──────────────────────
#
# Font discovery library.  Depends on freetype + libexpat (both ported
# earlier).  Minimal build — we don't wire in the system font cache
# directory as something dynamic; clients pass explicit font paths or
# rely on the sysroot-baked config.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FC_VERSION="2.15.0"
FC_URL="https://gitlab.freedesktop.org/fontconfig/fontconfig/-/archive/${FC_VERSION}/fontconfig-${FC_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
FC_SRC="$THIRD_PARTY/fontconfig-${FC_VERSION}"
FC_TARBALL="$THIRD_PARTY/fontconfig-${FC_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

CROSS_CC="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-gcc"
CROSS_AR="$REPO_ROOT/toolchain/bin/x86_64-pc-makaos-ar"
if [ ! -x "$CROSS_CC" ]; then
    echo "[port-fc] FATAL: cross-toolchain missing" >&2
    exit 1
fi

CONFIG_HEADER="$REPO_ROOT/scripts/configs/fontconfig_config.h"

CFLAGS=(
    -O2 -fPIC
    -Wall
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
    -Wno-implicit-fallthrough
    -Wno-sign-compare
    -Wno-pointer-sign
    -DHAVE_CONFIG_H
    -DFC_GPERF_SIZE_T=size_t
    -I "$SYSROOT/usr/include/freetype2"
)

log() { printf '[port-fc] %s\n' "$*" >&2; }

fetch_fc() {
    mkdir -p "$THIRD_PARTY"
    if [ ! -f "$FC_TARBALL" ]; then
        log "downloading fontconfig ${FC_VERSION}"
        curl -fSL --retry 3 -o "$FC_TARBALL" "$FC_URL"
    fi
    if [ ! -d "$FC_SRC" ]; then
        log "extracting fontconfig"
        tar -xzf "$FC_TARBALL" -C "$THIRD_PARTY"
    fi

    # ── Generate fcstdint.h (upstream meson does this via configure_file;
    # our raw-gcc path skipped it, which worked once because the build
    # dir still had a stale copy around).  Content is the same as
    # src/fcstdint.h.in — just a thin forward to <stdint.h>.
    if [ ! -f "$FC_SRC/src/fcstdint.h" ]; then
        cp "$FC_SRC/src/fcstdint.h.in" "$FC_SRC/src/fcstdint.h"
    fi

    # ── Generate fc-lang/fclang.h via upstream fc-lang.py.
    # Consumed by src/fclang.c; encodes the .orth charset mappings into
    # a C table.  Without this the build hits "fclang.h: No such file".
    if [ ! -f "$FC_SRC/fc-lang/fclang.h" ]; then
        ( cd "$FC_SRC/fc-lang" && \
          python3 ./fc-lang.py *.orth --template fclang.tmpl.h \
            --output fclang.h --directory . > /dev/null 2>&1 )
    fi

    # ── Generate fc-case/fccase.h via upstream fc-case.py.
    # Consumed by src/fcstr.c (case folding tables).
    if [ ! -f "$FC_SRC/fc-case/fccase.h" ]; then
        ( cd "$FC_SRC/fc-case" && \
          python3 ./fc-case.py CaseFolding.txt --template fccase.tmpl.h \
            --output fccase.h > /dev/null 2>&1 )
    fi

    # ── Patch: guard FamilyTableAdd against NULL family strings.
    # FcValueString returns NULL when the FcValue is non-string;
    # FamilyTableAdd then hands NULL to FcHashTableFind which derefs it
    # in FcStrHashIgnoreCase and segfaults.  Skip the NULL entry.
    if ! grep -q MAKAOS_FAMILY_NULL_GUARD "$FC_SRC/src/fccfg.c"; then
        log "patching fccfg.c: NULL-guard in FamilyTableAdd"
        python3 - <<PY
path = "$FC_SRC/src/fccfg.c"
needle = "const FcChar8 *s = FcValueString (&ll->value);\n            FamilyTableEntry *fe;\n\n            if (!FcHashTableFind (table->family_hash,"
repl   = "const FcChar8 *s = FcValueString (&ll->value);\n            FamilyTableEntry *fe;\n\n            /* MAKAOS_FAMILY_NULL_GUARD */\n            if (!s) continue;\n\n            if (!FcHashTableFind (table->family_hash,"
with open(path) as h: src = h.read()
if needle not in src:
    raise SystemExit("fontconfig FamilyTableAdd shape changed; update patch")
with open(path, "w") as h: h.write(src.replace(needle, repl, 1))
print("patched")
PY
    fi
}

install_headers() {
    log "installing headers into $SYSROOT/usr/include/fontconfig"
    mkdir -p "$SYSROOT/usr/include/fontconfig"
    cp "$FC_SRC/fontconfig/"*.h "$SYSROOT/usr/include/fontconfig/"
}

build_lib() {
    local build_objs="$BUILD_DIR/fc_objs"
    mkdir -p "$build_objs"

    cp "$CONFIG_HEADER" "$FC_SRC/config.h"

    # Generate fcalias.h/fcaliastail.h + fcftalias.h/fcftaliastail.h
    # via the makealias shell script (normally invoked from Makefile.am
    # / meson.build).  Lists of public symbols come from the public
    # headers; the script rewrites declarations with hidden visibility.
    if [ ! -f "$FC_SRC/src/fcalias.h" ]; then
        (cd "$FC_SRC/src" && \
         sh ./makealias "$FC_SRC/src" fcalias.h fcaliastail.h \
            fontconfig.h fcprivate.h fcdeprecate.h)
    fi
    # makealias's fcftalias.h path is broken when the freetype public
    # header has no FcFT*-prefixed entry points (which is our case —
    # modern fontconfig moved them elsewhere).  It generates an
    # unmatched fcftaliastail.h and no fcftalias.h.  Hidden-visibility
    # aliasing is purely an optimization; empty both files so the
    # #include directives become no-ops.
    : > "$FC_SRC/src/fcftalias.h"
    : > "$FC_SRC/src/fcftaliastail.h"

    # fcobjshash.h is normally gperf-generated.  We don't ship gperf, so
    # synthesize a replacement that maps object-name strings → enum IDs
    # via linear scan of a compile-time table (driven by fcobjs.h).
    # Lookup is O(n=55) on the non-hot config-parse path; unknown names
    # fall through to fontconfig's "other types" path that allocates
    # on demand.  When we ship gperf, swap back to the perfect hash.
    if [ ! -f "$FC_SRC/src/fcobjshash.h" ]; then
        log "generating fcobjshash.h (linear scan, derived from fcobjs.h)"
        {
            cat <<'HEADER'
/* MakaOS port: linear-scan replacement for the gperf-generated
 * fcobjshash.h.  Source of truth: fcobjs.h's FC_OBJECT entries.
 * Matches fontconfig.h's property-string convention (strip
 * underscores, lowercase) — rare exceptions (FC_DESKTOP_NAME →
 * "desktop") degrade to the "other type" path, still correct. */
#include <string.h>
struct FcObjectTypeInfo { const char* name; int id; };
static const struct FcObjectTypeInfo fc_object_table[] = {
HEADER
            awk '
                /^FC_OBJECT[[:space:]]*\(/ {
                    s = $0
                    sub(/^FC_OBJECT[[:space:]]*\([[:space:]]*/, "", s)
                    n = s
                    sub(/[[:space:]]*,.*$/, "", n)
                    lower = tolower(n)
                    gsub(/_/, "", lower)
                    printf "    { \"%s\", FC_%s_OBJECT },\n", lower, n
                }' "$FC_SRC/src/fcobjs.h"
            cat <<'FOOTER'
};
static unsigned int
FcObjectTypeHash(register const char* str, register FC_GPERF_SIZE_T len)
{ (void)str; (void)len; return 0; }
static const struct FcObjectTypeInfo*
FcObjectTypeLookup(register const char* str, register FC_GPERF_SIZE_T len) {
    size_t n = sizeof(fc_object_table) / sizeof(fc_object_table[0]);
    for (size_t i = 0; i < n; i++) {
        const char* name = fc_object_table[i].name;
        if (strncmp(str, name, len) == 0 && name[len] == 0)
            return &fc_object_table[i];
    }
    return 0;
}
FOOTER
        } > "$FC_SRC/src/fcobjshash.h"
    fi

    local includes=(
        -I "$FC_SRC"
        -I "$FC_SRC/src"
        -I "$FC_SRC/fontconfig"
    )

    # Fontconfig sources — full set including fccache + fcatomic, which
    # use utimes/flock/timercmp (now supplied by libc).
    local srcs=(
        "src/fccache.c"
        "src/fcatomic.c"
        "src/fccfg.c"
        "src/fccharset.c"
        "src/fccompat.c"
        "src/fcdbg.c"
        "src/fcdefault.c"
        "src/fcdir.c"
        "src/fcformat.c"
        "src/fcfreetype.c"
        "src/fcfs.c"
        "src/fcptrlist.c"
        "src/fchash.c"
        "src/fcinit.c"
        "src/fclang.c"
        "src/fclist.c"
        "src/fcmatch.c"
        "src/fcmatrix.c"
        "src/fcname.c"
        "src/fcobjs.c"
        "src/fcpat.c"
        "src/fcrange.c"
        "src/fcserialize.c"
        "src/fcstat.c"
        "src/fcstr.c"
        "src/fcweight.c"
        "src/fcxml.c"
        "src/ftglue.c"
    )

    log "compiling ${#srcs[@]} fontconfig sources with $CROSS_CC"
    local objs=()
    for name in "${srcs[@]}"; do
        local src="$FC_SRC/$name"
        [ -f "$src" ] || { log "missing $name (skip)"; continue; }
        local obj="$build_objs/$(basename "$name" .c).o"
        objs+=("$obj")
        if [ "$src" -nt "$obj" ]; then
            "$CROSS_CC" "${CFLAGS[@]}" "${includes[@]}" -c "$src" -o "$obj"
        fi
    done

    local lib="$SYSROOT/usr/lib/libfontconfig.a"
    rm -f "$lib"
    "$CROSS_AR" rcs "$lib" "${objs[@]}"
    log "done — $(stat -c%s "$lib") bytes → $lib"
}

main() {
    if [ "${FORCE:-0}" = "1" ]; then
        log "FORCE=1 — wiping prior build"
        rm -rf "$BUILD_DIR/fc_objs" "$SYSROOT/usr/lib/libfontconfig.a"
    fi
    fetch_fc
    install_headers
    build_lib
}

main "$@"
