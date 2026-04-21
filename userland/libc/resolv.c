// ── resolv.c — BSD resolver() API stubs ─────────────────────────────
//
// GLib's gio feature-detects these at configure time.  MakaOS uses
// its own DNS resolver (libc/dns.c) driven by gethostbyname/
// getaddrinfo rather than the low-level BIND API.  The stubs here
// exist so the link succeeds; every call reports "no answer."
// TODO(scalability-debt-ledger): when a port actually requires direct
// DNS RR queries, route through the libc/dns.c path.

#include <resolv.h>
#include <errno.h>

struct __res_state _res;

int res_init(void)                                  { return 0; }
int res_query(const char* dname, int klass, int type,
              unsigned char* answer, int anslen) {
    (void)dname; (void)klass; (void)type;
    (void)answer; (void)anslen;
    errno = ENOSYS;
    return -1;
}
int res_search(const char* dname, int klass, int type,
               unsigned char* answer, int anslen) {
    return res_query(dname, klass, type, answer, anslen);
}
void res_close(void) { }
