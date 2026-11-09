#!/usr/bin/env bash
# Copy files between the host and MakaOS's root file system inside
# build/disk.img, without rebuilding the image.
#
#   scripts/diskfiles.sh put <file|dir>... [--to /dir] [--user maka]
#   scripts/diskfiles.sh get <path in image> <host dir>
#   scripts/diskfiles.sh ls  [dir in image]
#
# put copies files and whole directories (default destination /root, owner
# root; --user maka makes uid/gid 1000).  Files keep their executable bit
# (0755, else 0644); directories are 0755.  An existing file is replaced.
#
# The root file system is the ext2 partition at sector 4096 of the GPT disk
# (EXT2_LBA in build.sh); debugfs works on it in place via "?offset=".
# Changes survive boots started by run.sh or scripts/boot.sh (they write to
# disk.img); scripts/run-gl-gui.sh boots with snapshot=on and discards writes;
# every build.sh run recreates the file system, so put files again afterwards.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMG="${DISK_IMG:-$REPO/build/disk.img}"
EXT2_OFFSET=$((4096 * 512))
FS="$IMG?offset=$EXT2_OFFSET"

die() { echo "diskfiles: $*" >&2; exit 1; }
usage() { sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }

command -v debugfs >/dev/null || die "debugfs missing (apt install e2fsprogs)"
[ -f "$IMG" ] || die "no image at $IMG (run build.sh first)"

# Two writers on one ext2 corrupt it: refuse while a QEMU has this image open
# for writing (a snapshot=on boot never writes it, so that one is fine).
check_not_in_use() {
    local pid args
    for pid in $(pgrep -x qemu-system-x86 2>/dev/null || true); do
        args="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
        case "$args" in
            *"$(basename "$IMG")"*snapshot=on*) ;;
            *"$(basename "$IMG")"*) die "QEMU (pid $pid) is using $IMG; shut MakaOS down first" ;;
        esac
    done
}

dfs() { debugfs -R "$1" "$FS" 2>&1; }   # read-only: ls, get

cmd="${1:-}"; shift || true
case "$cmd" in
put)
    dest=/root; uid=0; gid=0; srcs=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --to)   dest="${2:?--to needs a directory}"; shift 2 ;;
            --user) case "${2:-}" in
                        root) uid=0; gid=0 ;;
                        maka) uid=1000; gid=1000 ;;
                        *) die "--user must be root or maka" ;;
                    esac; shift 2 ;;
            -h|--help) usage 0 ;;
            *) srcs+=("$1"); shift ;;
        esac
    done
    [ ${#srcs[@]} -gt 0 ] || usage
    case "$dest" in /*) ;; *) die "--to needs an absolute path, e.g. /root" ;; esac
    dest="${dest%/}"; [ -n "$dest" ] || dest=/
    check_not_in_use
    script="$(mktemp)"; trap 'rm -f "$script"' EXIT
    # mkdir -p for the destination itself (debugfs mkdir fails harmlessly if present).
    p=""; IFS=/ read -ra parts <<< "${dest#/}"
    for part in "${parts[@]}"; do
        [ -n "$part" ] || continue
        p="$p/$part"; echo "mkdir $p" >> "$script"
    done
    emit_file() {   # $1 host file, $2 image path
        local mode=0100644; [ -x "$1" ] && mode=0100755
        printf 'rm %s\nwrite %s %s\nsif %s mode %s\nsif %s uid %s\nsif %s gid %s\n' \
            "$2" "$1" "$2" "$2" "$mode" "$2" "$uid" "$2" "$gid" >> "$script"
    }
    for s in "${srcs[@]}"; do
        [ -e "$s" ] || die "no such file: $s"
        case "$(basename "$s")" in *" "*) die "names with spaces are not supported: $s" ;; esac
        if [ -d "$s" ]; then
            base="$(cd "$s" && pwd)"; top="$dest/$(basename "$base")"; top="${top//\/\//\/}"
            echo "mkdir $top" >> "$script"
            printf 'sif %s mode 0040755\nsif %s uid %s\nsif %s gid %s\n' "$top" "$top" "$uid" "$top" "$gid" >> "$script"
            while IFS= read -r d; do
                printf 'mkdir %s\nsif %s mode 0040755\nsif %s uid %s\nsif %s gid %s\n' \
                    "$top/$d" "$top/$d" "$top/$d" "$uid" "$top/$d" "$gid" >> "$script"
            done < <(cd "$base" && find . -mindepth 1 -type d | sed 's|^\./||' | LC_ALL=C sort)
            while IFS= read -r f; do
                emit_file "$base/$f" "$top/$f"
            done < <(cd "$base" && find . -type f | sed 's|^\./||' | LC_ALL=C sort)
        else
            emit_file "$(cd "$(dirname "$s")" && pwd)/$(basename "$s")" "$dest/$(basename "$s")"
        fi
    done
    debugfs -w -f "$script" "$FS" > /tmp/diskfiles-debugfs.log 2>&1 || true
    # debugfs keeps going on errors; surface real ones ("File not found" from
    # rm of a new file and "already exists" from mkdir are expected).
    # Ignore debugfs' banner and command echo ("debugfs: ..."), too.
    if grep -vE '^debugfs|File not found by ext2_lookup|already exists|^Allocated inode' \
           /tmp/diskfiles-debugfs.log | grep -q .; then
        cat /tmp/diskfiles-debugfs.log >&2; die "debugfs reported errors (above)"
    fi
    echo "copied into $dest:"; dfs "ls -l $dest" | grep -v '^debugfs' | tail -n +1
    ;;
get)
    [ $# -eq 2 ] || usage
    mkdir -p "$2"
    out="$(dfs "rdump $1 $2")"
    if printf '%s\n' "$out" | grep -qiE 'not found|error'; then printf '%s\n' "$out" >&2; die "could not copy $1"; fi
    echo "copied $1 to $2/$(basename "$1")"
    ;;
ls)
    dfs "ls -l ${1:-/root}" | grep -v '^debugfs'
    ;;
-h|--help|"") usage 0 ;;
*) die "unknown command '$cmd' (put, get, ls)" ;;
esac
