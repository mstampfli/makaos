# ── MakaOS — CMake platform description ──────────────────────────────
#
# MakaOS is a POSIX-ish static-only OS: syscall layer is
# Linux-uAPI-compatible (evdev, epoll, io_uring, timerfd, signalfd),
# but there is no dynamic linker, no shared libraries on the default
# sysroot, and no /proc/self/exe.  This file gives CMake the right
# mental model for that — identifies us as UNIX + pulls in the
# standard UNIX search paths, then overrides the shared-library /
# rpath machinery since it doesn't apply.

include(Platform/UnixPaths)

# Static-only world: no dlopen surface on the cross-gcc sysroot.
# (Kernel has a dynamic-loading path for ELF interp, but the libc
# the toolchain links against does not expose dlopen to apps yet.)
set(CMAKE_DL_LIBS "")

# We build static archives exclusively; shared-library handling is
# inert.  Without these overrides CMake still tries to emit -soname
# / --export-dynamic / -Wl,-rpath flags, which our linker doesn't
# accept.
set(CMAKE_SHARED_LIBRARY_C_FLAGS         "")
set(CMAKE_SHARED_LIBRARY_CXX_FLAGS       "")
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS    "")
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS  "")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG  "")
set(CMAKE_SHARED_LIBRARY_RUNTIME_CXX_FLAG "")
set(CMAKE_SHARED_LIBRARY_RPATH_LINK_C_FLAG "")
set(CMAKE_SHARED_LIBRARY_SONAME_C_FLAG   "")
set(CMAKE_SHARED_LIBRARY_SONAME_CXX_FLAG "")
set(CMAKE_EXE_EXPORTS_C_FLAG             "")
set(CMAKE_EXE_EXPORTS_CXX_FLAG           "")

# find_library should return .a, not .so.
set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
