#!/bin/bash
# Build bash 5.2 for MakaOS
# Usage: ./build_bash.sh [bash-source-dir]
# Requires: /tmp/bash-5.2/ (extracted bash 5.2 tarball) or pass path as arg

set -e

BASH_SRC="${1:-/tmp/bash-5.2}"
REPO="$(cd "$(dirname "$0")" && pwd)"
LIBC="$REPO/userland/libc"
ENTRY="$REPO/userland/entry"
USERLAND="$REPO/userland"
BUILD="$REPO/build"
STUBS_DIR="$REPO/userland/apps/bash"
SHIM_INC="$STUBS_DIR/include"
OBJ_DIR="/tmp/bash_build_objs"
RT_DIR="/tmp/bash_build_rt"

mkdir -p "$OBJ_DIR" "$RT_DIR" "$STUBS_DIR"

CC=x86_64-linux-gnu-gcc
LD=x86_64-linux-gnu-ld

CFLAGS=(
  -O2 -ffreestanding -nostdinc -nostdlib
  -fno-stack-protector -fno-builtin
  -I"$SHIM_INC"
  -I"$BASH_SRC" -I"$BASH_SRC/include" -I"$BASH_SRC/lib"
  -I"$LIBC"
  -DMAKAOS -DHAVE_CONFIG_H -DSHELL -DPREFER_STDARG -DOLD_ALIAS_HACK
  -DPACKAGE='"bash"' -DLOCALEDIR='"/usr/share/locale"'
  -Wno-implicit-function-declaration -Wno-int-conversion
  -Wno-incompatible-pointer-types -Wno-unused-value
  -Wno-pointer-sign -Wno-discarded-qualifiers
)

BUILTIN_CFLAGS=(
  "${CFLAGS[@]}"
  -I"$BASH_SRC/builtins"
  -DSIGSTOP=23
  -include "$BASH_SRC/include/stdc.h"
)

# Exclude files that conflict or aren't needed per-directory
EXCLUDE=(array2.c gen-helpfiles.c glob_loop.c gm_loop.c sm_loop.c getenv.c
  netconn.c netopen.c fnxform.c unicode.c random.c emacs_keymap.c vi_keymap.c
  mkbuiltins.c mksyntax.c nojobs.c memset.c strcasestr.c strchrnul.c strdup.c
  strftime.c strstr.c xfree.c oslib.c gettimeofday.c dprintf.c)
# Readline has its own shell.c, tilde.c, xmalloc.c that duplicate bash's
RL_EXCLUDE=(shell.c xmalloc.c xfree.c emacs_keymap.c vi_keymap.c)

# A pristine bash tarball lacks headers its own build generates
# (pathnames.h, version.h, signames.h, builtins/pipesize.h).  Run bash's
# configure on the host once to produce exactly those, then drop the host
# config.h it also writes: it would shadow our MakaOS shim config.h, since
# #include "config.h" searches the source dir first.
if [ ! -f "$BASH_SRC/pathnames.h" ] || [ ! -f "$BASH_SRC/builtins/pipesize.h" ]; then
  echo "[+] Preparing bash source (host configure for generated headers)"
  (cd "$BASH_SRC" && ./configure --without-bash-malloc --disable-nls > configure.makaos.log 2>&1 \
     && make pathnames.h version.h signames.h >> configure.makaos.log 2>&1 \
     && make -C builtins pipesize.h >> configure.makaos.log 2>&1) \
    || { echo "[!] bash source preparation failed, see $BASH_SRC/configure.makaos.log"; exit 1; }
  rm -f "$BASH_SRC/config.h"
fi

echo "[+] Generating builtin .c files from .def"
if [ ! -f "$BASH_SRC/builtins/mkbuiltins" ]; then
  # mkbuiltins runs on the HOST — give it a minimal config.h, not our full shim set
  _mkb_tmp=$(mktemp -d)
  cat > "$_mkb_tmp/config.h" << 'MKCFG'
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_LIMITS_H 1
MKCFG
  gcc -DHAVE_CONFIG_H -DHAVE_RENAME -I"$_mkb_tmp" -I"$BASH_SRC" -I"$BASH_SRC/include" \
    -o "$BASH_SRC/builtins/mkbuiltins" "$BASH_SRC/builtins/mkbuiltins.c"
  rm -rf "$_mkb_tmp"
