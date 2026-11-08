// ifaddrs.c - <ifaddrs.h>.
//
// MakaOS has no interface-address enumeration facility yet, so getifaddrs()
// succeeds with an empty list. Callers iterate nothing and degrade gracefully
// (e.g. OpenJDK reports no per-interface network counters). TODO: back with a
// real netlink/RTM_GETADDR query when the network stack exposes one.

#include <ifaddrs.h>
#include <stddef.h>

int getifaddrs(struct ifaddrs** ifap) {
    if (ifap) *ifap = NULL;
    return 0;
}

void freeifaddrs(struct ifaddrs* ifa) {
    (void)ifa;
}
