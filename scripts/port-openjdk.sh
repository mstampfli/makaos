#!/usr/bin/env bash
# port-openjdk.sh - cross-build OpenJDK 17 (Zero interpreter) to x86_64-pc-makaos.
#
# The OpenJDK source tree is gitignored (build/third_party/openjdk17u), so this
# script records every patch + build step needed to reproduce the port that runs
# `java -version` and `java Hello` on MakaOS. Strategy: masquerade MakaOS as a
# linux-family target so the build uses os/linux + os_cpu/linux_zero + cpu/zero
# compiled against MakaOS libc (whose ucontext/signals are already glibc-layout).
#
# Prereqs:
#   - a prior `bash build.sh` (fresh build/sysroot: libc.a + C++-safe headers)
#   - host boot JDK at /usr/lib/jvm/java-17-openjdk-amd64
#   - the openjdk17u source unpacked at build/third_party/openjdk17u
#   - the MakaOS gcc cross-toolchain on PATH (toolchain/bin)
#
# After this runs: `make hotspot java.base` produces the exploded JDK image at
# build/third_party/openjdk17u/build/linux-x86_64-zero-release/jdk/. Boot it with
#   AVIANTEST=0 JDKTEST=1 CONSOLE_SERIAL=1 bash build.sh   (then boot at -m 1024M)
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
JDK="$REPO/build/third_party/openjdk17u"
SYSROOT="$REPO/build/sysroot"
TOOLS="$REPO/toolchain"
GCCVER_DIR="$TOOLS/lib/gcc/x86_64-pc-makaos/14.2.0"
BOOTJDK="/usr/lib/jvm/java-17-openjdk-amd64"
CONF="linux-x86_64-zero-release"

[ -d "$JDK" ] || { echo "[port-openjdk] missing $JDK (unpack openjdk17u there first)"; exit 1; }
export PATH="$TOOLS/bin:$PATH"

echo "[port-openjdk] 1/8 autoconf: teach the build about the makaos triplet"
# config.sub: accept *-makaos (echo the triplet straight back), like pc-wsl/pc-msys.
CS="$JDK/make/autoconf/build-aux/config.sub"
grep -q "grep makaos" "$CS" || sed -i '0,/^# /s//# Allow makaos (MakaOS cross target; treated as linux-family in platform.m4)\nif echo $* | grep makaos >\/dev\/null ; then echo $*; exit; fi\n# /' "$CS"

# platform.m4: map *makaos* onto the linux os-layer + gnu libc/abi.
PM="$JDK/make/autoconf/platform.m4"
if ! grep -q "makaos" "$PM"; then
  perl -0pi -e 's/(\*linux\*\)\s*\n\s*VAR_OS=linux\s*\n\s*VAR_OS_TYPE=unix\s*\n\s*;;)/$1\n    *makaos*)\n      # MakaOS presents a glibc-flavored POSIX\/linux personality; build the\n      # linux os-layer (os\/linux, os_cpu\/linux_zero) against MakaOS libc.\n      VAR_OS=linux\n      VAR_OS_TYPE=unix\n      ;;/' "$PM"
  # VAR_LIBC=gnu and VAR_ABI=gnu for the makaos case (two later switch arms).
  perl -0pi -e 's/(VAR_LIBC=gnu\s*\n\s*;;)/$1\n    *makaos*)\n      VAR_LIBC=gnu\n      ;;/' "$PM"
  perl -0pi -e 's/(VAR_ABI=gnuabi64\s*\n\s*;;)/$1\n    *makaos*)\n      VAR_ABI=gnu\n      ;;/' "$PM"
fi

# flags-ldflags.m4: drop -Wl,-z,defs. MakaOS .so's resolve libc at load against
# the launcher's exported dynsym, so undefined symbols in a .so are expected.
sed -i 's/.*-Wl,-z,defs.*/    # NOTE(makaos): -z,defs (forbid undefined symbols) is intentionally omitted./' "$JDK/make/autoconf/flags-ldflags.m4" || true

echo "[port-openjdk] 2/8 launcher: static-link libjli (MakaOS has no DT_NEEDED at exec)"
# MakaOS's kernel ELF loader loads a static PIE and applies R_X86_64_RELATIVE; it
# does NOT process a main executable's DT_NEEDED. So the java launcher must embed
# jli statically (no shared -ljli) and dlopen libjvm.so at runtime -- the Avian
# model. Build libjli_static for linux (upstream only builds it for aix) ...
sed -i 's/ifeq ($(call isTargetOs, aix), true)/ifeq ($(call isTargetOs, aix linux), true)/' \
    "$JDK/make/modules/java.base/lib/CoreLibraries.gmk"
# ... and point the linux launcher at it + drop --exclude-libs (which would hide
# the launcher's archived libc symbols from the dynsym the dlopen'd .so's need).
LC="$JDK/make/common/modules/LauncherCommon.gmk"
sed -i 's/  LDFLAGS_JDKEXE += -Wl,--exclude-libs,ALL/  # MakaOS: launcher must export its libc symbols for the dlopen'"'"'d libjvm.so/' "$LC"
sed -i 's/-ljli -lpthread $(LIBDL), \\/-ljli_static -lpthread $(LIBDL), \\/' "$LC"
grep -q "native/java.base, \\\\" "$LC" || \
  sed -i 's#-L$(call FindLibDirForModule, java.base), \\#-L$(call FindLibDirForModule, java.base) \\\n          -L$(SUPPORT_OUTPUTDIR)/native/java.base, \\#' "$LC"
