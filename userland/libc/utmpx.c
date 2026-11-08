// utmpx.c - <utmpx.h>.
//
// MakaOS keeps no utmp accounting database, so the database is always empty:
// setutxent/endutxent are no-ops and getutxent() returns NULL immediately.
// Callers scanning for a record (e.g. OpenJDK's OS-uptime lookup) find nothing
// and skip. TODO: back with a real login/boot record source if one appears.

#include <utmpx.h>
#include <stddef.h>

void setutxent(void) {}
void endutxent(void) {}

struct utmpx* getutxent(void) {
    return NULL;
}
