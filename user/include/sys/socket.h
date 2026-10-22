/* sys/socket.h - BSD-ish sockets over the VibeOS net syscalls (IPv4 only).
 * Addresses and ports are in network byte order. */
#pragma once
#include "abi.h"
#include <stdint.h>
#include <sys/types.h>

typedef uint32_t socklen_t;
struct sockaddr { uint16_t sa_family; char sa_data[14]; };

#define htons(x) __builtin_bswap16(x)
#define ntohs(x) __builtin_bswap16(x)
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)

int socket(int domain, int type, int proto);
int connect(int fd, const struct sockaddr *a, socklen_t len);
int bind(int fd, const struct sockaddr *a, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *a, socklen_t *len);     /* a gets a sockaddr_in, len may be NULL */
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to, socklen_t len);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *len);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);

int netinfo(struct netinfo *ni);
int resolve(const char *name, uint32_t *ip_be);      /* hostname or dotted quad; 0 / -1 + errno */
int inet_aton(const char *s, uint32_t *ip_be);       /* 1 on success */
char *inet_ntoa_r(uint32_t ip_be, char buf[16]);
