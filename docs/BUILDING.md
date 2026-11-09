# Building and running MakaOS

This is the canonical guide. It was checked on 2026-11-08 by building a fresh clone of
`main` from scratch on WSL2 Ubuntu 24.04 (16 cores).

**MakaOS is a hobby OS: not usable for real work, and insecure (see the README). Run it
only inside QEMU, on a host you don't mind experimenting on.**

## Requirements

- An x86-64 Linux host (WSL2 works). All commands run from the repo root.
- **To boot: KVM, or QEMU 9.0 or newer.** The kernel requires x2APIC and halts without
  it (`kernel/time/lapic.c`); the serial log then ends right after `K`. Two setups
  provide x2APIC:
  - **KVM** (fast): the CPU must show `vmx` or `svm` in `/proc/cpuinfo`, and your user
    must be in the `kvm` group. WSL2: set `nestedVirtualization=true` under `[wsl2]` in
    `%UserProfile%\.wslconfig`, run `wsl --shutdown`, and reopen WSL.
  - **Software emulation** with QEMU 9.0 or newer: slower, but needs no virtualization
    support. QEMU 8.x (Ubuntu 24.04's) does not emulate x2APIC; see
    [Booting without KVM](#booting-without-kvm).
  - `run.sh`, `scripts/boot.sh` and `build.sh` pick the mode themselves
    (`scripts/qemu-host.sh`): KVM when it works, otherwise the newest QEMU 9+ found in
    `$QEMU`, `PATH` or `~/opt/qemu-*/bin`, otherwise a message saying what is missing.
    Force a mode with `QEMU_ACCEL=kvm` or `QEMU_ACCEL=tcg`.
  - Building needs neither; only booting does.
- Host packages (Debian/Ubuntu names):

```sh
sudo apt install build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev \
  curl xz-utils clang lld nasm gdisk mtools e2fsprogs qemu-system-x86 ovmf \
  meson ninja-build pkg-config python3 xkb-data fonts-dejavu-core
```

- Network access on the first build: the scripts download binutils, gcc, bash and the
  base libraries into `build/third_party/`.

## Quick start: kernel, login and shell (no cross-compiler)

```sh
git clone --recurse-submodules https://github.com/mstampfli/makaos.git
cd makaos
MINIMAL=1 NO_QEMU=1 bash build.sh   # 20 to 75 s from a fresh clone
./run.sh
```

`MINIMAL=1` builds the kernel, the UEFI loader, libc and the in-tree core programs
(`login`, `/bin/shell`, `svcmgr`, `net`, `ls`, `ps`, `cat`, `echo`, `mkdir`, `rm`, `mv`,
`clear`, `reboot`, `uname`, `hello`) with the host gcc. There are no ports, no bash and no
desktop; root's login shell is the in-tree `/bin/shell`.

## Full build

```sh
bash scripts/build-toolchain.sh   # once: cross binutils + gcc + hosted libstdc++
NO_QEMU=1 bash build.sh           # ports, libc, apps, bash, kernel, disk image
./run.sh                          # boot in QEMU (SDL window)
```

1. `scripts/build-toolchain.sh` builds the `x86_64-pc-makaos` cross-compiler into
   `toolchain/`. It needs MakaOS's `libc.a` first, so on a fresh clone it runs
   `build.sh` in bootstrap mode itself (libc only, built with the host gcc). This is
   the long step: about 7 minutes of compiling on 16 cores (measured 2026-11-08), plus
   downloading about 120 MB of binutils and gcc source, which can take far longer when
   ftp.gnu.org is slow. It is skipped on later runs unless you pass `FORCE=1`.
2. `build.sh` builds mbedTLS and the base libraries (zlib, expat, libffi, pixman,
   libxkbcommon, freetype, harfbuzz, libdrm), then libc, the userland programs, bash 5.2
   (source fetched automatically; set `BASH_SRC` to use your own tree), the kernel and
   the UEFI loader, and assembles `build/disk.img` (GPT: FAT ESP + ext2 root). First run:
   about 7 minutes. Ports are skipped when already built, so reruns take about 25 seconds.
3. Without `NO_QEMU=1`, `build.sh` boots the image headless at the end (serial to
   `build/serial.txt`, network capture to `build/net.pcap`). `run.sh` boots it with a
   window.

The default image has no graphical desktop. sway, foot, Mesa, SDL and the JVMs are built
separately; see the next section.

## Desktop and JVM ports

Built from a fresh clone and tested on 2026-11-09 (WSL2 Ubuntu 24.04, QEMU 9.2.0 without
KVM). Needs the full build above first, plus these host packages:

```sh
sudo apt install openjdk-17-jdk-headless python3-mako gperf scdoc gettext glslang-tools \
  libcups2-dev libasound2-dev
```

```sh
bash scripts/run-port-chain.sh   # wayland ... Mesa, wlroots, foot, SDL3, glib, pango, sway, swaybg, tofi
for p in glfw libjpeg libogg libvorbis sdl2-compat darkplaces; do bash scripts/port-$p.sh; done
JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64 bash scripts/port-avian.sh
bash scripts/port-openjdk.sh     # OpenJDK 17 Zero, java.base only
NO_QEMU=1 bash build.sh          # puts the new binaries into the image
```

Every script downloads its own source (pinned versions). The port chain resumes with
`START_FROM=<port>`. Note: `port-openjdk.sh` writes a `specs` file into the cross
toolchain and deletes its fixincluded `stdio.h`, so it changes how the toolchain links
from then on.

What runs (all measured under software emulation):

| Test | How | Result |
|---|---|---|
| sway desktop on the GPU | `AUTOLOGIN=1 NO_QEMU=1 bash build.sh`, then `GLPROVIDER=none bash scripts/run-gl-gui.sh` | sway, swaybar, wallpaper; `Alt+Return` opens foot with bash; `Alt+s` opens the SDL3 test window |
| sway without virgl | 2D `virtio-gpu-pci`, log in, run `WLR_RENDERER=pixman sway` | same desktop and foot, software rendering |
| Avian JVM | `AVIANTEST=1 CONSOLE_SERIAL=1 bash build.sh` | "Hello from Avian JVM on MakaOS" |
| OpenJDK 17 Zero | `JDKTEST=1 CONSOLE_SERIAL=1 bash build.sh`, boot with 2 GiB | Hello, 800 MB heap allocated and used, `2 + 2 = 4` |

`GLPROVIDER=none` leaves the host's OpenGL setup alone; the script's default selects the
NVIDIA driver of the machine it was written on. Under WSLg the QEMU window renders through
Mesa's d3d12 driver; both `sdl,gl=on` and `gtk,gl=on` work. QEMU cannot `screendump` a
virgl display ("no surface"), so take screenshots of the host window instead.

Not tested: DarkPlaces needs Quake game data that is not in the repository; LWJGL and
Minecraft need their downloaded jars (`docs/JVM_PORT_PLAN.md`).

## Logging in and checking it works

- Accounts: `root` / `toor` and `maka` / `maka` (plaintext, by design of a toy).
- After boot you should get a `login:` prompt on the QEMU display. Log in as root and try
  `uname`, `ls /bin`, `ps`.
- `build.sh` options worth knowing:
  - `CONSOLE_SERIAL=1` mirrors the user console to serial, so a headless boot shows the
    login prompt and your session in `build/serial.txt`.
  - `SELFTESTS=1` runs the in-kernel self-tests at boot (`SELF-TEST PASSED` markers on
    serial).
  - `AUTOLOGIN=1` logs straight in (test images only). docs/FEATURES.md lists all
    build flags.

## Booting without KVM

Without KVM, MakaOS boots under QEMU 9.0 or newer in software emulation. Checked
2026-11-09 with QEMU 9.2.0 (`-accel tcg -cpu max,+x2apic`): the minimal image reached
`login:` in about 4 seconds, and the full image logged in to bash 5.2.

Build QEMU 9.2 into `~/opt` (about 10 minutes; `run.sh` finds it there by itself):

```sh
sudo apt install libglib2.0-dev libpixman-1-dev libslirp-dev libsdl2-dev libpulse-dev \
  libvirglrenderer-dev libepoxy-dev libgbm-dev
mkdir -p ~/src && cd ~/src
curl -fLO https://download.qemu.org/qemu-9.2.0.tar.xz
sha256sum qemu-9.2.0.tar.xz   # f859f0bc65e1f533d040bbe8c92bcfecee5af2c921a6687c652fb44d089bd894
tar -xJf qemu-9.2.0.tar.xz && cd qemu-9.2.0
./configure --prefix="$HOME/opt/qemu-9.2.0" --target-list=x86_64-softmmu \
  --disable-docs --enable-slirp --enable-sdl --enable-opengl --enable-virglrenderer
make -j"$(nproc)" && make install
```

`libslirp-dev` gives the guest networking (`-netdev user`), `libsdl2-dev` the window,
`libpulse-dev` the sound device `run.sh` attaches, and virglrenderer/epoxy/gbm the 3D
device (`virtio-gpu-gl-pci`) the GPU desktop needs.

WSL2 on AMD: Windows update KB5127648 (build 26100.9711, October 2026) silently breaks
nested virtualization. `/proc/cpuinfo` shows no `svm`, `dmesg` shows
`Hyper-V: Nested features: 0x0`, and `/dev/kvm` exists but QEMU reports "No such
device". The fix is KB5127936, which needs a full Windows restart (Restart, not Shut
down); see [microsoft/WSL#41917](https://github.com/microsoft/WSL/issues/41917). Until
then, use software emulation.

## Debugging

- QEMU's GDB stub listens on `tcp::1234`: `gdb build/kernel.elf`, then
  `target remote :1234` (or use `./gdb.sh`).
- `run.sh` exposes the QEMU monitor on `/tmp/qemu-hmp.sock`
  (`nc -U /tmp/qemu-hmp.sock`, then for example `info registers`).
- Map a stuck instruction pointer to source with
  `addr2line -f -i -e build/kernel.elf <RIP>`.
- More in `DEBUGGING.md`.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Serial log ends at `K`, black screen | No x2APIC: QEMU 8.x without KVM | Use KVM or QEMU 9+ (see Booting without KVM) |
| `[!] Cannot boot MakaOS: it needs x2APIC` | Same, caught before launch | The message names what is missing |
| WSL2 on AMD: no `svm`, `Nested features: 0x0` | Windows update KB5127648 | Restart Windows to apply KB5127936, or use QEMU 9+ |
| Typed `-` shows as `'`, `/` as `-` | The console uses a Swiss German keymap | Type as on a Swiss German keyboard |
| Every bash command prints `[stat-dbg] ... -> -2` | A leftover debug print in `kernel/syscall/syscall.c` | Harmless; to be removed |
| In foot, output lines start where the prompt line ended | Looks like the pty does not turn `\n` into `\r\n` (ONLCR); not yet checked in the code | Open bug |
| swaybar clock shows 1970 | Wall clock not set at boot; cause not yet checked | Open bug |
| `build.sh` prints "bootstrap mode" and stops after libc | No cross-compiler yet | Run `bash scripts/build-toolchain.sh` (or use `MINIMAL=1`) |
| A port fails to download | Network or upstream outage | Rerun; finished ports are skipped |
| No `/bin/bash` on the image | bash source unavailable | Check the bash step in the build log; login falls back to `/bin/shell` |
