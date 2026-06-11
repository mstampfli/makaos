#!/usr/bin/env bash
# ── compositor-test.sh — headless compositor smoke test ──────────────
#
# Boots the built disk image in QEMU (VNC display backend so the DRM
# stack has a real scanout consumer, no host GUI needed), logs in as
# root via QMP send-key, runs a compositor command, and captures QMP
# screendumps at intervals so the framebuffer contents can be checked
# offline.  Reuses the CH-DE QWERTZ translation from auto-repro-pf.sh.
#
# Usage: scripts/compositor-test.sh ["command to type"] [settle-seconds]
#   default command: "dwl -s foot"
# Outputs: /tmp/comptest-{1,2,3}.ppm + build/serial.txt + QEMU stays up
#          (QMP at /tmp/qemu-qmp.sock, gdb stub :1234) for follow-up.

set -eu

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

CMD="${1:-dwl -s foot}"
SETTLE="${2:-8}"
QMP=/tmp/qemu-qmp.sock

log() { printf '[comptest] %s\n' "$*" >&2; }

log "killing stale qemu"
pkill -9 qemu-system-x86 2>/dev/null || true
rm -f "$QMP" /tmp/comptest-*.ppm
: > build/serial.txt
sleep 0.5

OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS.fd

log "launching QEMU (VNC :99, QMP, gdb stub)"
qemu-system-x86_64 \
  -accel kvm -cpu host -smp 4 -m 256M \
  -nodefaults -no-user-config \
  -k en-us \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
  -drive format=raw,file=build/disk.img,if=none,id=hd0 \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga none -device virtio-vga,xres=1280,yres=800 \
  -display vnc=127.0.0.1:99 \
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

CMD="$CMD" SETTLE="$SETTLE" python3 - <<'PY' 2>&1
import socket, json, time, sys, os

cmd     = os.environ["CMD"]
settle  = float(os.environ["SETTLE"])

s = socket.socket(socket.AF_UNIX)
s.connect("/tmp/qemu-qmp.sock")
fh = s.makefile('rwb')
fh.readline()
fh.write(b'{"execute":"qmp_capabilities"}\n')
fh.flush(); fh.readline()

def execute(req):
    fh.write((json.dumps(req)+'\n').encode()); fh.flush()
    return fh.readline()

def key(k, hold=100):
    execute({"execute":"send-key","arguments":{"hold-time":hold,
             "keys":[{"type":"qcode","data":k}]}})

def combo(ks, hold=300):
    execute({"execute":"send-key","arguments":{"hold-time":hold,
             "keys":[{"type":"qcode","data":k} for k in ks]}})

def screendump(path):
    execute({"execute":"screendump","arguments":{"filename":path}})
    time.sleep(0.5)

def typestr(t, per=0.15):
    # CH-DE QWERTZ translation (see auto-repro-pf.sh)
    CH_DE = {'y':'z', 'z':'y'}
    for c in t:
        if c == ' ':   key('spc')
        elif c == '-': key('slash')
        elif c == '.': key('dot')
        elif c == '/': combo(['shift', '7'])   # CH-DE: '/' = shift+7
        elif c == '"': combo(['shift', '2'])   # CH-DE: '"' = shift+2
        elif c == '>': combo(['shift', 'less'])# CH-DE: '>' = shift+<
        elif c == '<': key('less')
        elif c == ';': combo(['shift', 'comma']) # CH-DE: ';' = shift+,
        elif c == ':': combo(['shift', 'dot'])   # CH-DE: ':' = shift+.
        elif c == '$': combo(['shift', '4'])     # CH-DE: '$' = shift+4
        elif c.isalpha() and c.islower(): key(CH_DE.get(c, c))
        elif c.isalpha() and c.isupper():
            low = c.lower(); combo(['shift', CH_DE.get(low, low)])
        elif c.isdigit(): key(c)
        else: print(f"[py] can't type '{c}'", file=sys.stderr)
        time.sleep(per)

print("[py] login as root")
typestr("root"); key('ret'); time.sleep(1.5)
typestr("toor"); key('ret'); time.sleep(3.0)

screendump("/tmp/comptest-0.ppm")     # shell prompt before launch

print(f"[py] running: {cmd}")
typestr(cmd)
screendump("/tmp/comptest-typed.ppm")   # verify keymap translation
key('ret')

time.sleep(settle / 2)
screendump("/tmp/comptest-1.ppm")
time.sleep(settle / 2)
screendump("/tmp/comptest-2.ppm")

print("[py] done")
s.close()
PY

log "report:"
log "  dwl loaded:     $(grep -c 'lazy /bin/dwl'  build/serial.txt 2>/dev/null || true)"
log "  foot loaded:    $(grep -c 'lazy /bin/foot' build/serial.txt 2>/dev/null || true)"
log "  PF-KILL:        $(grep -c 'PF-KILL' build/serial.txt 2>/dev/null || true)"
log "  drm commits:    $(grep -c 'drm.*commit' build/serial.txt 2>/dev/null || true)"
log "  screendumps:    $(ls -la /tmp/comptest-*.ppm 2>/dev/null | wc -l)"
log "  QEMU pid: $QEMU_PID (left running; pkill qemu-system-x86 to stop)"
