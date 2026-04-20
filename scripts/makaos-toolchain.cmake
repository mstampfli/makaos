# ── MakaOS CMake toolchain file ───────────────────────────────────────
#
# Use with any upstream CMake project to cross-compile into the MakaOS
# sysroot:
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=$REPO/scripts/makaos-toolchain.cmake \
#         -DCMAKE_INSTALL_PREFIX=$SYSROOT/usr \
#         -S source-dir -B build-dir
#   cmake --build build-dir
#   cmake --install build-dir
#
# The cross-gcc already knows its target triple, sysroot, crt0, link
# script, and libgcc path, so CMake just needs to be pointed at it
# and told to stop searching host paths.

# Require either REPO_ROOT env var or the caller to set CMAKE_SOURCE_DIR
# relative to the toolchain file's location.
if(DEFINED ENV{REPO_ROOT})
    set(_MAKAOS_REPO "$ENV{REPO_ROOT}")
else()
    get_filename_component(_MAKAOS_REPO "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(CMAKE_SYSTEM_NAME      MakaOS)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_SYSROOT          "${_MAKAOS_REPO}/build/sysroot")

set(CMAKE_C_COMPILER       "${_MAKAOS_REPO}/toolchain/bin/x86_64-pc-makaos-gcc")
set(CMAKE_CXX_COMPILER     "${_MAKAOS_REPO}/toolchain/bin/x86_64-pc-makaos-g++")
set(CMAKE_AR               "${_MAKAOS_REPO}/toolchain/bin/x86_64-pc-makaos-ar")
set(CMAKE_RANLIB           "${_MAKAOS_REPO}/toolchain/bin/x86_64-pc-makaos-ranlib")
set(CMAKE_STRIP            "${_MAKAOS_REPO}/toolchain/bin/x86_64-pc-makaos-strip")
set(CMAKE_OBJCOPY          "${_MAKAOS_REPO}/toolchain/bin/x86_64-pc-makaos-objcopy")

# Don't search host paths for libraries or headers — only the sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# MakaOS ships no dynamic linker; static link everything.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static")

# Upstream projects that run compile-tests during configure break
# when the result can't execute (it's a cross-compile).  Tell CMake
# to skip those.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
