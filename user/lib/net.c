/* net.c - socket wrappers and address helpers */
#include <syscall.h>
#include <sys/socket.h>

#define P(x) ((int)(uintptr_t)(x))

int socket(int domain, int type, int proto) { return __sysret(__syscall3(SYS_socket, domain, type, proto)); }
int connect(int fd, const struct sockaddr *a, socklen_t len) { return __sysret(__syscall3(SYS_connect, fd, P(a), len)); }
int bind(int fd, const struct sockaddr *a, socklen_t len) { return __sysret(__syscall3(SYS_bind, fd, P(a), len)); }
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to, socklen_t len) {
    return __sysret(__syscall5(SYS_sendto, fd, P(buf), n, flags, P(to)));
}
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *len) {
    int r = __sysret(__syscall5(SYS_recvfrom, fd, P(buf), n, flags, P(from)));
    if (r >= 0 && from && len) *len = sizeof(struct sockaddr_in);
    return r;
}
ssize_t send(int fd, const void *buf, size_t n, int flags) { return sendto(fd, buf, n, flags, 0, 0); }
ssize_t recv(int fd, void *buf, size_t n, int flags) { return recvfrom(fd, buf, n, flags, 0, 0); }
int netinfo(struct netinfo *ni) { return __sysret(__syscall1(SYS_netinfo, P(ni))); }
int resolve(const char *name, uint32_t *ip) { return __sysret(__syscall2(SYS_resolve, P(name), P(ip))); }

int inet_aton(const char *s, uint32_t *ip) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + *s++ - '0'; if (v > 255) return 0; }
        b[i] = v;
        if (i < 3 && *s++ != '.') return 0;
    }
    if (*s) return 0;
    *ip = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    return 1;
}

char *inet_ntoa_r(uint32_t ip, char buf[16]) {
    char *o = buf;
    for (int i = 0; i < 4; i++) {
        unsigned v = (ip >> (i * 8)) & 255;
        if (v >= 100) *o++ = '0' + v / 100;
        if (v >= 10) *o++ = '0' + v / 10 % 10;
        *o++ = '0' + v % 10;
        if (i < 3) *o++ = '.';
    }
    *o = 0;
    return buf;
}
