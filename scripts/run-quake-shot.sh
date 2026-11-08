#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
RENDERNODE="${RENDERNODE:-/dev/dri/renderD129}"
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS_gl.fd
: > build/serial-gl.txt; rm -f build/qmp.sock build/qframe*.ppm
qemu-system-x86_64 -accel kvm -cpu host -smp 4 -m 1024M -nodefaults -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS_gl.fd \
  -drive format=raw,file=build/disk.img,if=none,id=hd0,snapshot=on \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga std -device virtio-gpu-gl-pci,id=vgpu -display egl-headless,rendernode="$RENDERNODE" \
  -serial file:build/serial-gl.txt -qmp unix:build/qmp.sock,server,nowait \
  -no-reboot -no-shutdown 2>build/qemu-gl-err.txt &
QPID=$!
for i in $(seq 1 3300); do ss -x >/dev/null 2>&1; done   # ~65s: sway + Quake up
python3 - <<'PY'
import socket,json,time
s=socket.socket(socket.AF_UNIX); s.connect("build/qmp.sock")
f=s.makefile('rw')
f.readline(); f.write(json.dumps({"execute":"qmp_capabilities"})+"\n"); f.flush(); f.readline()
def shot(name, dev=None):
    a={"filename":name}
    if dev: a["device"]=dev
    f.write(json.dumps({"execute":"screendump","arguments":a})+"\n"); f.flush()
    print(name, f.readline().strip()[:90])
for n in range(1,5):
    shot(f"build/qframe{n}.ppm","vgpu")
    time.sleep(3)
PY
kill -9 $QPID 2>/dev/null
echo "frames:"; for f in build/qframe*.ppm; do [ -s "$f" ] && convert "$f" -format "  %f %wx%h mean=%[fx:mean] colors=%k\n" info: 2>/dev/null; done
