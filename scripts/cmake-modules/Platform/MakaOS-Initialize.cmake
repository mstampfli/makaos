# ── MakaOS — CMake platform init (CMake 3.27+) ───────────────────────
#
# Runs BEFORE CMakeSystemSpecificInitialize / language detection.
# Job: set the coarse CMake platform flags so cross-project checks
# (UNIX=TRUE, MAKAOS=TRUE) see correct values early enough.  The
# heavier-weight configuration (search paths, link flags) lives in
# MakaOS.cmake which runs later.

set(MAKAOS 1)
set(UNIX 1)

# No multi-arch library directory naming on MakaOS — the sysroot
# uses a single /usr/lib tree.  (Linux-Initialize sets a regex here
# for /usr/lib/x86_64-linux-gnu and friends.)
set(CMAKE_LIBRARY_ARCHITECTURE_REGEX "")
