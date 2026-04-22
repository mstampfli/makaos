#!/usr/bin/env bash
# ── auto-repro-pf.sh — fully automated dwl-PF reproducer (QMP) ───────
#
# Headless QEMU + QMP send-key.  QMP is the supported automation
# channel; HMP sendkey has spotty routing under -display none across
# QEMU versions.

set -eu

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

QMP=/tmp/qemu-qmp.sock

log() { printf '[auto-repro] %s\n' "$*" >&2; }

# ── QMP helpers ─────────────────────────────────────────────────────
qmp_cmd() {
    # Single-shot QMP: send capabilities handshake + command, read
    # response, close.  socat closes when stdin EOF + server response.
    {
        echo '{"execute":"qmp_capabilities"}'
        sleep 0.05
        echo "$1"
    } | socat -T 1 - UNIX-CONNECT:"$QMP" 2>/dev/null
}

# Send a QMP send-key with `qcode` name (e.g. "r", "shift", "ret").
qmp_key() {
    qmp_cmd "{\"execute\":\"send-key\",\"arguments\":{\"keys\":[{\"type\":\"qcode\",\"data\":\"$1\"}]}}" \
        > /dev/null
    sleep 0.05
}

# Send a modifier-combo (e.g. alt+shift+ret).  keys are comma-sep.
qmp_combo() {
    local keys=""
    local first=1
    for k in $(echo "$1" | tr '+' ' '); do
        if [ $first -eq 1 ]; then first=0; else keys="$keys,"; fi
        keys="$keys{\"type\":\"qcode\",\"data\":\"$k\"}"
    done
    qmp_cmd "{\"execute\":\"send-key\",\"arguments\":{\"hold-time\":200,\"keys\":[$keys]}}" \
        > /dev/null
    sleep 0.2
}

# Type a string as sequential qcodes.
qmp_type() {
    local s="$1" i ch code
    for (( i=0; i<${#s}; i++ )); do
        ch="${s:i:1}"
        case "$ch" in
            ' ') code=spc ;;
            '-') code=minus ;;
            '.') code=dot ;;
            '/') code=slash ;;
            [a-z]) code="$ch" ;;
            [A-Z]) qmp_combo "shift+$(echo -n "$ch" | tr '[:upper:]' '[:lower:]')"; continue ;;
            [0-9]) code="$ch" ;;
            *) log "can't type '$ch'"; continue ;;
        esac
        qmp_key "$code"
    done
}

# ── Clean start ─────────────────────────────────────────────────────
log "killing stale qemu + gdb + socat"
pkill -9 qemu-system-x86 2>/dev/null || true
pkill -9 -f 'gdb.*gdb-pf.cmd' 2>/dev/null || true
pkill -9 socat 2>/dev/null || true
rm -f "$QMP"
: > build/serial.txt
: > /tmp/gdb-pf.log
sleep 0.5

log "launching headless QEMU (QMP + gdb stub)"
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS.fd
qemu-system-x86_64 \
  -accel kvm -cpu host -smp 4 -m 256M \
  -nodefaults -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
  -drive format=raw,file=build/disk.img,if=none,id=hd0 \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga none -device virtio-vga,xres=1280,yres=800 \
  -display none \
  -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -serial file:build/serial.txt \
  -qmp unix:"$QMP",server,nowait \
  -gdb tcp::1234 \
  -no-reboot -no-shutdown \
  > /tmp/qemu-stdout.log 2>&1 &
QEMU_PID=$!

log "waiting for QMP socket"
for _ in $(seq 1 60); do
    [ -S "$QMP" ] && break
    sleep 0.5
done
[ -S "$QMP" ] || { log "FATAL: QMP socket never appeared"; exit 1; }

log "waiting for login ELF to load"
for _ in $(seq 1 120); do
    grep -q 'lazy /bin/login' build/serial.txt 2>/dev/null && break
    sleep 0.5
done
sleep 2   # give login time to reach its read()

log "attaching GDB (kill: breakpoint, bg)"
./scripts/debug-pf.sh &
GDB_PID=$!
sleep 3

log "sanity-testing QMP: query-status"
qmp_cmd '{"execute":"query-status"}' | head -c 200
echo ""

log "login: typing 'root' + Enter"
qmp_type "root"
qmp_key ret
sleep 2

log "login: typing 'toor' + Enter"
qmp_type "toor"
qmp_key ret
sleep 3

log "checking if bash loaded (expect 1)"
grep -c 'lazy /bin/bash' build/serial.txt

log "spawning: typing 'dwl -s foot' + Enter"
qmp_type "dwl -s foot"
qmp_key ret
sleep 6

log "trigger: Alt+Shift+Return"
qmp_combo "alt+shift_l+ret"
sleep 3

log "done —"
log "  serial PF-KILL count: $(grep -c 'PF-KILL' build/serial.txt 2>/dev/null || echo 0)"
log "  gdb capture count:    $(grep -c 'PF-KILL CAUGHT' /tmp/gdb-pf.log 2>/dev/null || echo 0)"
log "  QEMU PID: $QEMU_PID   (pkill qemu-system-x86 to stop)"
