#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USERLAND_DIR="userland"
BUILD_DIR="build"

mkdir -p "$BUILD_DIR"

CC="gcc"
NASM="nasm"

# ── Shared user runtime (must match build.sh) ────────────────────────────

USER_CFLAGS=(
  -ffreestanding -m64 -mno-red-zone
  -fno-pie -fno-pic -fno-plt
  -fno-stack-protector -fno-builtin
  -fno-asynchronous-unwind-tables -fno-unwind-tables
  -O0 -Wall -Wextra
  -Wno-unused-parameter
)

USER_INCLUDES=(
  -I "$USERLAND_DIR/libc"
  -I "$USERLAND_DIR/include"
)

USER_LINK="$USERLAND_DIR/link.ld"

# Build user runtime objects if they don't exist yet
if [ ! -f "$BUILD_DIR/user_entry.o" ]; then
    "$NASM" -f elf64 "$USERLAND_DIR/entry/entry.asm"    -o "$BUILD_DIR/user_entry.o"
fi
if [ ! -f "$BUILD_DIR/user_setjmp.o" ]; then
    "$NASM" -f elf64 "$USERLAND_DIR/libc/setjmp.asm"    -o "$BUILD_DIR/user_setjmp.o"
fi
if [ ! -f "$BUILD_DIR/user_libc.o" ]; then
    "$CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/libc/libc.c"  -o "$BUILD_DIR/user_libc.o"
fi
if [ ! -f "$BUILD_DIR/user_stdio.o" ]; then
    "$CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -c "$USERLAND_DIR/libc/stdio.c" -o "$BUILD_DIR/user_stdio.o"
fi
if [ ! -f "$BUILD_DIR/user_math.o" ]; then
    "$CC" "${USER_CFLAGS[@]}" "${USER_INCLUDES[@]}" -msse2 -c "$USERLAND_DIR/libc/math.c" -o "$BUILD_DIR/user_math.o"
fi

USER_RT=(
    "$BUILD_DIR/user_entry.o"
    "$BUILD_DIR/user_libc.o"
    "$BUILD_DIR/user_stdio.o"
    "$BUILD_DIR/user_math.o"
    "$BUILD_DIR/user_setjmp.o"
)

# ── Doom (doomgeneric) ───────────────────────────────────────────────────

DOOM_DIR="$USERLAND_DIR/apps/doom"
DOOMGENERIC_DIR="$DOOM_DIR/doomgeneric"

if [ ! -d "$DOOMGENERIC_DIR" ] || [ -z "$(ls -A "$DOOMGENERIC_DIR" 2>/dev/null)" ]; then
    echo "[+] Initialising doomgeneric submodule..."
    git -c http.sslVerify=false submodule update --init || \
        echo "[!] submodule init failed — place doomgeneric at $DOOMGENERIC_DIR manually"
fi

if [ ! -d "$DOOMGENERIC_DIR" ] || [ -z "$(ls -A "$DOOMGENERIC_DIR" 2>/dev/null)" ]; then
    echo "[!] doomgeneric not found — cannot build doom"
    exit 1
fi

echo "[+] Building doom"

DOOM_CFLAGS=(
    -ffreestanding -m64 -mno-red-zone -msse2
    -fno-pie -fno-pic -fno-plt
    -fno-stack-protector -fno-builtin
    -fno-asynchronous-unwind-tables -fno-unwind-tables
    -O1 -g
    -DFEATURE_SOUND
    -DMAKAOS_NO_SDL_MIXER
    -nostdinc
    -I"$DOOM_DIR/include"
    -I"$DOOMGENERIC_DIR/doomgeneric"
    -I"$USERLAND_DIR/libc"
    -Wno-implicit-function-declaration
    -Wno-implicit-int
    -Wno-int-conversion
    -Wno-pointer-sign
    -Wno-missing-field-initializers
    -Wno-char-subscripts
    -Wno-sign-compare
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-pointer-to-int-cast
    -Wno-macro-redefined
)

DOOM_OBJS=()

for src in "$DOOMGENERIC_DIR/doomgeneric/"*.c; do
    base=$(basename "$src")
    case "$base" in
        doomgeneric_*.c) continue ;;
        i_sdlmusic.c|i_sdlsound.c) continue ;;
        i_allegromusic.c|i_allegrosound.c|i_cdmus.c) continue ;;
    esac
    obj="$BUILD_DIR/doom_${base%.c}.o"
    "$CC" "${DOOM_CFLAGS[@]}" -c "$src" -o "$obj"
    DOOM_OBJS+=("$obj")
done

"$CC" "${DOOM_CFLAGS[@]}" -c "$DOOM_DIR/doomgeneric_makaos.c" -o "$BUILD_DIR/doom_makaos.o"
DOOM_OBJS+=("$BUILD_DIR/doom_makaos.o")
"$CC" "${DOOM_CFLAGS[@]}" -c "$DOOM_DIR/i_sound_makaos.c" -o "$BUILD_DIR/doom_i_sound_makaos.o"
DOOM_OBJS+=("$BUILD_DIR/doom_i_sound_makaos.o")

ld -nostdlib -T "$USER_LINK" \
   "${USER_RT[@]}" \
   "${DOOM_OBJS[@]}" \
   -o "$BUILD_DIR/user_doom.elf"

echo "[+] Doom ELF built: $(stat -c '%s' "$BUILD_DIR/user_doom.elf") bytes"
