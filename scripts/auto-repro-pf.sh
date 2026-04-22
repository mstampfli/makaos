#!/usr/bin/env bash
# ── auto-repro-pf.sh — QMP-driven headless PF reproducer ─────────────
#
# Spawns QEMU with VNC display (no viewer connects — just gives
# sendkey + DRM backend a valid display backend), QMP socket, and
# gdb stub.  Attaches scripts/debug-pf.sh to trap `kill:` PFs.
# Logs in root/toor, runs `dwl -s foot`, triggers Alt+Shift+Return.
# Typing driven by an embedded Python QMP client — bash was too
# fast for bash's readline (dropped chars after login).

set -eu

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

QMP=/tmp/qemu-qmp.sock

log() { printf '[auto-repro] %s\n' "$*" >&2; }

# ── Clean start ─────────────────────────────────────────────────────
log "killing stale qemu + gdb + socat"
pkill -9 qemu-system-x86 2>/dev/null || true
pkill -9 -f 'gdb.*gdb-pf.cmd' 2>/dev/null || true
pkill -9 socat 2>/dev/null || true
rm -f "$QMP"
: > build/serial.txt
: > /tmp/gdb-pf.log
sleep 0.5

log "launching QEMU (VNC display on :99, QMP + gdb stub)"
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS.fd
qemu-system-x86_64 \
  -accel kvm -cpu host -smp 4 -m 256M \
  -nodefaults -no-user-config \
  -k en-us \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
  -drive format=raw,file=build/disk.img,if=none,id=hd0 \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga none -device virtio-vga,xres=1280,yres=800 \
  -display sdl \
  -audiodev none,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -serial file:build/serial.txt \
  -qmp unix:"$QMP",server,nowait \
  -gdb tcp::1234 \
  -no-reboot -no-shutdown \
  > /tmp/qemu-stdout.log 2>&1 &
QEMU_PID=$!

log "waiting for QMP socket"
for _ in $(seq 1 60); do [ -S "$QMP" ] && break; sleep 0.5; done
[ -S "$QMP" ] || { log "FATAL: QMP socket never appeared"; exit 1; }

log "waiting for /bin/login to load (boot done)"
for _ in $(seq 1 120); do
  grep -q 'lazy /bin/login' build/serial.txt 2>/dev/null && break
  sleep 0.5
done
sleep 2

log "attaching GDB on kill: breakpoint (bg)"
./scripts/debug-pf.sh &
GDB_PID=$!
sleep 3

# ── Python QMP driver ───────────────────────────────────────────────
python3 - <<'PY' 2>&1
import socket, json, time, sys

s = socket.socket(socket.AF_UNIX)
s.connect("/tmp/qemu-qmp.sock")
fh = s.makefile('rwb')
fh.readline()                                    # banner
fh.write(b'{"execute":"qmp_capabilities"}\n')
fh.flush(); fh.readline()                         # handshake ack

def key(k, hold=100):
    req = {"execute":"send-key","arguments":{"hold-time":hold,
           "keys":[{"type":"qcode","data":k}]}}
    fh.write((json.dumps(req)+'\n').encode()); fh.flush()
    fh.readline()

def combo(ks, hold=300):
    req = {"execute":"send-key","arguments":{"hold-time":hold,
           "keys":[{"type":"qcode","data":k} for k in ks]}}
    fh.write((json.dumps(req)+'\n').encode()); fh.flush()
    fh.readline()

def typestr(s, per=0.15):
    # Our kernel uses Swiss German (CH-DE) QWERTZ.  QMP qcodes map
    # to US-position scancodes; translate so the CH-DE keymap
    # produces the intended character:
    #   '-' lives at CH-DE's scancode 0x35 = US '/' position → qcode "slash"
    #   'y' and 'z' are swapped on QWERTZ → send swapped qcodes
    CH_DE = {'y':'z', 'z':'y'}
    for c in s:
        if c == ' ':   key('spc')
        elif c == '-': key('slash')           # CH-DE: '-' at US-'/' position
        elif c == '.': key('dot')
        elif c == '/': key('7')               # CH-DE: '/' is shift+7, approx
        elif c.isalpha() and c.islower():
            key(CH_DE.get(c, c))
        elif c.isalpha() and c.isupper():
            low = c.lower()
            combo(['shift', CH_DE.get(low, low)])
        elif c.isdigit(): key(c)
        else: print(f"[py] can't type '{c}'", file=sys.stderr)
        time.sleep(per)

print("[py] login: typing 'root' + Enter")
typestr("root"); key('ret'); time.sleep(1.5)

print("[py] login: typing 'toor' + Enter")
typestr("toor"); key('ret'); time.sleep(3.0)

print("[py] launching: typing 'dwl -s foot' + Enter")
typestr("dwl -s foot"); key('ret')
print("[py] waiting 8s for dwl to come up")
time.sleep(8.0)

print("[py] trigger #1: Alt+Shift+Return")
combo(['alt','shift','ret'], hold=400); time.sleep(2.0)

print("[py] trigger #2: Alt+Shift+Return (second attempt, in case dwl missed)")
combo(['alt','shift','ret'], hold=400); time.sleep(2.0)

print("[py] done")
s.close()
PY

sleep 3
log "report:"
log "  bash loaded:      $(grep -c 'lazy /bin/bash' build/serial.txt 2>/dev/null || echo 0)"
log "  dwl loaded:       $(grep -c 'lazy /bin/dwl'  build/serial.txt 2>/dev/null || echo 0)"
log "  foot loaded:      $(grep -c 'lazy /bin/foot' build/serial.txt 2>/dev/null || echo 0)"
log "  serial PF-KILL:   $(grep -c 'PF-KILL' build/serial.txt 2>/dev/null || echo 0)"
log "  gdb capture:      $(grep -c 'PF-KILL CAUGHT' /tmp/gdb-pf.log 2>/dev/null || echo 0)"
log "  QEMU pid: $QEMU_PID   (pkill qemu-system-x86 to stop)"
