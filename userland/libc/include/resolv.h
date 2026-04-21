#ifndef _MAKAOS_RESOLV_H
#define _MAKAOS_RESOLV_H 1

#include <arpa/nameser.h>

// BIND resolver API subset.  GLib's gio probes for res_query() at
// build time; the real DNS path goes through our own resolver in
// libc/dns.c.  These exist so feature detection passes.

struct __res_state { int _opaque[128]; };
extern struct __res_state _res;

int res_init(void);
int res_query(const char* dname, int klass, int type,
              unsigned char* answer, int anslen);
int res_search(const char* dname, int klass, int type,
               unsigned char* answer, int anslen);
void res_close(void);

// Convenience — some callers use the non-__ prefixed name.
typedef struct __res_state res_state_t;

#endif
