#!/usr/bin/env bash
# Cross-build the Avian JVM (interpreter) for MakaOS.
#
# Produces a position-independent, dynamically-linkable MakaOS x86-64 `avian`
# executable plus its classpath.jar (Avian's bundled minimal class library).
# Build with AVIANTEST=1 to install them onto the disk image and run
# /avian/Hello.class at boot; see docs/JVM_PORT_PLAN.md.
#
# Requires:
#   - a host JDK (javac/java) for Avian's classpath compile + generators
#     (JAVA_HOME auto-detected from `javac` if unset);
#   - a prior MakaOS build so build/sysroot has the fresh libc.a + C++-safe
#     headers (run `bash build.sh` first).
#
# Design notes (why the flags):
#   - process=interpret bootimage=false  : no JIT bootimage cross-generation.
#   - classpath=avian                    : Avian's own tiny class library (no OpenJDK).
#   - java-version=8                     : Avian parses the old `javac 1.x` version
#                                          string; a modern JDK breaks it, so pin 8
#                                          (-> `-source 1.8`, still supported).
#   - cc/cxx = makaos toolchain + --sysroot ; build-cc/build-cxx = host (Avian's
#     type/bootimage generators must run on the host).
#   - rdynamic=-Wl,--export-dynamic      : the makaos gcc rejects bare -rdynamic.
#
# Why we re-link instead of using Avian's own final link (step 5):
#   Avian resolves its builtin native methods (Avian_java_lang_System_arraycopy,
#   ...) at runtime via dlsym on dlopen(NULL) -- the *main executable's* dynamic
#   symbol table.  Avian's make links a static ET_EXEC with no PT_DYNAMIC/DT_HASH,
#   so those symbols are undlsym-able -> UnsatisfiedLinkError.  We instead link a
#   PIE (ET_DYN) with --export-dynamic --hash-style=sysv, which MakaOS's loader
#   maps at VMM_USER_CODE_BASE and whose SysV hash table dlfcn.c can read.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$(pwd)"
AVIAN="$REPO/build/third_party/avian"
SYSROOT="$REPO/build/sysroot"
TOOLS="$REPO/toolchain/bin"
OUT="$REPO/build/avian-app"
BUILT="$AVIAN/build/linux-x86_64-interpret"

: "${JAVA_HOME:=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
export JAVA_HOME
[ -f "$JAVA_HOME/bin/javac" ] || { echo "FATAL: no host JDK (set JAVA_HOME)"; exit 1; }
[ -f "$SYSROOT/usr/lib/libc.a" ] || { echo "FATAL: run 'bash build.sh' first (no build/sysroot)"; exit 1; }
[ -x "$TOOLS/x86_64-pc-makaos-g++" ] || { echo "FATAL: makaos toolchain missing"; exit 1; }

# 1. Fetch Avian.
if [ ! -d "$AVIAN" ]; then
    echo "[port-avian] cloning Avian"
    mkdir -p "$REPO/build/third_party"
    git clone --depth 1 https://github.com/ReadyTalk/avian "$AVIAN"
fi

# 2. Portability patches (idempotent) -- Avian quirks, not MakaOS bugs.
#    (a) posix.cpp relies on a non-POSIX transitive limits.h for PATH_MAX.
#    (b) java-lang.cpp includes the unused, removed-in-modern-libc sys/sysctl.h.
grep -q '__FreeBSD__ || defined __makaos__' "$AVIAN/src/system/posix.cpp" || \
    sed -i 's/#if defined __FreeBSD__$/#if defined __FreeBSD__ || defined __makaos__/' \
        "$AVIAN/src/system/posix.cpp"
grep -q '!defined __ANDROID__ && !defined __makaos__' "$AVIAN/classpath/java-lang.cpp" || \
    perl -0pi -e 's/#ifndef __ANDROID__\n#include "sys\/sysctl.h"/#if !defined __ANDROID__ && !defined __makaos__\n#include "sys\/sysctl.h"/' \
        "$AVIAN/classpath/java-lang.cpp"

