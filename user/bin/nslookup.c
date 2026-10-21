/* nslookup host */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

int main(int argc, char **argv) {
    struct netinfo n;
    char buf[16];
    if (argc != 2) { fprintf(stderr, "usage: nslookup host\n"); return 2; }
    netinfo(&n);
    if (n.dns) printf("Server:  %s\n", inet_ntoa_r(n.dns, buf));
    uint32_t ip;
    if (resolve(argv[1], &ip) < 0) {
        printf("** can't resolve %s: %s\n", argv[1], errno == ENOENT ? "no such host" : strerror(errno));
        return 1;
    }
    printf("Name:    %s\nAddress: %s\n", argv[1], inet_ntoa_r(ip, buf));
    return 0;
}
