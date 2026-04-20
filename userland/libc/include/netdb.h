#ifndef _MAKAOS_NETDB_H
#define _MAKAOS_NETDB_H 1

#include <sys/socket.h>
#include <netinet/in.h>

// getaddrinfo error codes
#define EAI_BADFLAGS   -1
#define EAI_NONAME     -2
#define EAI_AGAIN      -3
#define EAI_FAIL       -4
#define EAI_FAMILY     -6
#define EAI_SOCKTYPE   -7
#define EAI_SERVICE    -8
#define EAI_MEMORY     -10
#define EAI_SYSTEM     -11

struct addrinfo {
    int               ai_flags;
    int               ai_family;
    int               ai_socktype;
    int               ai_protocol;
    socklen_t         ai_addrlen;
    struct sockaddr*  ai_addr;
    char*             ai_canonname;
    struct addrinfo*  ai_next;
};

int  getaddrinfo(const char* node, const char* svc,
                  const struct addrinfo* hints, struct addrinfo** res);
void freeaddrinfo(struct addrinfo* ai);
const char* gai_strerror(int err);

// MakaOS-native lightweight resolver (what dns.c implements).
int gethostbyname_ipv4(const char* name, uint32_t* out_ip_be);
int getaddrinfo_ipv4(const char* host, uint16_t port,
                      struct sockaddr_in* out);

#endif
