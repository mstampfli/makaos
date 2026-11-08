// sys/param.h -- historical BSD/Unix system parameter macros.
// Minimal but standard: the bits portable software (Mesa's loader, many
// others) actually pulls in -- path limits, MIN/MAX, and the roundup family.
#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H

#include <limits.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif
#ifndef NBBY
#define NBBY 8              // bits per byte
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define howmany(x, y)  (((x) + ((y) - 1)) / (y))
#define roundup(x, y)  ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) (((x) / (y)) * (y))
#define powerof2(x)    ((((x) - 1) & (x)) == 0)

#endif // _SYS_PARAM_H
