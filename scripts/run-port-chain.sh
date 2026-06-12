#!/usr/bin/env bash
# ── run-port-chain.sh — rebuild the full sysroot port DAG ─────────────
#
# One-shot orchestration for a fresh checkout: runs build.sh (which
# owns libc + the Tier-2 ports), then every higher-tier port script in
# dependency order, then build.sh again so the refreshed sysroot
# binaries (dwl/foot/tinywl/…) land in the ext2 image.
#
# Each port logs to build/port_chain_<name>.log; progress lines go to
# stdout so a supervisor can stream them.  Stops on first failure.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PORTS=(
    libiconv
    fontconfig
    wayland
    wayland-protocols
    libudev
    libseat
    mtdev
    libevdev
    libinput
    hwdata
    libdisplay-info
    wlroots
    json-c
    pcre2
    dwl
    tinywl
    tllist
    libutf8proc
    fcft
    foot
    sdl3
    # sway tier — text stack + the compositor itself
    fribidi
    libpng
    glib
    cairo
    pango
    sway
    # desktop environment: wallpaper + launcher (taskbar = swaybar,
    # built by the sway port; -Dswaybar/-Dswaynag enabled in port-sway.sh)
    swaybg
    tofi
    wlr-randr
)

# The meson cross file embeds absolute paths; regenerate it for this
# checkout before anything consumes it.  Then make sure the sysroot
# has the link/include scaffolding every meson port assumes.
bash scripts/gen-meson-cross.sh
bash scripts/gen-sysroot-scaffold.sh

# START_FROM=<port> resumes mid-chain: skips the leading build.sh pass
# and every port before <port>.  Default: run everything.
START_FROM="${START_FROM:-}"

if [ -z "$START_FROM" ]; then
    echo "PORT-START build.sh(pass2)"
    if ! NO_QEMU=1 bash build.sh > build/port_chain_buildsh_pass2.log 2>&1; then
        echo "PORT-FAILED build.sh(pass2) — tail of log:"
        tail -5 build/port_chain_buildsh_pass2.log
        exit 1
    fi
    echo "PORT-OK build.sh(pass2)"
fi

skipping="$START_FROM"
for p in "${PORTS[@]}"; do
    if [ -n "$skipping" ] && [ "$p" != "$skipping" ]; then
        echo "PORT-SKIP $p"
        continue
    fi
    skipping=""
    echo "PORT-START $p"
    if ! bash "scripts/port-$p.sh" > "build/port_chain_$p.log" 2>&1; then
        echo "PORT-FAILED $p — tail of log:"
        tail -5 "build/port_chain_$p.log"
        exit 1
    fi
    echo "PORT-OK $p"
done

echo "PORT-START build.sh(final)"
if ! NO_QEMU=1 bash build.sh > build/port_chain_buildsh_final.log 2>&1; then
    echo "PORT-FAILED build.sh(final) — tail of log:"
    tail -5 build/port_chain_buildsh_final.log
    exit 1
fi
echo "PORT-OK build.sh(final)"
echo "ALL-DONE"
