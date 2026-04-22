#!/usr/bin/env bash
# ── MakaOS tllist port ────────────────────────────────────────────────
#
# tllist is a single-header, type-safe intrusive linked list used by
# foot and fcft.  No build step — just install the header and the
# pkg-config stub so meson resolves tllist as a dep.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TL_VERSION="1.1.0"
TL_URL="https://codeberg.org/dnkl/tllist/archive/${TL_VERSION}.tar.gz"

BUILD_DIR="$REPO_ROOT/build"
THIRD_PARTY="$BUILD_DIR/third_party"
SRC_DIR="$THIRD_PARTY/tllist"
TARBALL="$THIRD_PARTY/tllist-${TL_VERSION}.tar.gz"

SYSROOT="${SYSROOT:-$BUILD_DIR/sysroot}"

log() { printf '[port-tllist] %s\n' "$*" >&2; }

fetch() {
    mkdir -p "$THIRD_PARTY"
    [ -f "$TARBALL" ] || {
        log "downloading tllist ${TL_VERSION}"
        curl -fsSL -o "$TARBALL" "$TL_URL"
    }
    [ -d "$SRC_DIR" ] || {
        log "extracting"
        tar -xzf "$TARBALL" -C "$THIRD_PARTY"
    }
}

install_headers() {
    /usr/bin/install -Dm644 "$SRC_DIR/tllist.h" "$SYSROOT/usr/include/tllist.h"

    cat > "$SYSROOT/usr/lib/pkgconfig/tllist.pc" <<EOF
prefix=/usr
includedir=\${prefix}/include
Name: tllist
Description: typesafe linked list implementation (single header)
Version: ${TL_VERSION}
Cflags: -I\${includedir}
EOF
    log "done — tllist.h + tllist.pc installed"
}

main() { fetch; install_headers; }
main "$@"
