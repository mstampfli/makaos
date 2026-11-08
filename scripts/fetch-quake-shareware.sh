#!/usr/bin/env bash
# Fetch the freely-redistributable Quake shareware pak0.pak (id Software, 1996)
# and stage it at build/quake-id1/pak0.pak.  build.sh installs it into the image
# at /root/id1/pak0.pak so DarkPlaces (DarkPlaces-Quake mode) has data to render.
# The 18 MB pak is NOT committed -- it is fetched on demand.  Skips if present.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="build/quake-id1"
PAK="$OUT/pak0.pak"
WORK="build/quake-fetch"

if [ -f "$PAK" ] && [ "$(stat -c%s "$PAK")" -gt 18000000 ]; then
    echo "[quake] pak0.pak already staged ($PAK) -- skip"; exit 0
fi
command -v 7z  >/dev/null || { echo "FATAL: need 7z (p7zip) to unpack resource.1"; exit 1; }
command -v unzip >/dev/null || { echo "FATAL: need unzip"; exit 1; }

mkdir -p "$WORK" "$OUT"
# quake106.zip (id shareware 1.06) -> resource.1 (LZH) -> ID1/PAK0.PAK.
URL="https://raw.githubusercontent.com/Jason2Brownlee/QuakeOfficialArchive/main/bin/quake106.zip"
echo "[quake] downloading quake106.zip"
curl -fsSL -o "$WORK/quake106.zip" "$URL"
head -c2 "$WORK/quake106.zip" | grep -q PK || { echo "FATAL: quake106.zip not a zip"; exit 1; }
( cd "$WORK" && unzip -oq quake106.zip resource.1 && 7z x -y resource.1 >/dev/null )
SRC="$(find "$WORK" -iname 'pak0.pak' | head -1)"
[ -n "$SRC" ] || { echo "FATAL: pak0.pak not found in resource.1"; exit 1; }
head -c4 "$SRC" | grep -q PACK || { echo "FATAL: pak0.pak missing PACK magic"; exit 1; }
cp "$SRC" "$PAK"
rm -rf "$WORK"
echo "[quake] staged $PAK ($(stat -c%s "$PAK") bytes)"
