#!/usr/bin/env bash
# Sourced by the scripts that launch QEMU. Decides what this host's QEMU can do
# for MakaOS: the binary and CPU acceleration it boots with, and the window
# backend that works here.
#
# Acceleration. The kernel requires x2APIC (kernel/time/lapic.c halts without
# it; the serial log then ends at "K"). Two setups provide it:
#   1. KVM with the host CPU (fast). Needs vmx/svm and a usable /dev/kvm.
#   2. QEMU 9.0 or newer in software emulation (TCG) with -cpu max,+x2apic.
#      Slower, but needs no virtualization support at all. QEMU 8.x and older
#      do not emulate x2APIC under TCG.
#
# Display. SDL is the default window backend; QEMU_DISPLAY overrides it (e.g.
# gtk).  Under WSLg both SDL and GTK work, 2D and with virgl (gl=on), checked
# 2026-11-09 with QEMU 9.2.0.
#
# Usage:
#   . scripts/qemu-host.sh
#   qemu_select_accel || exit 1
#   "$QEMU_BIN" "${QEMU_ACCEL_ARGS[@]}" -display "$(qemu_display)" ...
#
# Overrides:
#   QEMU=/path/to/qemu-system-x86_64   use this binary
#   QEMU_ACCEL=kvm|tcg                 force one mode instead of auto-detecting
#   QEMU_DISPLAY=sdl|gtk|...           force a display backend
#   QEMU_AUDIO=pa|none|...             host audio backend (none = sound card stays, output discarded)

# Prints the major version of a qemu-system binary, or nothing if unusable.
_qemu_major() {
    "$1" --version 2>/dev/null | sed -n 's/^QEMU emulator version \([0-9][0-9]*\)\..*/\1/p'
}

_kvm_usable() {
    grep -qE '\b(vmx|svm)\b' /proc/cpuinfo && [ -r /dev/kvm ] && [ -w /dev/kvm ]
}

# Finds a QEMU >= 9.0 for TCG: $QEMU, then the one on PATH, then the newest
# under ~/opt/qemu-*/ (where docs/BUILDING.md suggests installing one).
_find_tcg_qemu() {
    local c major best="" best_major=0
    for c in ${QEMU:+"$QEMU"} "$(command -v qemu-system-x86_64 2>/dev/null)" \
             "$HOME"/opt/qemu-*/bin/qemu-system-x86_64; do
        [ -n "$c" ] && [ -x "$c" ] || continue
        major="$(_qemu_major "$c")"
        [ -n "$major" ] && [ "$major" -ge 9 ] || continue
        if [ "$major" -gt "$best_major" ]; then best="$c"; best_major="$major"; fi
    done
    [ -n "$best" ] && printf '%s\n' "$best"
}

qemu_select_accel() {
    local mode="${QEMU_ACCEL:-auto}" tcg_qemu
    QEMU_BIN=""
    QEMU_ACCEL_ARGS=()

    if [ "$mode" = "kvm" ] || { [ "$mode" = "auto" ] && _kvm_usable; }; then
        QEMU_BIN="${QEMU:-qemu-system-x86_64}"
        QEMU_ACCEL_ARGS=(-accel kvm -cpu host)
        return 0
    fi

    tcg_qemu="$(_find_tcg_qemu)"
    if [ -n "$tcg_qemu" ]; then
        QEMU_BIN="$tcg_qemu"
        QEMU_ACCEL_ARGS=(-accel tcg -cpu max,+x2apic)
        echo "[qemu] No KVM: using software emulation with $QEMU_BIN (QEMU $(_qemu_major "$QEMU_BIN")). Expect it to be slower." >&2
        return 0
    fi

    echo "[!] Cannot boot MakaOS: it needs x2APIC, and this machine offers neither way to get it:" >&2
    if [ "$mode" = "tcg" ]; then
        echo "    - KVM: not tried (QEMU_ACCEL=tcg)." >&2
    elif ! grep -qE '\b(vmx|svm)\b' /proc/cpuinfo; then
        echo "    - KVM: the CPU shows no hardware virtualization (no vmx/svm flag)." >&2
        echo "      WSL2: set nestedVirtualization=true under [wsl2] in %UserProfile%\\.wslconfig," >&2
        echo "      then 'wsl --shutdown'. On AMD hosts, Windows update KB5127648 (build 26100.9711)" >&2
        echo "      breaks this silently; KB5127936 fixes it after a full restart." >&2
    else
        echo "    - KVM: /dev/kvm is missing or not read/writable for $(id -un) (join the kvm group)." >&2
    fi
    echo "    - Software emulation: no QEMU 9.0 or newer found (\$QEMU, PATH, ~/opt/qemu-*/bin)." >&2
    echo "    See docs/BUILDING.md, section \"Booting without KVM\"." >&2
    return 1
}

# Prints the -display backend for a window: $QEMU_DISPLAY if set, else SDL.
# Optional $1 is appended as backend options (e.g. "gl=on").
qemu_display() {
    printf '%s%s\n' "${QEMU_DISPLAY:-sdl}" "${1:+,$1}"
}

# Prints the -audiodev backend: $QEMU_AUDIO if set, else PulseAudio.  "none"
# keeps the guest's HDA card (nothing changes for MakaOS) but discards output.
qemu_audio() {
    printf '%s\n' "${QEMU_AUDIO:-pa}"
}
