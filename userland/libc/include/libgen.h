#ifndef _MAKAOS_LIBGEN_H
#define _MAKAOS_LIBGEN_H 1

// POSIX basename / dirname.  Both modify the passed buffer on Linux;
// our impls match that contract so libinput's path-manipulation calls
// keep semantics.  Returns a pointer into `path` or a static string
// for edge cases ("." / "/").

char* basename(char* path);
char* dirname (char* path);

#endif
