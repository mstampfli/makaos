// ── net/if.h — network interface naming ─────────────────────────────
// Port-surface header (glib's gnetworking.h includes it).  MakaOS has
// exactly one interface (virtio-net "eth0"); the index/name mapping is
// fixed and the ioctl-based enumeration APIs are not provided.

#ifndef _MAKAOS_NET_IF_H
#define _MAKAOS_NET_IF_H 1

#define IF_NAMESIZE 16
#define IFNAMSIZ    IF_NAMESIZE

unsigned int if_nametoindex(const char* name);
char*        if_indextoname(unsigned int index, char* name);

#endif
