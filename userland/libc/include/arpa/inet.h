#ifndef _MAKAOS_ARPA_INET_H
#define _MAKAOS_ARPA_INET_H 1

#include <netinet/in.h>

uint16_t htons(uint16_t v);
uint16_t ntohs(uint16_t v);
uint32_t htonl(uint32_t v);
uint32_t ntohl(uint32_t v);

in_addr_t inet_addr(const char* s);
char*     inet_ntoa(struct in_addr in);
int       inet_pton(int af, const char* s, void* dst);
const char* inet_ntop(int af, const void* src, char* dst, socklen_t size);

#endif
