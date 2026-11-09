# Building and running MakaOS

This is the canonical guide. It was checked on 2026-11-08 by building a fresh clone of
`main` from scratch on WSL2 Ubuntu 24.04 (16 cores).

**MakaOS is a hobby OS: not usable for real work, and insecure (see the README). Run it
only inside QEMU, on a host you don't mind experimenting on.**

## At a glance

Three image sizes. All have the same kernel; they differ in userland.

| Image | Build | What is in it |
|---|---|---|
| Minimal | `MINIMAL=1 NO_QEMU=1 bash build.sh` (about 20 s, no cross compiler) | kernel, login, `/bin/shell`, `ls`, `ps`, `cat`, `kilo` and the other core programs. No bash |
| Base | `bash scripts/build-toolchain.sh` once, then `NO_QEMU=1 bash build.sh` (about 7 min first time, then about 25 s) | the above plus bash 5.2, all in-tree programs and libraries, the cross compiler for your own programs |
| Desktop | Base, then the port scripts ([Desktop and JVM ports](#desktop-and-jvm-ports)) | the above plus sway, foot, Mesa, SDL, GLFW, Avian, OpenJDK, DarkPlaces. Every later `build.sh` then takes about 3.5 min |

How to start it:

| Command | Window | Graphics | sway | Disk writes |
|---|---|---|---|---|
| `./run.sh` | yes | 2D | no, text login | kept |
| `bash scripts/boot.sh` | yes | 2D | no, text login | kept |
| `./run-sway-gui.sh` | yes | 2D | by hand: log in, run `WLR_RENDERER=pixman sway` (software rendering; an autostarted sway needs 3D) | kept |
| `bash scripts/run-gl-gui.sh` | yes | 3D (virgl) | with an `AUTOLOGIN=1` image (it autostarts sway) | **discarded** (`snapshot=on`) |
| `bash build.sh` without `NO_QEMU=1` | no, serial to `build/serial.txt` | 2D | no | kept |

- sway starts by itself only when the image was built with `AUTOLOGIN=1` (default
  `AUTOLOGIN_SPEC=root:/bin/sway`). Otherwise you get a login prompt; log in as `root` /
  `toor`. On the Desktop image you can then start sway by hand.
- "Kept" means files you create in MakaOS are still there at the next boot.
- **Every `build.sh` run wipes the disk**: it recreates the root file system from
  scratch. That includes `scripts/boot.sh --build`, `scripts/run-port-chain.sh` and the
  `AVIANTEST`/`JDKTEST` builds. Copy files back in with `scripts/diskfiles.sh` afterwards
  ([Getting files in and out](#getting-files-in-and-out)).
- `QEMU_AUDIO=none` silences any of the launchers; `QEMU_DISPLAY=gtk` uses a GTK window.

## One-command setup

From a fresh clone, on Ubuntu/Debian or WSL2:

```sh
git clone https://github.com/mstampfli/makaos.git
cd makaos
bash scripts/setup.sh            # cross compiler + Base image (about 8 min)
bash scripts/setup.sh --qemu     # same, plus QEMU 9.2 when the machine has no KVM
bash scripts/setup.sh --desktop  # same, plus all desktop and JVM ports
```

It checks the host packages first and prints the `apt install` line for anything
missing, skips steps that are already done, and finally checks that the machine can
boot the image. The rest of this guide describes the individual steps.

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
- No git submodules are needed. The only one, `userland/apps/doom/doomgeneric`, is used
  only by the optional `build_doom.sh`, which fetches it itself (not rebuilt or tested
  since the build changes of 2026-11).

## Quick start: kernel, login and shell (no cross-compiler)

```sh
git clone https://github.com/mstampfli/makaos.git
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

DarkPlaces (Quake): `bash scripts/fetch-quake-shareware.sh` stages the freely
redistributable shareware data; the next `AUTOLOGIN=1` build makes sway start it (`Alt+d`
restarts it). Known problem: under sway it renders briefly, then goes black or is
disconnected. Not tested: LWJGL and Minecraft, which need their downloaded jars
(`scripts/fetch-minecraft.sh`).

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

## Getting files in and out

MakaOS has no text editor. Write and compile on the host, then copy into the image with
`scripts/diskfiles.sh` (works on `build/disk.img` in place, no rebuild):

```sh
toolchain/bin/x86_64-pc-makaos-gcc --sysroot=build/sysroot -O2 -o /tmp/t/prog prog.c
bash scripts/diskfiles.sh put /tmp/t              # -> /root/t (dirs recursive, +x kept)
bash scripts/diskfiles.sh put notes.txt --to /home/maka --user maka
bash scripts/diskfiles.sh ls /root/t
bash scripts/diskfiles.sh get /root/out.txt /tmp/from-makaos
```

- `put` refuses while a QEMU has the image open for writing; shut MakaOS down first.
- What persists: `run.sh` and `scripts/boot.sh` write to `build/disk.img`, so files you
  create in MakaOS survive a reboot (checked 2026-11-09: written in MakaOS, read back on
  the host with `get`). `scripts/run-gl-gui.sh` boots with `snapshot=on` and discards
  all writes. Every `build.sh` run recreates the file system, so `put` your files again
  after a build.

## Booting without KVM

Without KVM, MakaOS boots under QEMU 9.0 or newer in software emulation. Checked
2026-11-09 with QEMU 9.2.0 (`-accel tcg -cpu max,+x2apic`): the minimal image reached
`login:` in about 4 seconds, and the full image logged in to bash 5.2.

Build QEMU 9.2 into `~/opt` with `bash scripts/build-qemu.sh` (about 10 minutes; `run.sh`
finds it there by itself). It checks the host packages first, verifies the source
tarball's SHA-256 and configures SDL windows (GTK too when `libgtk-3-dev` is installed),
guest networking (slirp),
PulseAudio and the 3D device (`virtio-gpu-gl-pci`, via virglrenderer) the GPU desktop and
the 3D ioctls need.

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
| UEFI dump `#UD - Invalid Opcode` right after `kernel_phys=` | Loader zeroed memory it did not own (fixed 2026-11-09 in `boot/uefi/main.c`) | Rebuild |
| Typed `-` shows as `'`, `/` as `-` | The console uses a Swiss German keymap | Type as on a Swiss German keyboard |
| Every bash command prints `[stat-dbg] ... -> -2` | A leftover debug print in `kernel/syscall/syscall.c` | Harmless; to be removed |
| In foot, output lines start where the prompt line ended | Looks like the pty does not turn `\n` into `\r\n` (ONLCR); not yet checked in the code | Open bug |
| swaybar clock shows 1970 | Wall clock not set at boot; cause not yet checked | Open bug |
| `build.sh` prints "bootstrap mode" and stops after libc | No cross-compiler yet | Run `bash scripts/build-toolchain.sh` (or use `MINIMAL=1`) |
| A port fails to download | Network or upstream outage | Rerun; finished ports are skipped |
| No `/bin/bash` on the image | bash source unavailable | Check the bash step in the build log; login falls back to `/bin/shell` |
