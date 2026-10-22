/* nc host port - connect over TCP; stdin -> socket, socket -> stdout
 * nc -l port    - wait for one incoming connection instead */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: nc host port | nc -l port\n"); return 2; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!strcmp(argv[1], "-l")) {
        struct sockaddr_in sa = { AF_INET, htons(atoi(argv[2])), 0, { 0 } }, peer;
        char ipb[16];
        if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 1) < 0) {
            fprintf(stderr, "nc: port %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        int c = accept(fd, (struct sockaddr *)&peer, 0);
        if (c < 0) { fprintf(stderr, "nc: accept: %s\n", strerror(errno)); return 1; }
        fprintf(stderr, "nc: connection from %s:%d\n", inet_ntoa_r(peer.sin_addr, ipb), ntohs(peer.sin_port));
        close(fd);
        fd = c;
    } else {
        uint32_t ip;
        if (resolve(argv[1], &ip) < 0) { fprintf(stderr, "nc: %s: %s\n", argv[1], strerror(errno)); return 1; }
        struct sockaddr_in sa = { AF_INET, htons(atoi(argv[2])), ip, { 0 } };
        if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
            fprintf(stderr, "nc: %s:%s: %s\n", argv[1], argv[2], strerror(errno));
            return 1;
        }
    }
    char buf[1024];
    int in_open = 1, idle = 0;
    for (;;) {
        struct pollfd p[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
        int r = poll(p, in_open ? 2 : 1, in_open ? -1 : 3000);   /* after stdin EOF, drain the reply */
        if (r < 0) break;
        if (r == 0) { if (++idle >= 1) break; continue; }
        if (p[0].revents) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) break;
            write(1, buf, n);
            idle = 0;
        }
        if (in_open && p[1].revents) {
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) { in_open = 0; continue; }
            for (ssize_t o = 0; o < n; ) {
                ssize_t w = write(fd, buf + o, n - o);
                if (w <= 0) { fprintf(stderr, "nc: write: %s\n", strerror(errno)); return 1; }
                o += w;
            }
        }
    }
    close(fd);
    return 0;
}
