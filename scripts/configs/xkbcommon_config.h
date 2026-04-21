// Hand-written config.h for libxkbcommon, replacing the meson-
// generated one.  x86_64 / GCC / POSIX-ish libc.

#pragma once

#define PACKAGE                  "xkbcommon"
#define PACKAGE_NAME             "xkbcommon"
#define PACKAGE_VERSION          "1.7.0"
#define VERSION                  "1.7.0"

#define HAVE_UNISTD_H            1
#define HAVE_STDINT_H            1
#define HAVE_STRINGS_H           1
#define HAVE_SYS_TYPES_H         1
#define HAVE_SYS_STAT_H          1
#define HAVE_SYS_MMAN_H          1
#define HAVE_FCNTL_H             1
#define HAVE_DLFCN_H             1
#define HAVE_INTTYPES_H          1

#define HAVE_STRNDUP             1
#define HAVE_STRDUP              1
#define HAVE_STRCASECMP          1
#define HAVE_STRNCASECMP         1
// Features we DON'T have — left undefined because xkb uses
// `#if defined(HAVE_X)` which is true regardless of value:
//   HAVE_VASPRINTF, HAVE_ASPRINTF, HAVE_OPEN_MEMSTREAM, HAVE_MKSTEMP,
//   HAVE_EACCESS, HAVE_EUIDACCESS, HAVE_SECURE_GETENV, HAVE___SECURE_GETENV
#define HAVE_MMAP                1
#define HAVE___BUILTIN_EXPECT    1

// Defaults — passed via -D in the port script as well, here for
// symbol-completeness when xkb headers are reincluded indirectly.
#ifndef DFLT_XKB_CONFIG_ROOT
# define DFLT_XKB_CONFIG_ROOT    "/usr/share/X11/xkb"
#endif
#ifndef DEFAULT_XKB_RULES
# define DEFAULT_XKB_RULES       "evdev"
#endif
#ifndef DEFAULT_XKB_MODEL
# define DEFAULT_XKB_MODEL       "pc105"
#endif
#ifndef DEFAULT_XKB_LAYOUT
# define DEFAULT_XKB_LAYOUT      "us"
#endif
#ifndef DFLT_XKB_CONFIG_EXTRA_PATH
# define DFLT_XKB_CONFIG_EXTRA_PATH ""
#endif
#ifndef DEFAULT_XKB_VARIANT
# define DEFAULT_XKB_VARIANT     ""
#endif
#ifndef DEFAULT_XKB_OPTIONS
# define DEFAULT_XKB_OPTIONS     ""
#endif
#ifndef XLOCALEDIR
# define XLOCALEDIR              "/usr/share/X11/locale"
#endif

// xkbcommon-specific build options.
#define XKBCOMMON_VERSION        "1.7.0"
