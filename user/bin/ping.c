/* ping [-c count] host   (default 4 probes, one per second) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <syscall.h>
#include <sys/socket.h>

#define PAYLOAD 56

int main(int argc, char **argv) {
    int count = 4, i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-c")) { count = atoi(argv[i + 1]); i += 2; }
    if (i != argc - 1 || count < 1) { fprintf(stderr, "usage: ping [-c count] host\n"); return 2; }
    const char *host = argv[i];
    uint32_t ip;
    char ips[16];
    if (resolve(host, &ip) < 0) { fprintf(stderr, "ping: %s: %s\n", host, errno == ENOENT ? "unknown host" : strerror(errno)); return 1; }
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) { fprintf(stderr, "ping: socket: %s\n", strerror(errno)); return 1; }
    struct sockaddr_in to = { AF_INET, 0, ip, { 0 } };
    uint16_t id = getpid();
    printf("PING %s (%s): %d data bytes\n", host, inet_ntoa_r(ip, ips), PAYLOAD);

    int sent = 0, got = 0;
    uint32_t tmin = ~0u, tmax = 0, tsum = 0;
    for (int seq = 1; seq <= count; seq++) {
        uint8_t pkt[8 + PAYLOAD];
        memset(pkt, 0, sizeof pkt);
        pkt[0] = 8;                                     /* echo request; the kernel fills the checksum */
        pkt[4] = id >> 8; pkt[5] = id; pkt[6] = seq >> 8; pkt[7] = seq;
        for (int k = 8; k < (int)sizeof pkt; k++) pkt[k] = k;
        uint32_t t0 = uptime_ms();
        if (sendto(fd, pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) < 0) {
            fprintf(stderr, "ping: send: %s\n", strerror(errno));
        } else {
            sent++;
            for (;;) {
                int left = 1000 - (int)(uptime_ms() - t0);
                struct pollfd p = { fd, POLLIN, 0 };
                if (left <= 0 || poll(&p, 1, left) <= 0) { printf("Request timeout for icmp_seq %d\n", seq); break; }
                uint8_t r[128];
                struct sockaddr_in from;
                socklen_t fl = sizeof from;
                int n = recvfrom(fd, r, sizeof r, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
                if (n < 8) continue;
                if (r[0] == 0 && (r[4] << 8 | r[5]) == id && (r[6] << 8 | r[7]) == seq && from.sin_addr == ip) {
                    uint32_t dt = uptime_ms() - t0;
                    got++; tsum += dt;
                    if (dt < tmin) tmin = dt;
                    if (dt > tmax) tmax = dt;
                    if (dt) printf("%d bytes from %s: icmp_seq=%d time=%u ms\n", n, ips, seq, dt);
                    else printf("%d bytes from %s: icmp_seq=%d time=<1 ms\n", n, ips, seq);
                    break;
                }
            }
        }
        if (seq < count) {
            int rest = 1000 - (int)(uptime_ms() - t0);
            if (rest > 0) usleep(rest * 1000);
        }
    }
    printf("--- %s ping statistics ---\n", host);
    printf("%d packets transmitted, %d received, %d%% packet loss\n", sent, got, sent ? (sent - got) * 100 / sent : 100);
    if (got) printf("round-trip min/avg/max = %u/%u/%u ms\n", tmin, tsum / got, tmax);
    return got ? 0 : 1;
}