fi
(cd "$BASH_SRC/builtins" && ./mkbuiltins \
    -externfile builtext.h -includefile builtext.h \
    -structfile builtins.c *.def) 2>/dev/null || true

echo "[+] Generating syntax.c"
if [ ! -f "$BASH_SRC/mksyntax" ]; then
  _mkb_tmp=$(mktemp -d)
  cat > "$_mkb_tmp/config.h" << 'MKCFG'
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
MKCFG
  gcc -DHAVE_CONFIG_H -I"$_mkb_tmp" -I"$BASH_SRC" -I"$BASH_SRC/include" \
    -o "$BASH_SRC/mksyntax" "$BASH_SRC/mksyntax.c" 2>/dev/null
  rm -rf "$_mkb_tmp"
fi
[ ! -f "$BASH_SRC/syntax.c" ] && (cd "$BASH_SRC" && ./mksyntax -o syntax.c)

rm -f "$OBJ_DIR"/*.o

echo "[+] Compiling bash main sources"
errors=0
compile_dir() {
    local dir="$1" prefix="$2"
    shift 2
    local extra_excl=("$@")
    for f in "$dir"/*.c; do
        [[ ! -f "$f" ]] && continue
        local name=$(basename "$f" .c)
        local skip=0
        for ex in "${EXCLUDE[@]}"; do [[ "$name.c" == "$ex" ]] && skip=1 && break; done
        for ex in "${extra_excl[@]}"; do [[ "$name.c" == "$ex" ]] && skip=1 && break; done
        [[ $skip -eq 1 ]] && continue
        "$CC" "${CFLAGS[@]}" -c "$f" -o "$OBJ_DIR/${prefix}${name}.o" 2>/dev/null \
            || errors=$((errors+1))
    done
}
compile_dir "$BASH_SRC"               ""
compile_dir "$BASH_SRC/lib/sh"        "sh_"
compile_dir "$BASH_SRC/lib/readline"  "rl_"  "${RL_EXCLUDE[@]}"
compile_dir "$BASH_SRC/lib/glob"      "gl_"
# tilde is already in readline; skip standalone lib/tilde to avoid dupes

echo "[+] Compiling builtins"
for f in "$BASH_SRC"/builtins/alias.c "$BASH_SRC"/builtins/bind.c \
          "$BASH_SRC"/builtins/break.c "$BASH_SRC"/builtins/builtin.c \
          "$BASH_SRC"/builtins/caller.c "$BASH_SRC"/builtins/cd.c \
          "$BASH_SRC"/builtins/colon.c "$BASH_SRC"/builtins/command.c \
          "$BASH_SRC"/builtins/declare.c \
          "$BASH_SRC"/builtins/echo.c "$BASH_SRC"/builtins/enable.c \
          "$BASH_SRC"/builtins/eval.c "$BASH_SRC"/builtins/exec.c \
          "$BASH_SRC"/builtins/exit.c "$BASH_SRC"/builtins/fc.c \
          "$BASH_SRC"/builtins/fg_bg.c "$BASH_SRC"/builtins/getopts.c \
          "$BASH_SRC"/builtins/hash.c "$BASH_SRC"/builtins/help.c \
          "$BASH_SRC"/builtins/history.c "$BASH_SRC"/builtins/jobs.c \
          "$BASH_SRC"/builtins/kill.c "$BASH_SRC"/builtins/let.c \
          "$BASH_SRC"/builtins/printf.c "$BASH_SRC"/builtins/pushd.c \
          "$BASH_SRC"/builtins/read.c "$BASH_SRC"/builtins/return.c \
          "$BASH_SRC"/builtins/set.c "$BASH_SRC"/builtins/setattr.c \
          "$BASH_SRC"/builtins/shift.c "$BASH_SRC"/builtins/shopt.c \
          "$BASH_SRC"/builtins/source.c "$BASH_SRC"/builtins/suspend.c \
          "$BASH_SRC"/builtins/test.c "$BASH_SRC"/builtins/times.c \
          "$BASH_SRC"/builtins/trap.c "$BASH_SRC"/builtins/type.c \
          "$BASH_SRC"/builtins/ulimit.c "$BASH_SRC"/builtins/umask.c \
          "$BASH_SRC"/builtins/wait.c "$BASH_SRC"/builtins/common.c \
          "$BASH_SRC"/builtins/evalfile.c "$BASH_SRC"/builtins/evalstring.c \
          "$BASH_SRC"/builtins/bashgetopt.c "$BASH_SRC"/builtins/getopt.c \
          "$BASH_SRC"/builtins/builtins.c; do
    [[ ! -f "$f" ]] && continue
    name="def_$(basename "$f" .c)"
    "$CC" "${BUILTIN_CFLAGS[@]}" -c "$f" -o "$OBJ_DIR/${name}.o" 2>/dev/null \
      || errors=$((errors+1))
done

# mapfile stub (complex dependencies)
cat > /tmp/_mapfile_stub.c << 'EOF'
typedef struct word_list WORD_LIST;
int mapfile_builtin(WORD_LIST *l) { (void)l; return 1; }
EOF
"$CC" -O2 -ffreestanding -nostdinc -nostdlib -fno-stack-protector -fno-builtin \
  -I"$SHIM_INC" -I"$LIBC" -DMAKAOS \
  -c /tmp/_mapfile_stub.c -o "$OBJ_DIR/def_mapfile.o" 2>/dev/null

# misc stubs (termcap, random, progcomp, etc.)
"$CC" -O2 -ffreestanding -nostdinc -nostdlib -fno-stack-protector -fno-builtin \
  -I"$SHIM_INC" -I"$LIBC" -DMAKAOS \
  -c "$STUBS_DIR/bash_stubs.c" -o "$OBJ_DIR/bash_stubs.o"

if [ "$errors" -gt 0 ]; then
  echo "[!] $errors bash source file(s) failed to compile (errors suppressed; rerun one by hand to see them)"
fi

# Link against the real MakaOS libc that build.sh puts in the sysroot (crt0 +
# libc.a), not a private copy of a few libc files: libc is split across many
# objects (unistd.c, tls.c, ...) and a partial copy leaves symbols unresolved.
SYSROOT_LIB="$BUILD/sysroot/usr/lib"
if [ ! -f "$SYSROOT_LIB/libc.a" ] || [ ! -f "$SYSROOT_LIB/crt0.o" ]; then
  echo "[!] $SYSROOT_LIB/libc.a or crt0.o missing: run build.sh first"
  exit 1
fi

echo "[+] Linking bash.elf"
"$LD" -T "$USERLAND/link.ld" \
  "$SYSROOT_LIB/crt0.o" \
  "$OBJ_DIR"/*.o \
  --start-group "$SYSROOT_LIB/libc.a" --end-group \
  -o "$BUILD/user_bash.elf"

# Standalone use: refresh an existing image.  build.sh installs /bin/bash
# itself when it assembles the image, so nothing to do if there is none yet.
if [ -f "$BUILD/ext2.img" ] && [ -f "$BUILD/disk.img" ]; then
  echo "[+] Installing bash to disk image"
  debugfs -w "$BUILD/ext2.img" -R "write $BUILD/user_bash.elf bin/bash" >/dev/null 2>&1
  debugfs -w "$BUILD/ext2.img" -R "set_inode_field /bin/bash mode 0100755" >/dev/null 2>&1
  debugfs -w "$BUILD/ext2.img" -R "set_inode_field /bin/bash uid 0" >/dev/null 2>&1
  debugfs -w "$BUILD/ext2.img" -R "set_inode_field /bin/bash gid 0" >/dev/null 2>&1
  dd if="$BUILD/ext2.img" of="$BUILD/disk.img" bs=512 seek=4096 conv=notrunc status=none
fi

echo "[+] bash built and installed: $(stat -c '%s' "$BUILD/user_bash.elf") bytes"
