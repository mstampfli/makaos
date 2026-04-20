#pragma once
// ── Connection abstraction: HTTP (plain TCP) or HTTPS (TLS over TCP) ──
//
// One conn_t wraps either form.  Callers use conn_send / conn_recv /
// conn_close and don't care which protocol is underneath.  Lets the
// bulk of http_get.c stay protocol-agnostic.

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct conn_t conn_t;

// Open a connection.  `host` is a hostname (DNS resolved internally);
// `port` is host-byte-order.  If `use_tls` is non-zero, performs a TLS
// handshake after the TCP connect; SNI is set from `host`.  When
// `insecure` is non-zero, the server certificate chain isn't verified
// (curl -k style — useful before we ship a CA bundle).
//
// Returns a heap-allocated conn_t on success; caller frees via
// conn_close().  NULL on failure with a short stderr message.
conn_t* conn_open(const char* host, uint16_t port,
                  int use_tls, int insecure, int verbose);

// Blocking send / recv.  Return negative on error, 0 on EOF (recv).
// Short reads are allowed — caller loops as needed.
ssize_t conn_send(conn_t* c, const void* buf, size_t len);
ssize_t conn_recv(conn_t* c, void* buf, size_t len);

// Tear down.  Sends TLS close_notify if is_tls.  Closes the TCP socket.
void conn_close(conn_t* c);
