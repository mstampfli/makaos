#!/usr/bin/env bash
# ── MakaOS hwdata stub ─────────────────────────────────────────────────
#
# hwdata is a data-only package shipping PNP IDs, PCI IDs, USB IDs,
# OUI lists, etc.  wlroots and libdisplay-info only want pnp.ids to
# resolve monitor manufacturer prefixes (3 letters → company name).
#
# We install the host's pnp.ids file into $SYSROOT/usr/share/hwdata/
# and write an hwdata.pc so consumers find it.  No binary, no libs.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSROOT="${SYSROOT:-$REPO_ROOT/build/sysroot}"
PNP_HOST="/usr/share/hwdata/pnp.ids"

log() { printf '[port-hwdata] %s\n' "$*" >&2; }

if [ ! -f "$PNP_HOST" ]; then
    log "host pnp.ids missing at $PNP_HOST — install hwdata package first"
    log "trying minimal synthesized pnp.ids"
    mkdir -p "$SYSROOT/usr/share/hwdata"
    cat > "$SYSROOT/usr/share/hwdata/pnp.ids" <<'EOF'
VRT	Virtio
MAK	MakaOS
EOF
else
    mkdir -p "$SYSROOT/usr/share/hwdata"
    cp "$PNP_HOST" "$SYSROOT/usr/share/hwdata/pnp.ids"
    log "copied $PNP_HOST → sysroot"
fi

mkdir -p "$SYSROOT/usr/lib/pkgconfig"
cat > "$SYSROOT/usr/lib/pkgconfig/hwdata.pc" <<EOF
prefix=/usr
datadir=\${prefix}/share
pkgdatadir=\${datadir}/hwdata

Name: hwdata
Description: Hardware ID data files (PNP IDs)
Version: 0.380
EOF
log "installed hwdata.pc"
log "done"