# 3. Correctness patch (idempotent) -- a genuine Avian interpreter+JIT gap, not a
#    MakaOS bug: invokeinterface of a java.lang.Object method (hashCode/equals/
#    toString) on an interface-typed reference resolves the target to Object,
#    which is not an interface and is absent from the receiver's interface table,
#    so findInterfaceMethod() falls through to abort(t).  Avian's own HashMap.java
#    (`cell.hashCode()` where cell is the Cell interface) trips this during
#    bootstrap.  Per JVMS invokeinterface such a call dispatches virtually.  The
#    fix lives in the shared findInterfaceMethod so it covers interpreter AND JIT.
if ! grep -q 'ACC_INTERFACE) == 0' "$AVIAN/src/avian/machine.h"; then
    perl -0pi -e 's/(  GcClass\* interface = method->class_\(\);\n)(  GcArray\* itable = cast<GcArray>\(t, class_->interfaceTable\(\)\);)/$1\n  \/\/ A method invoked through an interface reference may actually be a member\n  \/\/ of java\/lang\/Object (hashCode\/equals\/toString\/...).  Object is not an\n  \/\/ interface and is therefore absent from the interface table, so such a\n  \/\/ call must dispatch virtually per the JVMS invokeinterface rules.\n  if ((interface->flags() & ACC_INTERFACE) == 0) {\n    return findVirtualMethod(t, method, class_);\n  }\n\n$2/' \
        "$AVIAN/src/avian/machine.h"
    grep -q 'ACC_INTERFACE) == 0' "$AVIAN/src/avian/machine.h" || \
        { echo "FATAL: findInterfaceMethod patch failed to apply"; exit 1; }
fi

# 4. Cross-build the classpath.jar and all VM object files.  Avian's own final
#    link (the "$BUILT/avian" target) makes a static ET_EXEC we do not use; it may
#    even fail under the makaos toolchain -- that is fine, the .o files are what we
#    need, so tolerate a nonzero make status as long as the objects exist.
MCC="$TOOLS/x86_64-pc-makaos-gcc --sysroot=$SYSROOT"
MCXX="$TOOLS/x86_64-pc-makaos-g++ --sysroot=$SYSROOT"
echo "[port-avian] cross-building (JAVA_HOME=$JAVA_HOME)"
make -C "$AVIAN" -j"$(nproc)" \
    platform=linux arch=x86_64 process=interpret bootimage=false classpath=avian \
    java-version=8 \
    cc="$MCC" cxx="$MCXX" build-cc=gcc build-cxx=g++ rdynamic=-Wl,--export-dynamic \
    "$BUILT/classpath.jar" "$BUILT/avian" || true
[ -f "$BUILT/interpret.o" ] || { echo "FATAL: Avian objects did not build"; exit 1; }

# 5. Link a MakaOS PIE from the objects (see header comment for why).  Object set
#    is the fixed process=interpret bootimage=false list; libc.a precedes
#    libstdc++.a so libc's sinf/cosf win (no -z muldefs needed).
LD="$TOOLS/x86_64-pc-makaos-ld"
L="$SYSROOT/usr/lib"
LIBGCC="$(find "$REPO/toolchain/lib/gcc/x86_64-pc-makaos" -name libgcc.a | head -1)"
LIBSTDCXX="$REPO/toolchain/x86_64-pc-makaos/lib/libstdc++.a"
OBJNAMES="system/posix.o system/posix/crash.o system/posix/memory.o \
system/posix/signal.o finder.o machine.o util.o heap/heap.o interpret.o \
classpath-avian.o builtin.o jnienv.o process.o heapdump.o x86_64-asm.o java-io.o \
java-net.o java-nio.o java-util-zip.o java-util.o sockets.o java-lang.o main.o \
heapwalk.o boot.o classpath-jar.o"
OBJS=""; for o in $OBJNAMES; do OBJS="$OBJS $BUILT/$o"; done
echo "[port-avian] linking MakaOS PIE"
"$LD" -m elf_x86_64_makaos -pie --export-dynamic --hash-style=sysv -e _entry \
    --build-id=none -T "$REPO/userland/link-pie.ld" \
    "$L/crt0.o" $OBJS \
    --start-group \
        "$L/libc.a" "$L/libm.a" "$L/libz.a" "$L/libpthread.a" "$L/libdl.a" \
        "$LIBSTDCXX" "$LIBGCC" \
    --end-group \
    -o "$BUILT/avian-pie"

# 6. Stage artifacts for build.sh's AVIANTEST installer.
mkdir -p "$OUT"
cp "$BUILT/avian-pie"     "$OUT/avian"
cp "$BUILT/classpath.jar" "$OUT/classpath.jar"

# 7. A trivial Hello.class targeting Java 8 bytecode (Avian is old; it will not
#    read modern classfiles).
cat > "$OUT/Hello.java" <<'JAVA'
public class Hello {
    public static void main(String[] args) {
        System.out.println("Hello from Avian JVM on MakaOS");
    }
}
JAVA
"$JAVA_HOME/bin/javac" -source 8 -target 8 -d "$OUT" "$OUT/Hello.java" 2>/dev/null

echo "[port-avian] done: $OUT/{avian,classpath.jar,Hello.class}"
echo "[port-avian] run first-light with:  AVIANTEST=1 CONSOLE_SERIAL=1 bash build.sh"
echo "[port-avian] (boot at -m 1024M; kernel/main.c passes -Xbootclasspath:/avian/classpath.jar)"
