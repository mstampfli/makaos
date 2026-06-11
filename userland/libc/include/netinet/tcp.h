// ── netinet/tcp.h — TCP socket options ───────────────────────────────
// Port-surface header (glib's gnetworking.h includes it).  The MakaOS
// TCP stack ignores unknown setsockopt options, so consumers can set
// these freely; only TCP_NODELAY has meaning today (and the stack does
// not batch sends, so it is effectively always on).

#ifndef _MAKAOS_NETINET_TCP_H
#define _MAKAOS_NETINET_TCP_H 1

#define TCP_NODELAY      1
#define TCP_MAXSEG       2
#define TCP_KEEPIDLE     4
#define TCP_KEEPINTVL    5
#define TCP_KEEPCNT      6

#endif
