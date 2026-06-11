#ifndef _MAKAOS_NETDB_H
#define _MAKAOS_NETDB_H 1

#include <sys/socket.h>
#include <netinet/in.h>

// Legacy resolver error reporting
extern int h_errno;
#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4

int getnameinfo(const struct sockaddr* sa, socklen_t salen,
                char* host, socklen_t hostlen,
                char* serv, socklen_t servlen, int flags);

// getaddrinfo hint flags
#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_V4MAPPED    0x0008
#define AI_ALL         0x0010
#define AI_ADDRCONFIG  0x0020
#define AI_NUMERICSERV 0x0400

// getnameinfo flags
#define NI_NUMERICHOST 0x0001
#define NI_NUMERICSERV 0x0002
#define NI_NOFQDN      0x0004
#define NI_NAMEREQD    0x0008
#define NI_DGRAM       0x0010
#define NI_MAXHOST     1025
#define NI_MAXSERV     32

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

// Service-name lookup — no /etc/services database on MakaOS; always
// returns NULL (callers fall back to numeric ports).
struct servent {
    char*  s_name;
    char** s_aliases;
    int    s_port;
    char*  s_proto;
};
struct servent* getservbyname(const char* name, const char* proto);
struct servent* getservbyport(int port, const char* proto);

// MakaOS-native lightweight resolver (what dns.c implements).
int gethostbyname_ipv4(const char* name, uint32_t* out_ip_be);
int getaddrinfo_ipv4(const char* host, uint16_t port,
                      struct sockaddr_in* out);

#endif
