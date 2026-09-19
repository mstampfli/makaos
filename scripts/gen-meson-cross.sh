#!/usr/bin/env bash
# ── gen-meson-cross.sh ─────────────────────────────────────────────────
#
# Renders scripts/makaos-meson-cross.ini.in into build/makaos-meson-cross.ini
# with @REPO_ROOT@ substituted for the absolute repo path.  Port scripts
# that use meson source this as their first step, so the cross-file is
# always fresh and never hardcodes a machine-specific path into git.
#
# Idempotent.  Safe to run from any working directory.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMPLATE="$REPO_ROOT/scripts/makaos-meson-cross.ini.in"
OUT="$REPO_ROOT/build/makaos-meson-cross.ini"

[ -f "$TEMPLATE" ] || { echo "[gen-meson-cross] FATAL: $TEMPLATE missing" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
sed "s|@REPO_ROOT@|$REPO_ROOT|g" "$TEMPLATE" > "$OUT"
echo "$OUT"
