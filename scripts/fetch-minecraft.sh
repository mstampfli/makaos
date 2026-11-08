#!/usr/bin/env bash
# fetch-minecraft.sh - download a Minecraft client + its libraries, natives and
# assets from Mojang's public endpoints and lay them out for staging into the
# MakaOS ext2 image.
#
#   scripts/fetch-minecraft.sh [VERSION]     # default 1.20.4 (Java 17, LWJGL 3.3.2)
#
# Layout produced under build/mc/<version>/:
#   client.jar                  the game
#   libraries/*.jar             every non-native dependency jar (linux ruleset)
#   natives/*.so                extracted LWJGL native .so's (glfw substituted later)
#   assets/indexes/<id>.json    asset index
#   assets/objects/<2>/<hash>   asset objects (textures, font, sounds, ...)
#   classpath.txt               client.jar:libraries/... (MakaOS /mc paths)
#   launch.txt                  main class + asset index id
#
# Mojang's jars/assets are public (only online login needs auth); this fetches
# the same files the vanilla launcher does.
set -euo pipefail

VERSION="${1:-1.20.4}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MC="$REPO_ROOT/build/mc"
OUT="$MC/$VERSION"
mkdir -p "$OUT/libraries" "$OUT/natives" "$OUT/assets/indexes" "$OUT/assets/objects"

log() { echo "[fetch-mc] $*"; }
J="$MC/manifest.json"
[ -f "$J" ] || curl -fsS -o "$J" https://launchermeta.mojang.com/mc/game/version_manifest_v2.json

VJSON="$OUT/version.json"
if [ ! -f "$VJSON" ]; then
    url=$(jq -r --arg v "$VERSION" '.versions[] | select(.id==$v) | .url' "$J")
    [ -n "$url" ] || { echo "version $VERSION not in manifest"; exit 1; }
    curl -fsS -o "$VJSON" "$url"
fi
log "version $VERSION  main=$(jq -r .mainClass "$VJSON")  java=$(jq -r .javaVersion.majorVersion "$VJSON")"

# ── client.jar ───────────────────────────────────────────────────────────────
if [ ! -f "$OUT/client.jar" ]; then
    log "client.jar"
    curl -fsS -o "$OUT/client.jar" "$(jq -r .downloads.client.url "$VJSON")"
fi

# ── libraries + natives (apply the linux ruleset) ────────────────────────────
# Emit, per library: TYPE<TAB>URL<TAB>PATH  where TYPE is jar|native.
python3 - "$VJSON" > "$OUT/dl.tsv" <<'PY'
import sys, json
v = json.load(open(sys.argv[1]))
def allowed(rules):
    if not rules: return True
    action = "disallow"
    for r in rules:
        os = r.get("os", {})
        name = os.get("name")
        match = (name is None) or (name == "linux")
        if match: action = r["action"]
    return action == "allow"
for lib in v["libraries"]:
    if not allowed(lib.get("rules")): continue
    dl = lib.get("downloads", {})
    art = dl.get("artifact")
    if art and art.get("url"):
        print("jar\t%s\t%s" % (art["url"], art["path"].split("/")[-1]))
    # classifier natives (older LWJGL layout) + modern natives-linux artifacts
    classifiers = dl.get("classifiers") or {}
    nat = lib.get("natives", {}).get("linux")
    if nat and nat in classifiers and classifiers[nat].get("url"):
        c = classifiers[nat]
        print("native\t%s\t%s" % (c["url"], c["path"].split("/")[-1]))
    # 1.19+ list natives as a normal artifact whose name ends -natives-linux
    if art and art.get("url") and art["path"].endswith("natives-linux.jar"):
        print("native\t%s\t%s" % (art["url"], art["path"].split("/")[-1]))
PY

# download jars + native jars in parallel
log "downloading $(grep -c . "$OUT/dl.tsv") library artifacts"
while IFS=$'\t' read -r typ url name; do
    [ "$typ" = jar ] && { dest="$OUT/libraries/$name"; } || { dest="$OUT/natives/$name"; }
    [ -f "$dest" ] && continue
    echo "$url $dest"
done < "$OUT/dl.tsv" | xargs -P 8 -n 2 sh -c 'curl -fsS -o "$1" "$0" || echo "FAIL $0"'

# extract native .so's from the native jars, then drop the jars
log "extracting native .so's"
for nj in "$OUT/natives"/*.jar; do
    [ -f "$nj" ] || continue
    (cd "$OUT/natives" && unzip -o -q "$nj" '*.so' 2>/dev/null || true)
done
find "$OUT/natives" -mindepth 2 -name '*.so' -exec mv -t "$OUT/natives" {} + 2>/dev/null || true
rm -f "$OUT/natives"/*.jar
rmdir "$OUT/natives"/linux* 2>/dev/null || true

# JNA ships its native dispatch lib inside jna-*.jar (used by oshi for system
# info). Extract libjnidispatch.so so it can be staged +x and loaded via
# jna.boot.library.path -- MakaOS can't mmap PROT_EXEC a JNA-extracted temp file.
jna=$(ls "$OUT"/libraries/jna-*.jar 2>/dev/null | grep -v platform | head -1)
if [ -n "$jna" ]; then
    unzip -p "$jna" com/sun/jna/linux-x86-64/libjnidispatch.so > "$OUT/natives/libjnidispatch.so" 2>/dev/null \
        && log "extracted libjnidispatch.so" || rm -f "$OUT/natives/libjnidispatch.so"
fi

# ── asset index + objects ────────────────────────────────────────────────────
AIDX_ID=$(jq -r .assetIndex.id "$VJSON")
AIDX="$OUT/assets/indexes/$AIDX_ID.json"
if [ ! -f "$AIDX" ]; then
    log "asset index $AIDX_ID"
    curl -fsS -o "$AIDX" "$(jq -r .assetIndex.url "$VJSON")"
fi
log "downloading asset objects (this is the big one)"
jq -r '.objects[].hash' "$AIDX" | sort -u | while read -r h; do
    sub=${h:0:2}
    dest="$OUT/assets/objects/$sub/$h"
    [ -f "$dest" ] && continue
    mkdir -p "$OUT/assets/objects/$sub"
    echo "https://resources.download.minecraft.net/$sub/$h $dest"
done | xargs -P 16 -n 2 sh -c 'curl -fsS -o "$1" "$0" || echo "FAIL $0"' 2>/dev/null

# ── classpath + launch metadata (MakaOS /mc paths) ───────────────────────────
{
    printf '/mc/client.jar'
    for j in "$OUT"/libraries/*.jar; do printf ':/mc/libraries/%s' "$(basename "$j")"; done
    printf '\n'
} > "$OUT/classpath.txt"
{
    echo "main=$(jq -r .mainClass "$VJSON")"
    echo "assetIndex=$AIDX_ID"
    echo "version=$VERSION"
} > "$OUT/launch.txt"

log "done. natives:"; ls "$OUT/natives" | sed 's/^/    /'
log "objects: $(find "$OUT/assets/objects" -type f | wc -l)   size: $(du -sh "$OUT" | cut -f1)"