sed -i 's/  ifeq ($(call isTargetOs, aix), true)\n    $$(BUILD_LAUNCHER_$1): $(call FindStaticLib, java.base, jli_static)/  ifeq ($(call isTargetOs, aix linux), true)\n    $$(BUILD_LAUNCHER_$1): $(call FindStaticLib, java.base, jli_static)/' "$LC" || \
  perl -0pi -e 's/ifeq \(\$\(call isTargetOs, aix\), true\)\n(\s*\$\$\(BUILD_LAUNCHER_\$1\): \$\(call FindStaticLib, java.base, jli_static\))/ifeq (\$(call isTargetOs, aix linux), true)\n$1/' "$LC"

echo "[port-openjdk] 3/8 toolchain: gcc specs for MakaOS static-PIE + shared links"
# The gcc driver defaults to -static executables. Teach it: -shared -> ET_DYN with
# BOTH hash tables (MakaOS's loader reads DT_HASH); -pie -> the proven MakaOS
# dynamic-PIE recipe (export-dynamic + link-pie.ld) with the whole libc group so
# the launcher exports libc/libm/... to the .so's it dlopens.
cat > "$GCCVER_DIR/specs" <<'SPECS'
*startfile:
%{shared:crtdso.o%s;:crt0.o%s}

*link:
%{shared:-shared -Bdynamic --hash-style=both;pie:-pie -Bdynamic --export-dynamic --hash-style=sysv --build-id=none -e _entry %{!T*:-T %R/usr/lib/link-pie.ld};:-static %{!T*:-T %R/usr/lib/makaos-link.ld}} -nostdlib

*lib:
%{shared:;:--whole-archive -lc -lm -lz -lpthread -ldl --no-whole-archive -lstdc++ -lgcc}
SPECS
# gcc's fixincludes copy of stdio.h SHADOWS the sysroot stdio.h; remove it so
# libc header edits are visible (only stdio.h + stddef.h get fixincluded).
rm -f "$GCCVER_DIR/include-fixed/stdio.h"

echo "[port-openjdk] 4/8 vendor cups/alsa/fontconfig/libffi API headers into sysroot"
# configure probes these even though we build headless; the headers just need to
# exist. (The stub bodies are never linked for a headless Zero build.)
# NOTE: the actual header contents were vendored by hand during bring-up; if this
# sysroot lacks them, copy from a distro's -dev packages or stub the few types
# configure checks. Left as a documented manual step.
for h in cups/cups.h cups/ppd.h alsa/asoundlib.h fontconfig/fontconfig.h ffi.h; do
  [ -f "$SYSROOT/usr/include/$h" ] || echo "  [warn] $SYSROOT/usr/include/$h missing -- vendor it before configure"
done

echo "[port-openjdk] 5/8 configure (Zero variant, headless, cross to makaos)"
cd "$JDK"
if [ ! -f "build/$CONF/spec.gmk" ]; then
  bash configure \
    --openjdk-target=x86_64-pc-makaos \
    --with-jvm-variants=zero \
    --with-boot-jdk="$BOOTJDK" \
    --with-build-jdk="$BOOTJDK" \
    --with-sysroot="$SYSROOT" \
    --with-toolchain-type=gcc \
    --enable-headless-only \
    --disable-warnings-as-errors \
    --disable-precompiled-headers \
    --with-freetype=bundled \
    --with-cups-include="$SYSROOT/usr/include" \
    --with-alsa-include="$SYSROOT/usr/include" \
    --with-fontconfig-include="$SYSROOT/usr/include" \
    --with-libffi-include="$SYSROOT/usr/include" \
    --with-libffi-lib="$SYSROOT/usr/lib" \
    --with-native-debug-symbols=none
fi

echo "[port-openjdk] 6/8 patch the generated spec.gmk (post-configure)"
SPEC="build/$CONF/spec.gmk"
# -D__linux__ on the JDK-native compiles (TimeZone_md.c etc. are guarded by it).
sed -i 's/-DLINUX -DNDEBUG/-DLINUX -D__linux__ -DNDEBUG/g' "$SPEC"
# The .so's must carry DT_HASH (MakaOS's loader reads it); libjvm forces gnu-only.
sed -i 's/--hash-style=gnu/--hash-style=both/g' "$SPEC"
# Belt-and-suspenders: strip any -z,defs the configure re-added.
sed -i 's/-Wl,-z,defs//g' "$SPEC"

echo "[port-openjdk] 7/8 build hotspot (libjvm.so) + java.base (libs + launcher + classes)"
make hotspot java.base CONF="$CONF" LOG=info

echo "[port-openjdk] 8/8 done."
echo "  Image: $JDK/build/$CONF/jdk  (bin/java, lib/*.so, modules/java.base)"
echo "  Run on MakaOS:  AVIANTEST=0 JDKTEST=1 CONSOLE_SERIAL=1 bash build.sh   (boot at -m 1024M)"
echo ""
echo "MakaOS-side support this port relies on (already committed to the repo):"
echo "  - libc: SSP runtime, dladdr, SONAME/\$ORIGIN dep resolution, RTLD_DEFAULT"
echo "          in dlsym, printf '*'/scanf '%n','ll', + ~40 header/symbol gap fills"
echo "  - kernel: real si_code for faults; RT signals (32-63) accepted as no-ops"
