#ifndef _FEATURES_H
#define _FEATURES_H 1

// Minimal <features.h> for the MakaOS sysroot.  MakaOS presents a glibc-
// compatible ABI.  This exists so the HOST libstdc++ headers -- used when
// cross-building C++ with host g++ + --sysroot=makaos (e.g. OpenJDK HotSpot's
// x86 C1/C2 code generators) -- find the glibc feature-test macros they expect.
// libstdc++'s os_defines.h only needs the version macros + __GLIBC_PREREQ.

#define __GLIBC__        2
#define __GLIBC_MINOR__  31
#define __GNU_LIBRARY__  6

#define __GLIBC_PREREQ(maj, min) \
    ((__GLIBC__ << 16) + __GLIBC_MINOR__ >= ((maj) << 16) + (min))

// A modern glibc would set these from the requested feature level; MakaOS's own
// headers do not gate on them, so a permissive default is safe here.
#ifndef _DEFAULT_SOURCE
# define _DEFAULT_SOURCE 1
#endif

#endif /* _FEATURES_H */
