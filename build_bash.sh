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
OBJ_DIR="/tmp/bash_build_objs"
RT_DIR="/tmp/bash_build_rt"

mkdir -p "$OBJ_DIR" "$RT_DIR" "$STUBS_DIR"

CC=x86_64-linux-gnu-gcc
LD=x86_64-linux-gnu-ld

CFLAGS=(
  -O2 -ffreestanding -nostdinc -nostdlib
  -fno-stack-protector -fno-builtin
  -I/tmp/bash-makaos-include
  -I"$BASH_SRC" -I"$BASH_SRC/include" -I"$BASH_SRC/lib"
  -I"$LIBC"
  -DMAKAOS -DHAVE_CONFIG_H -DSHELL -DPREFER_STDARG
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

EXCLUDE=(array2.c gen-helpfiles.c glob_loop.c gm_loop.c sm_loop.c getenv.c
  netconn.c netopen.c fnxform.c unicode.c random.c emacs_keymap.c vi_keymap.c
  mkbuiltins.c mksyntax.c nojobs.c memset.c strcasestr.c strchrnul.c strdup.c
  strftime.c strstr.c)

echo "[+] Generating builtin .c files from .def"
if [ ! -f "$BASH_SRC/builtins/mkbuiltins" ]; then
  gcc -DHAVE_CONFIG_H -I. -I.. -I../include \
    -o "$BASH_SRC/builtins/mkbuiltins" "$BASH_SRC/builtins/mkbuiltins.c"
fi
(cd "$BASH_SRC/builtins" && ./mkbuiltins -externfile builtins.h -structfile builtins.c *.def) 2>/dev/null || true

rm -f "$OBJ_DIR"/*.o

echo "[+] Compiling bash main sources"
errors=0
for f in "$BASH_SRC"/*.c "$BASH_SRC"/lib/sh/*.c \
          "$BASH_SRC"/lib/readline/*.c "$BASH_SRC"/lib/glob/*.c \
          "$BASH_SRC"/lib/tilde/*.c; do
    [[ ! -f "$f" ]] && continue
    name=$(basename "$f" .c)
    skip=0
    for ex in "${EXCLUDE[@]}"; do [[ "$name.c" == "$ex" ]] && skip=1 && break; done
    [[ $skip -eq 1 ]] && continue
    "$CC" "${CFLAGS[@]}" -c "$f" -o "$OBJ_DIR/${name}.o" 2>/dev/null || errors=$((errors+1))
done
# Recompile bash's shell.c and input.c with unique names to avoid readline conflicts
"$CC" "${CFLAGS[@]}" -c "$BASH_SRC/input.c" -o "$OBJ_DIR/bash_input.o"

echo "[+] Compiling builtins"
for f in "$BASH_SRC"/builtins/alias.c "$BASH_SRC"/builtins/bind.c \
          "$BASH_SRC"/builtins/break.c "$BASH_SRC"/builtins/builtin.c \
          "$BASH_SRC"/builtins/caller.c "$BASH_SRC"/builtins/cd.c \
          "$BASH_SRC"/builtins/colon.c "$BASH_SRC"/builtins/command.c \
          "$BASH_SRC"/builtins/complete.c "$BASH_SRC"/builtins/declare.c \
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
  -I/tmp/bash-makaos-include -I"$LIBC" -DMAKAOS \
  -c /tmp/_mapfile_stub.c -o "$OBJ_DIR/def_mapfile.o" 2>/dev/null

# misc stubs (termcap, random, progcomp, etc.)
"$CC" -O2 -ffreestanding -nostdinc -nostdlib -fno-stack-protector -fno-builtin \
  -I/tmp/bash-makaos-include -I"$LIBC" -DMAKAOS \
  -c "$STUBS_DIR/bash_stubs.c" -o "$OBJ_DIR/bash_stubs.o"

echo "[+] Building runtime objects"
"$CC" "${CFLAGS[@]}" -c "$LIBC/libc.c"  -o "$RT_DIR/_libc.o"
"$CC" "${CFLAGS[@]}" -c "$LIBC/stdio.c" -o "$RT_DIR/_stdio.o"
"$CC" "${CFLAGS[@]}" -c "$LIBC/math.c"  -o "$RT_DIR/_math.o"
nasm -f elf64 "$ENTRY/entry.asm" -o "$RT_DIR/_entry.o"
nasm -f elf64 "$LIBC/setjmp.asm" -o "$RT_DIR/_setjmp.o"

echo "[+] Linking bash.elf"
"$LD" -T "$USERLAND/link.ld" \
  "$RT_DIR/_entry.o" "$RT_DIR/_libc.o" "$RT_DIR/_stdio.o" \
  "$RT_DIR/_math.o" "$RT_DIR/_setjmp.o" \
  "$OBJ_DIR"/*.o \
  -o "$BUILD/user_bash.elf"

echo "[+] Installing bash to disk image"
debugfs -w "$BUILD/ext2.img" -R "write $BUILD/user_bash.elf bin/bash" >/dev/null 2>&1
debugfs -w "$BUILD/ext2.img" -R "set_inode_field /bin/bash mode 0100755" >/dev/null 2>&1
debugfs -w "$BUILD/ext2.img" -R "set_inode_field /bin/bash uid 0" >/dev/null 2>&1
debugfs -w "$BUILD/ext2.img" -R "set_inode_field /bin/bash gid 0" >/dev/null 2>&1
dd if="$BUILD/ext2.img" of="$BUILD/disk.img" bs=512 seek=4096 conv=notrunc status=none

echo "[+] bash built and installed: $(stat -c '%s' "$BUILD/user_bash.elf") bytes"
